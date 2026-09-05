#pragma once

#include "camera_mode.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace picamera {

// --- Pure-logic helpers (unit-testable, no socket dependency) ---

enum class BtCommandType {
  Unknown,
  Capture,
  Status,
  Settings,
  Set,
  List,
  Quit,
};

struct BtCommand {
  BtCommandType type = BtCommandType::Unknown;
  std::string key;   // for SET
  std::string value; // for SET
};

// Parse a single line of the Bluetooth serial protocol.
// Matches the command verb case-insensitively; "SET key=value" splits
// on the first '='. Returns false for unknown/empty input.
bool parseBtCommand(std::string_view line, BtCommand &out);

// JSON array of file basenames for the LIST command.
std::string fileListJson(const std::vector<std::string> &files);

// Build {"key":"value"} for the SET command. The value is always quoted
// as a string — applySettingsJson() handles quoted numeric values, so
// "5000" is parsed as the number 5000 by jsonNumValue.
std::string btSetToJson(std::string_view key, std::string_view value);

// --- BtServer: minimal Bluetooth RFCOMM serial server for remote control ---
// Runs in a background thread; handles one connection at a time.
// When BlueZ is not available at build time (HAVE_BLUEZ not defined),
// a stub is compiled that always returns false from start().
class BtServer {
public:
  BtServer();
  ~BtServer();
  BtServer(const BtServer &) = delete;
  BtServer &operator=(const BtServer &) = delete;

  bool start(int channel, const std::string &captureDir,
             CameraSettings &settings, std::mutex &settingsMtx,
             std::atomic<bool> &captureRequest,
             const std::atomic<int> &batteryPercent,
             const std::atomic<uint32_t> &captureCount);

  void stop();

  bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
  void serverLoop();
  void handleConnection(int clientFd);

  std::atomic<bool> running_{false};
  std::atomic<bool> stopFlag_{false};
  int channel_ = 0;
  int listenFd_ = -1;
  std::string captureDir_;
  std::thread thread_;
  CameraSettings *settings_ = nullptr;
  std::mutex *settingsMtx_ = nullptr;
  std::atomic<bool> *captureRequest_ = nullptr;
  const std::atomic<int> *batteryPercent_ = nullptr;
  const std::atomic<uint32_t> *captureCount_ = nullptr;
};

} // namespace picamera
