// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_DISCORD_RPC_H_INCLUDED
#define APP_DISCORD_RPC_H_INCLUDED
#pragma once

#include "app/context_observer.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace app {

class DiscordRPC : public ContextObserver {
public:
  DiscordRPC();
  ~DiscordRPC();

  void init();
  void shutdown();

  // ContextObserver
  void onActiveSiteChange(const Site& site) override;

private:
  void backgroundLoop();
  void updateFromSite(const Site& site);
  bool connectSocket();
  void closeSocket();
  bool sendData(int opcode, const std::string& payload);

  std::atomic<bool> m_running{false};
  std::thread m_thread;
  std::mutex m_mutex;

#if defined(_WIN32)
  void* m_pipeHandle{nullptr}; // HANDLE
#else
  int m_socketFd{-1};
#endif

  bool m_connected{false};
  int64_t m_startTimestamp{0};

  std::string m_details{"Idling"};
  std::string m_state{"No document open"};
  bool m_dirty{true};
};

} // namespace app

#endif
