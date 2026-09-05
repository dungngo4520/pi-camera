#pragma once

#include "camera_config.h"

#include <cstdint>
#include <string>

namespace picamera {

struct HardwareConfig {
  bool loaded = false;
  std::string spiDevice = "/dev/spidev0.0";
  int displayRotation = 90;

  bool enableBattery = true;
  std::string batteryI2cDevice = "/dev/i2c-1";
  uint8_t batteryI2cAddress = 0x48;

  std::string captureDir = "/home/pi/captures";
  std::string capturePrefix = "capture";

  uint32_t previewWidth = 320;
  uint32_t previewHeight = 240;
  uint32_t maxFps = 20;

  uint32_t captureWidth = kMaxSensorWidth;
  uint32_t captureHeight = kMaxSensorHeight;

  bool wifiEnabled = false;
  bool btEnabled = false;
};

bool loadHardwareConfig(const std::string &path, HardwareConfig &cfg);

// Convenience overload: returns a HardwareConfig with `loaded=true` if parsed.
HardwareConfig loadHardwareConfig(const std::string &path);

inline constexpr const char *kDefaultConfigPath = "/etc/picamera.conf";

} // namespace picamera
