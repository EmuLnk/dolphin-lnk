#pragma once

#include <atomic>
#include <thread>
#include <SFML/Network/UdpSocket.hpp>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

class EmuLinkServer
{
public:
  static EmuLinkServer& Instance();

  bool Start();
  void Stop();

private:
  EmuLinkServer() = default;
  ~EmuLinkServer();

  void ServerLoop();

  std::atomic<bool> m_running{false};
  std::thread m_thread;
  sf::UdpSocket m_socket;
};
