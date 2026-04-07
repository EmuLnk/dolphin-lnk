#include "Core/EmuLinkServer.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <mbedtls/md5.h>
#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Packet.hpp>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#ifdef USE_RETRO_ACHIEVEMENTS
#include "Core/AchievementManager.h"
#endif
#include "Core/ConfigManager.h"
#include "Core/HW/DVD/DVDThread.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"

#ifdef USE_RETRO_ACHIEVEMENTS
#include <rcheevos/include/rc_client.h>
#endif

EmuLinkServer& EmuLinkServer::Instance()
{
  static EmuLinkServer instance;
  return instance;
}

EmuLinkServer::~EmuLinkServer()
{
  Stop();
}

bool EmuLinkServer::Start()
{
  NOTICE_LOG_FMT(CORE, "{}", "Server Starting...");
  if (m_running)
    return true;

  if (m_socket.bind(55355, sf::IpAddress::Any) != sf::Socket::Status::Done)
  {
    NOTICE_LOG_FMT(CORE, "{}", "Failed to bind 55355");
    return false;
  }

  m_running = true;
  m_thread = std::thread(&EmuLinkServer::ServerLoop, this);
  NOTICE_LOG_FMT(CORE, "{}", "Server Started on 55355");
  return true;
}

void EmuLinkServer::Stop()
{
  if (!m_running)
    return;

  NOTICE_LOG_FMT(CORE, "{}", "Server Stopping...");
  m_running = false;
  m_game_hash.clear();
  m_socket.unbind();
  if (m_thread.joinable())
    m_thread.join();
}

void EmuLinkServer::ComputeBootDOLHash()
{
  m_game_hash.clear();

  auto& system = Core::System::GetInstance();
  auto& dvd_thread = system.GetDVDThread();
  const auto* volume = dvd_thread.GetVolume();
  if (!volume)
    return;

  const auto partition = volume->GetGamePartition();
  const auto dol_offset = DiscIO::GetBootDOLOffset(*volume, partition);
  if (!dol_offset)
    return;

  const auto dol_size = DiscIO::GetBootDOLSize(*volume, partition, *dol_offset);
  if (!dol_size || *dol_size == 0)
    return;

  std::vector<u8> dol_data(*dol_size);
  if (!volume->Read(*dol_offset, *dol_size, dol_data.data(), partition))
    return;

  std::array<u8, 16> digest{};
  mbedtls_md5_context ctx;
  mbedtls_md5_init(&ctx);
  mbedtls_md5_starts_ret(&ctx);
  mbedtls_md5_update_ret(&ctx, dol_data.data(), dol_data.size());
  mbedtls_md5_finish_ret(&ctx, digest.data());
  mbedtls_md5_free(&ctx);

  m_game_hash = Common::BytesToHexString(digest);

  NOTICE_LOG_FMT(CORE, "boot.dol hash: {}",
                       m_game_hash.c_str());
}

