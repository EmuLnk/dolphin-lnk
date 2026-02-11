#include "Core/EmuLinkServer.h"

#include <android/log.h>
#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/Packet.hpp>

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"

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
  __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "Server Starting...");
  if (m_running)
    return true;

  if (m_socket.bind(55355, sf::IpAddress::Any) != sf::Socket::Status::Done)
  {
    __android_log_print(ANDROID_LOG_ERROR, "EmuLinkServer", "Failed to bind 55355");
    return false;
  }

  m_running = true;
  m_thread = std::thread(&EmuLinkServer::ServerLoop, this);
  __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "Server Started on 55355");
  return true;
}

void EmuLinkServer::Stop()
{
  if (!m_running)
    return;

  __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "Server Stopping...");
  m_running = false;
  m_socket.unbind();
  if (m_thread.joinable())
    m_thread.join();
}

void EmuLinkServer::ServerLoop()
{
  __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "Loop Started (V2: READ/WRITE, UDP)");
  
  u8 packet_buffer[1024 + 8]; // Header (8) + Max Data (1024)
  u8 memory_buffer[1024];
  
  while (m_running)
  {
    std::optional<sf::IpAddress> sender;
    unsigned short port;
    std::size_t received;

    if (m_socket.receive(packet_buffer, sizeof(packet_buffer), received, sender, port) == sf::Socket::Status::Done)
    {
      if (received >= 8 && sender)
      {
        u32 address, size;
        std::memcpy(&address, packet_buffer, 4);
        std::memcpy(&size, packet_buffer + 4, 4);

        u32 physical_address = address & 0x3FFFFFFF;

        if (received == 8) 
        {
          if (size <= 1024)
          {
            Core::System::GetInstance().GetMemory().CopyFromEmu(memory_buffer, physical_address, size);
            (void)m_socket.send(memory_buffer, size, *sender, port);
          }
        }
        else 
        {
          u32 data_len = static_cast<u32>(received - 8);
          u32 actual_write_size = std::min(size, data_len);
          
          if (actual_write_size > 0)
          {
            Core::System::GetInstance().GetMemory().CopyToEmu(physical_address, packet_buffer + 8, actual_write_size);
            __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "WRITE: 0x%08X, Len: %u", address, actual_write_size);
          }
        }
      }
    }
  }
  __android_log_print(ANDROID_LOG_INFO, "EmuLinkServer", "Loop Exited");
}
