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
  Continuous,
  Bulb,
};

enum class GridType {
  Off,
  Thirds,
  Square,
  Diagonal,
  GoldenRatio,
};

enum class FileNamingMode {
  Timestamp = 0,
  Sequential = 1,
};

enum class ZebraMode {
  Off,
  Threshold70,
  Threshold80,
  Threshold100,
};

enum class SettingsTab {
  Shooting,
  Image,
  Display,
  System,
};
constexpr int kSettingsTabCount = 4;

enum class ExposureMode {
  Program = 0,
  Shutter = 1,
  Manual = 2,
  Auto = 3,
};

enum class BracketType {
  AE = 0,
  WB = 1,
  ISO = 2,
};

enum class PictureStyle {
  Standard = 0,
  Vivid = 1,
  Natural = 2,
  Monochrome = 3,
  Portrait = 4,
  Landscape = 5,
  Sepia = 6,
  Cool = 7,
  Warm = 8,
  Film = 9,
  HDR = 10,
};

enum class ColorSpace {
  SRGB = 0,
  AdobeRGB = 1,
};

enum class CustomMode {
  Auto = 0,
  C1 = 1,
  C2 = 2,
  C3 = 3,
};

struct CameraSettings {
  DriveMode driveMode = DriveMode::Single;

  ExposureMode exposureMode = ExposureMode::Program;
  bool aeEnable = true;
  uint64_t shutterUs = 0;
  float analogueGain = 0;
  float digitalGain = 0;
  float exposureValue = 0;
  // Auto-ISO range (min/max ISO). libcamera does not expose
  // AeAnalogueGainMin/Max controls, so these cannot constrain the AE
  // algorithm's auto-gain selection. They ARE applied in Manual exposure
  // mode: when the user sets a manual analogueGain, it is clamped to
  // [isoMin/100.0, isoMax/100.0] in applyControls()/applyCameraControls()
  // (ISO = gain * 100). See clampGainToIsoRange() in camera_config.h.
  int isoMin = 100;
  int isoMax = 3200;

  MeteringMode meteringMode = MeteringMode::Matrix;
  AeExposureMode aeExposureMode = AeExposureMode::Normal;
  AeConstraintMode aeConstraintMode = AeConstraintMode::Normal;

  bool antiFlicker = false;
  int flickerHz = 50;

  uint32_t timerDuration = 0;
  std::vector<float> bracketEv;
  BracketType bracketType = BracketType::AE;
  int timelapseInterval = 5;
  int timelapseCount = 10;

  OutputFormat captureFormat = OutputFormat::JPEG;
  int jpegQuality = 90;
  int pngLevel = 6;
  ImageSize imageSize = ImageSize::Large;

  AspectRatio aspectRatio = AspectRatio::Native;

  bool awbEnable = true;
  std::string awbMode = "auto";
  int wbKelvin = 5500;
  float wbRedGain = 1.0;
  float wbBlueGain = 1.0;
  // One-touch custom WB: armed by WBSET, consumed by the viewfinder to
  // update wbRedGain/wbBlueGain from the live frame's chroma. Transient.
  bool wbMeasurePending = false;

  PictureStyle pictureStyle = PictureStyle::Standard;
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
  int focusMagnify = 0; // 0=off, 2=2x, 4=4x

  bool enableBattery = true;
  int powerSaveTimeout = 30;

  FileNamingMode fileNamingMode = FileNamingMode::Timestamp;
  bool useDateSubfolders = false;

  // --- New feature settings ---
  ColorSpace colorSpace = ColorSpace::SRGB;
  float wbGmShift = 0.0f; // green-magenta WB shift: -9 (green) to +9 (magenta)
  uint64_t minShutterUs = 0; // minimum shutter speed for auto ISO (0 = auto)
  bool longExposureNr = false; // dark frame subtraction for exposures > 1s
  bool silentShutter = false; // suppress capture sounds/indicators
  bool airplaneMode = false; // disable Wi-Fi/BT at runtime
  bool rotateTall = false; // rotate portrait images in playback
  bool nightMode = false; // boost viewfinder brightness for low-light
  std::string copyright; // copyright string for EXIF/DNG embedding
  bool grainEffect = false; // film grain overlay on JPEG encode
  bool hdrMerge = false; // merge AE bracket frames into a single HDR JPEG
  CustomMode customMode = CustomMode::Auto; // C1/C2/C3 custom shooting modes
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
  // Bulb mode: seconds since exposure started (0 = inactive).
  uint32_t bulbSeconds = 0;
  std::string errorMessage;
  bool timelapseRunning = false;
  int focusMagnify = 0;
  bool wifiActive = false;
  bool btActive = false;
};

void drawOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                 const OverlayState &state);

void drawHistogram(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size, const uint8_t *yPlane, uint32_t w,
                   uint32_t h, uint32_t stride, size_t ySize);

void drawZebra(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, size_t rgb565Size,
               const uint8_t *yPlane, uint32_t w, uint32_t h, uint32_t stride,
               size_t ySize, uint8_t threshold);

void drawFocusPeaking(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size, const uint8_t *yPlane, uint32_t w,
                      uint32_t h, uint32_t stride, size_t ySize);

uint8_t zebraThreshold(ZebraMode mode);

// One-touch custom WB: estimate R/B gains from an NV12 UV (CbCr) plane so a
// neutral subject averages to equal RGB. NV12 chroma is 4:2:0. Out gains
// clamped to [0.1, 8.0]. Pure logic (unit-testable on x86).
bool computeWbGainsFromNv12(const uint8_t *uv, uint32_t w, uint32_t h,
                            size_t uvSize, float &outRed, float &outBlue);

void drawBulbTimer(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   uint32_t seconds);

void drawSplash(uint8_t *rgb565, uint32_t fbW, uint32_t fbH);

void drawCaptureIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH);

void drawTimerCountdown(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                        uint32_t seconds);

void drawGrid(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, GridType type);

void drawAspectRatioMask(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         AspectRatio ratio);

void drawFocusMagnifyIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                               int magnify);

void drawImageViewHistogramAndBlinkies(uint8_t *rgb565, uint32_t fbW,
                                       uint32_t fbH, size_t rgb565Size);

void drawProtectionIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH);

void drawReviewScreen(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size, const std::string &path,
                      const uint8_t *reviewPixels, size_t reviewSize);

void drawSettingsMenu(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const CameraSettings &settings, SettingsTab tab,
                      int selectedItem);

void drawPlaybackBrowser(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         const std::vector<std::string> &files, int selectedIdx,
                         int &scrollOffset);

void drawImageView(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size, const uint8_t *imageRgb565,
                   size_t imageSize, const std::string &path);

// Zoomed image view (1x/2x/4x) with pan. Source is imageW x imageH RGB565;
// visible region (panX, panY, fbW/zoom, fbH/zoom) is scaled to fill the FB.
void drawImageViewZoomed(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         size_t rgb565Size, const uint8_t *imageRgb565,
                         size_t imageSize, uint32_t imageW, uint32_t imageH,
                         int zoom, int panX, int panY, const std::string &path);

} // namespace picamera
