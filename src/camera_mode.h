#pragma once

#include "battery.h"
#include "camera_config.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace picamera {

enum class CameraMode {
    Viewfinder,
    Review,
    Playback,
    ImageView,
    Settings,
    Splash,
};

const char *modeName(CameraMode mode);

enum class DriveMode {
    Single,
    SelfTimer,
    Bracket,
    Timelapse,
};

enum class GridType {
    Off,
    Thirds,
    Square,
};

enum class ZebraMode {
    Off,
    Threshold70,
    Threshold80,
    Threshold100,
};

enum class AspectRatio {
    Native,
    Ratio43,
    Ratio169,
    Ratio11,
};

enum class SettingsTab {
    Shooting,
    Image,
    Display,
    System,
};
constexpr int kSettingsTabCount = 4;

struct CameraSettings {
    DriveMode driveMode = DriveMode::Single;

    bool aeEnable = true;
    uint64_t shutterUs = 0;
    float analogueGain = 0;
    float digitalGain = 0;
    float exposureValue = 0;

    MeteringMode meteringMode = MeteringMode::Matrix;
    AeExposureMode aeExposureMode = AeExposureMode::Normal;
    AeConstraintMode aeConstraintMode = AeConstraintMode::Normal;

    bool antiFlicker = false;
    int flickerHz = 50;

    uint32_t timerDuration = 0;
    std::vector<float> bracketEv;
    int timelapseInterval = 5;
    int timelapseCount = 10;

    OutputFormat captureFormat = OutputFormat::JPEG;
    int jpegQuality = 90;
    int pngLevel = 6;

    AspectRatio aspectRatio = AspectRatio::Native;

    bool awbEnable = true;
    std::string awbMode = "auto";
    int wbKelvin = 5500;
    float wbRedGain = 1.0;
    float wbBlueGain = 1.0;

    float brightness = 0;
    float contrast = 1.0;
    float saturation = 1.0;
    float sharpness = 1.0;

    NoiseReductionMode noiseReduction = NoiseReductionMode::Fast;

    GridType gridType = GridType::Off;
    bool showHistogram = false;
    ZebraMode zebraMode = ZebraMode::Off;
    bool focusPeaking = false;
    int displayBrightness = 100;

    bool enableBattery = true;
    int powerSaveTimeout = 30;
};

struct OverlayState {
    CameraMode mode = CameraMode::Viewfinder;
    uint32_t captureCount = 0;
    uint32_t frameCount = 0;
    BatteryReading battery;
    bool batteryValid = false;
    CameraSettings settings;
    std::string lastCapturePath;
    bool captureInProgress = false;
    uint32_t timerRemaining = 0;
    bool meteringLocked = false;
    uint32_t shutterMs = 0;
    uint32_t iso = 0;
    std::string errorMessage;
};

void drawOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                 const OverlayState &state);

void drawHistogram(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size,
                   const uint8_t *yPlane, uint32_t w, uint32_t h,
                   uint32_t stride, size_t ySize);

void drawZebra(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
               size_t rgb565Size,
               const uint8_t *yPlane, uint32_t w, uint32_t h,
               uint32_t stride, size_t ySize, uint8_t threshold);

void drawFocusPeaking(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size,
                      const uint8_t *yPlane, uint32_t w, uint32_t h,
                      uint32_t stride, size_t ySize);

void drawSplash(uint8_t *rgb565, uint32_t fbW, uint32_t fbH);

void drawCaptureIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH);

void drawTimerCountdown(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                        uint32_t seconds);

void drawGrid(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, GridType type);

void drawReviewScreen(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size,
                      const std::string &path,
                      const uint8_t *reviewPixels, size_t reviewSize);

void drawSettingsMenu(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const CameraSettings &settings,
                      SettingsTab tab, int selectedItem);

void drawPlaybackBrowser(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         const std::vector<std::string> &files,
                         int selectedIdx, int &scrollOffset);

void drawImageView(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size,
                   const uint8_t *imageRgb565, size_t imageSize,
                   const std::string &path);

}
