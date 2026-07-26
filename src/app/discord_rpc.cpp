// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/discord_rpc.h"

#include "app/app.h"
#include "app/context.h"
#include "app/doc.h"
#include "app/pref/preferences.h"
#include "app/site.h"
#include "app/ui_context.h"
#include "base/fs.h"
#include "doc/sprite.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <vector>

#if defined(_WIN32)
  #include <windows.h>
  #include <process.h>
  #define getpid _getpid
#else
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
#endif

namespace app {

namespace {

const char* DISCORD_CLIENT_ID = "1332560505700876358";

int64_t getCurrentTimeSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

std::string jsonEscape(const std::string& str)
{
  std::string out;
  out.reserve(str.size());
  for (char c : str) {
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else if (c == '\t')
      out += "\\t";
    else
      out += c;
  }
  return out;
}

} // anonymous namespace

DiscordRPC::DiscordRPC()
{
  m_startTimestamp = getCurrentTimeSeconds();
}

DiscordRPC::~DiscordRPC()
{
  shutdown();
}

void DiscordRPC::init()
{
  if (m_running)
    return;

  m_running = true;
  m_thread = std::thread(&DiscordRPC::backgroundLoop, this);

  if (auto ctx = UIContext::instance()) {
    ctx->add_observer(this);
    updateFromSite(ctx->activeSite());
  }
}

void DiscordRPC::shutdown()
{
  if (!m_running)
    return;

  if (auto ctx = UIContext::instance()) {
    ctx->remove_observer(this);
  }

  m_running = false;
  if (m_thread.joinable())
    m_thread.join();

  closeSocket();
}

void DiscordRPC::onActiveSiteChange(const Site& site)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  updateFromSite(site);
  m_dirty = true;
}

void DiscordRPC::updateFromSite(const Site& site)
{
  if (site.document()) {
    std::string filename = base::get_file_name(site.document()->filename());
    if (filename.empty())
      filename = "Untitled";

    m_details = "Editing " + filename;

    if (site.sprite()) {
      int w = site.sprite()->width();
      int h = site.sprite()->height();
      int frames = site.sprite()->totalFrames();
      m_state = std::to_string(w) + "x" + std::to_string(h) + " px | " + std::to_string(frames) +
                (frames == 1 ? " frame" : " frames");
    }
    else {
      m_state = "Editing sprite";
    }
  }
  else {
    m_details = "Idling";
    m_state = "No document open";
  }
}

void DiscordRPC::backgroundLoop()
{
  static int nonceCounter = 1;

  while (m_running) {
    if (!Preferences::instance().discord.enabled()) {
      if (m_connected)
        closeSocket();
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    if (!m_connected) {
      if (!connectSocket()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        continue;
      }
    }

    bool shouldSend = false;
    std::string details, state;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_dirty) {
        details = m_details;
        state = m_state;
        m_dirty = false;
        shouldSend = true;
      }
    }

    if (shouldSend && m_connected) {
      std::ostringstream json;
      json << "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" << getpid()
           << ",\"activity\":{\"details\":\"" << jsonEscape(details) << "\",\"state\":\""
           << jsonEscape(state) << "\",\"timestamps\":{\"start\":" << m_startTimestamp
           << "},\"assets\":{\"large_image\":\"aseprite\",\"large_text\":\"Aseprite Pixel Art "
              "Editor\"}}},\"nonce\":\""
           << (nonceCounter++) << "\"}";

      if (!sendData(1, json.str())) {
        closeSocket();
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}

bool DiscordRPC::connectSocket()
{
  closeSocket();

#if defined(_WIN32)
  for (int i = 0; i < 10; ++i) {
    std::string pipePath = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
    HANDLE hPipe = CreateFileA(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
      m_pipeHandle = hPipe;
      m_connected = true;
      break;
    }
  }
#else
  std::vector<std::string> searchPaths;
  for (int i = 0; i < 10; ++i) {
    searchPaths.push_back("/tmp/discord-ipc-" + std::to_string(i));
  }

  const char* tmpdir = getenv("TMPDIR");
  if (tmpdir) {
    for (int i = 0; i < 10; ++i)
      searchPaths.push_back(std::string(tmpdir) + "/discord-ipc-" + std::to_string(i));
  }

  const char* xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg) {
    for (int i = 0; i < 10; ++i)
      searchPaths.push_back(std::string(xdg) + "/discord-ipc-" + std::to_string(i));
  }

  for (const auto& path : searchPaths) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
      continue;

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      m_socketFd = fd;
      m_connected = true;
      break;
    }
    close(fd);
  }
#endif

  if (!m_connected)
    return false;

  // Send Opcode 0 (Handshake)
  std::ostringstream handshakeJson;
  handshakeJson << "{\"v\":1,\"client_id\":\"" << DISCORD_CLIENT_ID << "\"}";

  if (!sendData(0, handshakeJson.str())) {
    closeSocket();
    return false;
  }

  // Force sending initial state
  std::lock_guard<std::mutex> lock(m_mutex);
  m_dirty = true;

  return true;
}

void DiscordRPC::closeSocket()
{
  m_connected = false;
#if defined(_WIN32)
  if (m_pipeHandle && m_pipeHandle != INVALID_HANDLE_VALUE) {
    CloseHandle((HANDLE)m_pipeHandle);
    m_pipeHandle = nullptr;
  }
#else
  if (m_socketFd >= 0) {
    close(m_socketFd);
    m_socketFd = -1;
  }
#endif
}

bool DiscordRPC::sendData(int opcode, const std::string& payload)
{
  uint32_t header[2];
  header[0] = static_cast<uint32_t>(opcode);
  header[1] = static_cast<uint32_t>(payload.size());

#if defined(_WIN32)
  if (!m_pipeHandle || m_pipeHandle == INVALID_HANDLE_VALUE)
    return false;

  DWORD written = 0;
  if (!WriteFile((HANDLE)m_pipeHandle, header, sizeof(header), &written, NULL) || written != sizeof(header))
    return false;

  if (!payload.empty()) {
    if (!WriteFile((HANDLE)m_pipeHandle, payload.data(), (DWORD)payload.size(), &written, NULL) || written != payload.size())
      return false;
  }
  return true;
#else
  if (m_socketFd < 0)
    return false;

  ssize_t sent = write(m_socketFd, header, sizeof(header));
  if (sent != sizeof(header))
    return false;

  if (!payload.empty()) {
    sent = write(m_socketFd, payload.data(), payload.size());
    if (sent != (ssize_t)payload.size())
      return false;
  }
  return true;
#endif
}

} // namespace app