void EmuLinkServer::ServerLoop()
{
  NOTICE_LOG_FMT(CORE, "{}", "Loop Started (EMLKV2, UDP)");

  static constexpr u32 MAX_PAYLOAD = 4096;
  u8 packet_buffer[MAX_PAYLOAD + 8]; // Header (8) + Max Data
  u8 memory_buffer[MAX_PAYLOAD];

  while (m_running)
  {
    std::optional<sf::IpAddress> sender;
    unsigned short port;
    std::size_t received;

    if (m_socket.receive(packet_buffer, sizeof(packet_buffer), received, sender, port) ==
        sf::Socket::Status::Done)
    {
      // EMLKV2 handshake: 6-byte "EMLKV2" magic -> respond with JSON
      if (received == 6 && sender &&
          std::memcmp(packet_buffer, "EMLKV2", 6) == 0)
      {
        if (m_game_hash.empty())
          ComputeBootDOLHash();

        const std::string game_id = SConfig::GetInstance().GetGameID();
        const u64 title_id = SConfig::GetInstance().GetTitleID();
        const char* platform = (title_id != 0) ? "WII" : "GCN";

        char json[512];
        std::snprintf(json, sizeof(json),
          "{\"emulator\":\"dolphin\",\"game_id\":\"%s\","
          "\"game_hash\":\"%s\",\"platform\":\"%s\"}",
          game_id.c_str(), m_game_hash.c_str(), platform);

        (void)m_socket.send(json, std::strlen(json), *sender, port);
        continue;
      }

      // Batch read: "EL" magic (0x45, 0x4C) + count + entries
      if (received >= 4 && sender &&
          packet_buffer[0] == 0x45 && packet_buffer[1] == 0x4C)
      {
        uint16_t count;
        std::memcpy(&count, packet_buffer + 2, 2);
        if (count > 0 && count <= 256 && received >= 4 + count * 8)
        {
          u8 response[65000];
          int resp_off = 4;
          uint16_t actual = 0;
          response[0] = 0x45;
          response[1] = 0x4C;

          auto& memory = Core::System::GetInstance().GetMemory();

          for (uint16_t i = 0; i < count; i++)
          {
            u32 addr, size;
            std::memcpy(&addr, packet_buffer + 4 + i * 8, 4);
            std::memcpy(&size, packet_buffer + 4 + i * 8 + 4, 4);
            if (size > MAX_PAYLOAD) size = MAX_PAYLOAD;

            u32 phys = addr & 0x3FFFFFFF;
            bool valid = false;
            if (phys + size > phys)
            {
              if (phys + size <= memory.GetRamSizeReal())
                valid = true;
              else if (memory.GetEXRAM() && (phys >> 28) == 0x1 &&
                       ((phys & 0x0FFFFFFF) + size) <= memory.GetExRamSizeReal())
                valid = true;
            }

            if (valid && resp_off + 2 + (int)size <= (int)sizeof(response))
            {
              u16 len16 = static_cast<u16>(size);
              std::memcpy(response + resp_off, &len16, 2);
              resp_off += 2;
              memory.CopyFromEmu(response + resp_off, phys, size);
              resp_off += size;
              actual++;
            }
            else if (resp_off + 2 <= (int)sizeof(response))
            {
              response[resp_off++] = 0;
              response[resp_off++] = 0;
              actual++;
            }
            else
              break;
          }

          response[2] = actual & 0xFF;
          response[3] = (actual >> 8) & 0xFF;

          (void)m_socket.send(response, resp_off, *sender, port);
          continue;
        }
      }

      if (received >= 8 && sender)
      {
        u32 address, size;
        std::memcpy(&address, packet_buffer, 4);
        std::memcpy(&size, packet_buffer + 4, 4);

        u32 physical_address = address & 0x3FFFFFFF;

        // Bounds-check to avoid PanicAlertFmt in GetSpanForAddress
        auto& memory = Core::System::GetInstance().GetMemory();
        bool address_valid = false;

        if (physical_address + size <= physical_address)
        {
        }
        else if (physical_address + size <= memory.GetRamSizeReal())
        {
          address_valid = true;  // MEM1
        }
        else if (memory.GetEXRAM() && (physical_address >> 28) == 0x1 &&
                 ((physical_address & 0x0FFFFFFF) + size) <= memory.GetExRamSizeReal())
        {
          address_valid = true;  // EXRAM (MEM2, Wii)
        }

        if (!address_valid)
        {
          // Respond with zeros instead of timing out
          if (received == 8 && size <= MAX_PAYLOAD)
          {
            std::memset(memory_buffer, 0, size);
            (void)m_socket.send(memory_buffer, size, *sender, port);
          }
          continue;
        }

        if (received == 8)
        {
          if (size <= MAX_PAYLOAD)
          {
            memory.CopyFromEmu(memory_buffer, physical_address, size);
            (void)m_socket.send(memory_buffer, size, *sender, port);
          }
        }
        else
        {
          u32 data_len = static_cast<u32>(received - 8);
          u32 actual_write_size = std::min(size, data_len);

          if (actual_write_size > 0)
          {
            memory.CopyToEmu(physical_address, packet_buffer + 8, actual_write_size);
            // RA rules: memory tampering disqualifies hardcore
#ifdef USE_RETRO_ACHIEVEMENTS
            if (AchievementManager::GetInstance().IsHardcoreModeActive()) {
              rc_client_set_hardcore_enabled(AchievementManager::GetInstance().GetClient(), 0);
              INFO_LOG_FMT(CORE, "{}",
                                  "Hardcore mode disabled by memory write.");
            }
#endif
          }
        }
      }
    }
  }
  NOTICE_LOG_FMT(CORE, "{}", "Loop Exited");
}
