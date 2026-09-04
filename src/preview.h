#pragma once

#include <cstdint>
#include <string>
#include "camera_config.h"
#include "display.h"
#include "battery.h"

namespace picamera {

struct CliOptions; // forward declaration — defined in cli.h

struct PreviewConfig {
    // Display
    DisplayConfig displayCfg;
    // Camera viewfinder
    uint32_t previewWidth = 320;
    uint32_t previewHeight = 240;
    uint32_t maxFps = 20;
    // Still capture (on button press)
    uint32_t captureWidth = 4056;
    uint32_t captureHeight = 3040;
    std::string captureFormat = "jpeg";
    std::string captureDir = ".";
    std::string capturePrefix = "capture";
    // Camera settings (exposure/gain/AWB) applied to both viewfinder
    // and still captures. Defaults = auto everything.
    CameraConfig cameraCfg;
    // Battery monitor (optional — set enableBattery=true to show
    // battery icon + % overlay on the preview display)
    bool enableBattery = false;
    BatteryConfig batteryCfg;
};

// Run a live preview loop: stream low-res frames to the SPI display,
// and capture full-res images when the shutter button is pressed.
// Blocks until SIGINT/SIGTERM. Returns true on clean shutdown.
bool runPreview(PreviewConfig &pcfg);

// Build a PreviewConfig from parsed CLI options. Centralizes the field
// mapping so main.cpp doesn't hand-copy ~12 fields (and a new PreviewConfig
// field can't be silently left un-initialized).
PreviewConfig makePreviewConfig(const CliOptions &opts, const CameraConfig &cfg);

} // namespace picamera
