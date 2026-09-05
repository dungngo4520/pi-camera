#include "bt_server.h"
#include "util.h"
#include "wifi_server.h" // settingsToJson, statusToJson, applySettingsJson

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef HAVE_BLUEZ
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace picamera {

namespace {

// --- String helpers ---

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

bool ieq(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    auto lower = [](char c) {
      return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    if (lower(a[i]) != lower(b[i]))
      return false;
  }
  return true;
}

} // namespace

// --- Pure-logic helper implementations ---

bool parseBtCommand(std::string_view line, BtCommand &out) {
  out.type = BtCommandType::Unknown;
  out.key.clear();
  out.value.clear();

  std::string_view t = trim(line);
  if (t.empty())
    return false;

  auto sp = t.find(' ');
  std::string_view verb = (sp == std::string_view::npos) ? t : t.substr(0, sp);

  static const struct {
    const char *name;
    BtCommandType type;
  } kVerbs[] = {
      {"CAPTURE", BtCommandType::Capture},   {"STATUS", BtCommandType::Status},
      {"SETTINGS", BtCommandType::Settings}, {"LIST", BtCommandType::List},
      {"QUIT", BtCommandType::Quit},
  };
  for (const auto &v : kVerbs) {
    if (ieq(verb, v.name)) {
      out.type = v.type;
      return true;
    }
  }

  if (ieq(verb, "SET")) {
    if (sp == std::string_view::npos)
      return false;
    std::string_view args = trim(t.substr(sp + 1));
    if (args.empty())
      return false;
    auto eq = args.find('=');
    if (eq == std::string_view::npos)
      return false;
    out.type = BtCommandType::Set;
    out.key = std::string(trim(args.substr(0, eq)));
    out.value = std::string(trim(args.substr(eq + 1)));
    return !out.key.empty();
  }

  return false;
}

std::string fileListJson(const std::vector<std::string> &files) {
  std::ostringstream oss;
  oss << '[';
  for (size_t i = 0; i < files.size(); ++i) {
    std::string base = files[i];
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos)
      base = base.substr(slash + 1);
    if (i > 0)
      oss << ',';
    oss << '"' << jsonEscape(base) << '"';
  }
  oss << ']';
  return oss.str();
}

std::string btSetToJson(std::string_view key, std::string_view value) {
  std::ostringstream oss;
  oss << "{\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"}";
  return oss.str();
}

// --- BtServer implementation ---

BtServer::BtServer() = default;

BtServer::~BtServer() { stop(); }

