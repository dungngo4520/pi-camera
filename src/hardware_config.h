#pragma once

#include "camera_config.h"

#include <cstdint>
#include <string>

namespace picamera {

struct HardwareConfig {
    std::string spiDevice = "/dev/spidev0.0";
    int displayRotation = 90;

    bool enableBattery = true;
    std::string batteryI2cDevice = "/dev/i2c-1";
    uint8_t batteryI2cAddress = 0x48;

    std::string captureDir = "/home/pi/captures";
    std::string capturePrefix = "capture";

    uint32_t previewWidth = 320;
    uint32_t previewHeight = 240;
    uint32_t previewFps = 20;

    uint32_t captureWidth = kMaxSensorWidth;
    uint32_t captureHeight = kMaxSensorHeight;
};

bool loadHardwareConfig(const std::string &path, HardwareConfig &cfg);

inline constexpr const char *kDefaultConfigPath = "/etc/picamera.conf";

}
