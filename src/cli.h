#pragma once

#include "camera_config.h"
#include <string>

namespace picamera {

struct CliOptions {
  std::string mode;
  std::string captureFile;
  std::string outputPattern; // default derived from --format in parseArgs
  int timelapseInterval = 0;
  int timelapseCount = 1;
  // Preview mode
  uint32_t previewWidth = 320;
  uint32_t previewHeight = 240;
  uint32_t previewFps = 20;
  // Still capture during preview
  uint32_t captureWidth = 4056;
  uint32_t captureHeight = 3040;
  std::string captureFormat = "jpeg";
  std::string captureDir = ".";
  std::string capturePrefix = "capture";
  // Display
  std::string spiDevice = "/dev/spidev0.0";
  int displayRotation = 90; // Waveshare 1.44" HAT panel is mounted rotated 90°
  // Battery monitor (preview mode)
  bool enableBattery = false;
  std::string batteryI2cDevice = "/dev/i2c-1";
  uint8_t batteryI2cAddress = 0x48;
  // Wi-Fi remote control / image transfer server (optional)
  bool wifiEnabled = false;
  // Bluetooth RFCOMM serial remote control server (optional)
  bool btEnabled = false;
};

void printUsage(const char *prog);
bool parseArgs(int argc, char **argv, CliOptions &opts, CameraConfig &cfg);

} // namespace picamera