bool BtServer::start(int channel, const std::string &captureDir,
                     CameraSettings &settings, std::mutex &settingsMtx,
                     std::atomic<bool> &captureRequest,
                     const std::atomic<int> &batteryPercent,
                     const std::atomic<uint32_t> &captureCount) {
#ifdef HAVE_BLUEZ
  if (running_.load(std::memory_order_acquire))
    return false;

  channel_ = channel;
  captureDir_ = captureDir;
  settings_ = &settings;
  settingsMtx_ = &settingsMtx;
  captureRequest_ = &captureRequest;
  batteryPercent_ = &batteryPercent;
  captureCount_ = &captureCount;

  listenFd_ = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
  if (listenFd_ < 0) {
    std::cerr << "BtServer: socket() failed: " << errnoString(errno) << "\n";
    return false;
  }

  struct sockaddr_rc addr{};
  addr.rc_family = AF_BLUETOOTH;
  addr.rc_channel = static_cast<uint8_t>(channel);
  // rc_bdaddr defaults to BDADDR_ANY (00:00:00:00:00:00)
  if (::bind(listenFd_, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    std::cerr << "BtServer: bind() failed on channel " << channel << ": "
              << errnoString(errno) << "\n";
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }

  if (::listen(listenFd_, 1) < 0) {
    std::cerr << "BtServer: listen() failed: " << errnoString(errno) << "\n";
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }

  stopFlag_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread([this]() { serverLoop(); });

  std::cout << "BtServer: listening on RFCOMM channel " << channel
            << ", serving " << captureDir_ << "\n";
  return true;
#else
  (void)channel;
  (void)captureDir;
  (void)settings;
  (void)settingsMtx;
  (void)captureRequest;
  (void)batteryPercent;
  (void)captureCount;
  std::cerr << "BtServer: Bluetooth support not compiled in "
            << "(BlueZ development headers not found at build time).\n"
            << "Install libbluetooth-dev and rebuild to enable --bt.\n";
  return false;
#endif
}

void BtServer::stop() {
  if (!running_.load(std::memory_order_acquire))
    return;
  stopFlag_.store(true, std::memory_order_release);
#ifdef HAVE_BLUEZ
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
#endif
  if (thread_.joinable())
    thread_.join();
  running_.store(false, std::memory_order_release);
}

void BtServer::serverLoop() {
#ifdef HAVE_BLUEZ
  while (!stopFlag_.load(std::memory_order_acquire)) {
    struct sockaddr_rc peer{};
    socklen_t opt = sizeof(peer);
    int clientFd =
        ::accept(listenFd_, reinterpret_cast<struct sockaddr *>(&peer), &opt);
    if (clientFd < 0) {
      if (stopFlag_.load(std::memory_order_acquire))
        break;
      // Transient error — brief backoff to avoid busy-looping
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    handleConnection(clientFd);
    ::close(clientFd);
  }
#else
  // Stub — never called when BlueZ is not compiled in.
#endif
}

void BtServer::handleConnection(int clientFd) {
#ifdef HAVE_BLUEZ
  // Timeout so a slow/stuck client doesn't block the server forever.
  struct timeval tv{};
  tv.tv_sec = 10;
  tv.tv_usec = 0;
  ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  char buf[1024];
  std::string lineBuf;
  auto sendResp = [&](const std::string &s) {
    ::send(clientFd, s.data(), s.size(), 0);
  };
  auto snapshot = [&]() {
    std::lock_guard<std::mutex> lk(*settingsMtx_);
    return *settings_;
  };

  while (!stopFlag_.load(std::memory_order_acquire)) {
    ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    lineBuf.append(buf, static_cast<size_t>(n));

    for (;;) {
      auto nl = lineBuf.find('\n');
      if (nl == std::string::npos)
        break;
      std::string line = lineBuf.substr(0, nl);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lineBuf.erase(0, nl + 1);

      BtCommand cmd;
      if (!parseBtCommand(line, cmd)) {
        sendResp("ERROR: unknown command\n");
        continue;
      }

      bool quit = false;
      switch (cmd.type) {
      case BtCommandType::Capture:
        if (captureRequest_)
          captureRequest_->store(true, std::memory_order_release);
        sendResp("OK: capture triggered\n");
        break;
      case BtCommandType::Status: {
        CameraSettings snap = snapshot();
        int bat = batteryPercent_
                      ? batteryPercent_->load(std::memory_order_acquire)
                      : 0;
        uint32_t cap =
            captureCount_ ? captureCount_->load(std::memory_order_acquire) : 0;
        sendResp(statusToJson(bat, snap, cap) + "\n");
        break;
      }
      case BtCommandType::Settings:
        sendResp(settingsToJson(snapshot()) + "\n");
        break;
      case BtCommandType::Set: {
        std::string json = btSetToJson(cmd.key, cmd.value);
        {
          std::lock_guard<std::mutex> lk(*settingsMtx_);
          applySettingsJson(json, *settings_);
        }
        sendResp("OK: " + cmd.key + " set\n");
        break;
      }
      case BtCommandType::List: {
        std::vector<std::string> files;
        try {
          namespace fs = std::filesystem;
          if (fs::exists(captureDir_)) {
            for (const auto &entry : fs::directory_iterator(captureDir_)) {
              if (entry.is_regular_file())
                files.push_back(entry.path().string());
            }
            std::sort(files.begin(), files.end());
          }
        } catch (const std::exception &e) {
          std::cerr << "BtServer: directory listing failed: " << e.what()
                    << "\n";
        }
        sendResp(fileListJson(files) + "\n");
        break;
      }
      case BtCommandType::Quit:
        quit = true;
        break;
      case BtCommandType::Unknown:
        break;
      }
      if (quit)
        return;
    }
  }
#else
  (void)clientFd;
#endif
}

} // namespace picamera
