#include "settings_menu.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace picamera {

namespace {

constexpr float kFineStep = 0.1f;
constexpr int kMinJpegQuality = 10;
constexpr float kSharpnessStep = 0.5f;
constexpr float kMaxSharpness = 16.0f;
constexpr int kDisplayBrightnessStep = 10;
constexpr int kMinDisplayBrightness = 10;
constexpr int kMaxDisplayBrightness = 100;
constexpr int kMinKelvin = 2500;
constexpr int kMaxKelvin = 10000;
constexpr int kFlicker50HzPeriodUs = 10000;
constexpr int kFlicker60HzPeriodUs = 8333;

constexpr int kDriveModeCount = 7;
constexpr int kMeteringModeCount = 3;
constexpr int kAeExposureModeCount = 3;
constexpr int kAeConstraintModeCount = 3;
constexpr int kNoiseReductionModeCount = 4;
constexpr int kAspectRatioCount = 4;
constexpr int kGridTypeCount = 5;
constexpr int kZebraModeCount = 4;
constexpr int kImageSizeCount = 3;
constexpr int kFileNamingCount = 2;
constexpr int kExposureModeCount = 4;
constexpr int kBracketTypeCount = 3;
constexpr int kPictureStyleCount = 11;
constexpr int kColorSpaceCount = 2;
constexpr int kCustomModeCount = 4;
constexpr int kVideoResolutionCount = 4;
constexpr int kVideoCodecCount = 3;
constexpr int kSensorModeCount = 5;
constexpr int kVideoFpsSteps[] = {10, 24, 30, 50, 60};
constexpr int kVideoFpsStepCount = 5;
constexpr int kVideoBitrateSteps[] = {1, 5, 10, 20};
constexpr int kVideoBitrateStepCount = 4;

template <typename E> E cycleEnum(E value, int direction, int count) {
  return static_cast<E>((static_cast<int>(value) + direction + count) % count);
}

constexpr uint64_t kShutterSteps[] = {
    0,       60,       125,      250,      500,       1000,
    2000,    4000,     8000,     16667,    33333,     66667,
    125000,  250000,   500000,   1000000,  2000000,   4000000,
    8000000, 15000000, 30000000, 60000000, 120000000, 300000000,
};
constexpr int kShutterStepCount = static_cast<int>(std::size(kShutterSteps));

constexpr float kIsoSteps[] = {
    0, 100, 200, 400, 800, 1600, 3200, 6400,
};
constexpr int kIsoStepCount = static_cast<int>(std::size(kIsoSteps));

constexpr float kEvSteps[] = {
    -3.0f, -2.67f, -2.33f, -2.0f, -1.67f, -1.33f, -1.0f, -0.67f, -0.33f, 0.0f,
    0.33f, 0.67f,  1.0f,   1.33f, 1.67f,  2.0f,   2.33f, 2.67f,  3.0f,
};
constexpr int kEvStepCount = static_cast<int>(std::size(kEvSteps));

std::string formatShutter(uint64_t us) {
  if (us == 0)
    return "AUTO";
  if (us < kMicrosPerMillis)
    return std::to_string(us) + "us";
  if (us < kMicrosPerSec) {
    uint64_t denom = kMicrosPerSec / us;
    if (kMicrosPerSec % us > us / 2)
      ++denom;
    if (denom <= 1)
      return "1\"";
    return "1/" + std::to_string(denom);
  }
  uint64_t secs = us / kMicrosPerSec;
  if (secs < 60)
    return std::to_string(secs) + "\"";
  uint64_t mins = secs / 60;
  return std::to_string(mins) + "'";
}

std::string fmt1(float v) {
  int tenths = static_cast<int>(std::round(v * 10.0f));
  int sign = tenths < 0 ? -1 : 1;
  tenths = std::abs(tenths);
  std::string s;
  if (sign < 0)
    s += '-';
  s += std::to_string(tenths / 10);
  s += '.';
  s += std::to_string(tenths % 10);
  return s;
}

std::string fmtEv(float ev) {
  if (ev == 0)
    return "0";
  std::string s = (ev > 0) ? "+" : "-";
  s += fmt1(std::abs(ev));
  return s;
}

template <typename T> int nearestIndex(const T *steps, int count, T value) {
  for (int i = 0; i < count; ++i)
    if (steps[i] == value)
      return i;
  int best = 0;
  auto bestDiff = std::numeric_limits<T>::max();
  for (int i = 0; i < count; ++i) {
    auto diff = (steps[i] > value) ? steps[i] - value : value - steps[i];
    if (diff < bestDiff) {
      bestDiff = diff;
      best = i;
    }
  }
  return best;
}

int findShutterIdx(uint64_t us) {
  return nearestIndex(kShutterSteps, kShutterStepCount, us);
}

int findIsoIdx(float gain) {
  return nearestIndex(kIsoSteps, kIsoStepCount, gain * kIsoScaleFactor);
}

int findEvIdx(float ev) { return nearestIndex(kEvSteps, kEvStepCount, ev); }

const char *driveModeLabel(DriveMode d) {
  switch (d) {
  case DriveMode::Single:
    return "SINGLE";
  case DriveMode::SelfTimer:
    return "SELF";
  case DriveMode::Bracket:
    return "BRACKET";
  case DriveMode::Timelapse:
    return "LAPSE";
  case DriveMode::Continuous:
    return "CONT";
  case DriveMode::Bulb:
    return "BULB";
  case DriveMode::Video:
    return "VIDEO";
  }
  return "??";
}

const char *meteringLabel(MeteringMode m) {
  switch (m) {
  case MeteringMode::Matrix:
    return "MATRIX";
  case MeteringMode::Centre:
    return "CENTRE";
  case MeteringMode::Spot:
    return "SPOT";
  }
  return "??";
}

const char *aeExposureLabel(AeExposureMode m) {
  switch (m) {
  case AeExposureMode::Normal:
    return "NORMAL";
  case AeExposureMode::Short:
    return "SHORT";
  case AeExposureMode::Long:
    return "LONG";
  }
  return "??";
}

const char *aeConstraintLabel(AeConstraintMode m) {
  switch (m) {
  case AeConstraintMode::Normal:
    return "NORMAL";
  case AeConstraintMode::Highlight:
    return "HIGHLIGHT";
  case AeConstraintMode::Shadows:
    return "SHADOWS";
  }
  return "??";
}

const char *formatLabel(OutputFormat fmt) {
  switch (fmt) {
  case OutputFormat::JPEG:
    return "JPEG";
  case OutputFormat::PNG:
    return "PNG";
  case OutputFormat::DNG:
    return "DNG";
  case OutputFormat::RAW_NV12:
    return "RAW";
  case OutputFormat::PPM:
    return "PPM";
  case OutputFormat::RawJpeg:
    return "JPG+RAW";
  case OutputFormat::DngJpeg:
    return "DNG+JPG";
  }
  return "??";
}

const char *gridLabel(GridType g) {
  switch (g) {
  case GridType::Off:
    return "OFF";
  case GridType::Thirds:
    return "3RD";
  case GridType::Square:
    return "SQ";
  case GridType::Diagonal:
    return "DIAG";
  case GridType::GoldenRatio:
    return "GOLD";
  }
  return "??";
}

const char *zebraLabel(ZebraMode z) {
  switch (z) {
  case ZebraMode::Off:
    return "OFF";
  case ZebraMode::Threshold70:
    return "70%";
  case ZebraMode::Threshold80:
    return "80%";
  case ZebraMode::Threshold100:
    return "100%";
  }
  return "??";
}

const char *aspectLabel(AspectRatio a) {
  switch (a) {
  case AspectRatio::Native:
    return "3:2";
  case AspectRatio::Ratio43:
    return "4:3";
  case AspectRatio::Ratio169:
    return "16:9";
  case AspectRatio::Ratio11:
    return "1:1";
  }
  return "??";
}

const char *imageSizeLabel(ImageSize s) {
  switch (s) {
  case ImageSize::Large:
    return "L";
  case ImageSize::Medium:
    return "M";
  case ImageSize::Small:
    return "S";
  }
  return "??";
}

const char *fileNamingLabel(FileNamingMode m) {
  switch (m) {
  case FileNamingMode::Timestamp:
    return "TIME";
  case FileNamingMode::Sequential:
    return "SEQ";
  }
  return "??";
}

const char *nrLabel(NoiseReductionMode n) {
  switch (n) {
  case NoiseReductionMode::Off:
    return "OFF";
  case NoiseReductionMode::Fast:
    return "FAST";
  case NoiseReductionMode::HighQuality:
    return "HQ";
  case NoiseReductionMode::Minimal:
    return "MIN";
  }
  return "??";
}

const char *exposureModeLabel(ExposureMode m) {
  switch (m) {
  case ExposureMode::Program:
    return "P";
  case ExposureMode::Shutter:
    return "S";
  case ExposureMode::Manual:
    return "M";
  case ExposureMode::Auto:
    return "AUTO";
  }
  return "??";
}

const char *bracketTypeLabel(BracketType b) {
  switch (b) {
  case BracketType::AE:
    return "AE";
  case BracketType::WB:
    return "WB";
  case BracketType::ISO:
    return "ISO";
  }
  return "??";
}

const char *pictureStyleLabel(PictureStyle p) {
  switch (p) {
  case PictureStyle::Standard:
    return "STD";
  case PictureStyle::Vivid:
    return "VIVID";
  case PictureStyle::Natural:
    return "NAT";
  case PictureStyle::Monochrome:
    return "MONO";
  case PictureStyle::Portrait:
    return "PORTR";
  case PictureStyle::Landscape:
    return "LAND";
  case PictureStyle::Sepia:
    return "SEPIA";
  case PictureStyle::Cool:
    return "COOL";
  case PictureStyle::Warm:
    return "WARM";
  case PictureStyle::Film:
    return "FILM";
  case PictureStyle::HDR:
    return "HDR";
  }
  return "??";
}

const char *focusMagnifyLabel(int m) {
  switch (m) {
  case 0:
    return "OFF";
  case 2:
    return "2X";
  case 4:
    return "4X";
  default:
    return "??";
  }
}

const char *colorSpaceLabel(ColorSpace c) {
  switch (c) {
  case ColorSpace::SRGB:
    return "sRGB";
  case ColorSpace::AdobeRGB:
    return "Adobe";
  }
  return "??";
}

const char *customModeLabel(CustomMode c) {
  switch (c) {
  case CustomMode::Auto:
    return "AUTO";
  case CustomMode::C1:
    return "C1";
  case CustomMode::C2:
    return "C2";
  case CustomMode::C3:
    return "C3";
  }
  return "??";
}

std::string formatMinShutter(uint64_t us) {
  if (us == 0)
    return "AUTO";
  return formatShutter(us);
}

void adjustDrive(CameraSettings &s, int direction) {
  s.driveMode = cycleEnum(s.driveMode, direction, kDriveModeCount);
}

void adjustShutter(CameraSettings &s, int direction) {
  int idx = findShutterIdx(s.shutterUs);
  int newIdx = idx + direction;
  if (newIdx >= 0 && newIdx < kShutterStepCount)
    s.shutterUs = kShutterSteps[newIdx];
  s.aeEnable = (s.shutterUs == 0 && s.analogueGain == 0.0f);
}

void adjustIso(CameraSettings &s, int direction) {
  int idx = findIsoIdx(s.analogueGain);
  int newIdx = idx + direction;
  if (newIdx >= 0 && newIdx < kIsoStepCount)
    s.analogueGain = kIsoSteps[newIdx] / kIsoScaleFactor;
  s.aeEnable = (s.shutterUs == 0 && s.analogueGain == 0.0f);
}

// Auto-ISO bounds. No "0" sentinel (unlike kIsoSteps) — min/max are real
// values.
constexpr int kIsoBoundValues[] = {100, 200, 400, 800, 1600, 3200, 6400};

int cycleIsoBound(int current, int direction) {
  constexpr int n = static_cast<int>(std::size(kIsoBoundValues));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (current == kIsoBoundValues[i]) {
      idx = i;
      break;
    }
  return kIsoBoundValues[std::clamp(idx + direction, 0, n - 1)];
}

void adjustIsoMin(CameraSettings &s, int direction) {
  s.isoMin = cycleIsoBound(s.isoMin, direction);
  s.isoMax = std::max(s.isoMin, s.isoMax);
}

void adjustIsoMax(CameraSettings &s, int direction) {
  s.isoMax = cycleIsoBound(s.isoMax, direction);
  s.isoMin = std::min(s.isoMax, s.isoMin);
}

void adjustEv(CameraSettings &s, int direction) {
  int idx = findEvIdx(s.exposureValue);
  int newIdx = idx + direction;
  if (newIdx >= 0 && newIdx < kEvStepCount)
    s.exposureValue = kEvSteps[newIdx];
}

void adjustMeter(CameraSettings &s, int direction) {
  s.meteringMode = cycleEnum(s.meteringMode, direction, kMeteringModeCount);
}

void adjustAeMode(CameraSettings &s, int direction) {
  s.aeExposureMode =
      cycleEnum(s.aeExposureMode, direction, kAeExposureModeCount);
}

void adjustAeConst(CameraSettings &s, int direction) {
  s.aeConstraintMode =
      cycleEnum(s.aeConstraintMode, direction, kAeConstraintModeCount);
}

void adjustFlicker(CameraSettings &s, int direction) {
  enum FlickerState : int { Off, Hz50, Hz60 };
  FlickerState state = s.antiFlicker ? (s.flickerHz == 60 ? Hz60 : Hz50) : Off;
  if (direction > 0)
    state = static_cast<FlickerState>((state + 1) % 3);
  else if (state > Off)
    state = static_cast<FlickerState>(state - 1);
  if (state == Off)
    s.antiFlicker = false;
  else {
    s.antiFlicker = true;
    s.flickerHz = (state == Hz50) ? 50 : 60;
  }
}

void adjustTimer(CameraSettings &s, int direction) {
  static constexpr int kTimerPresets[] = {0, 2, 5, 10};
  constexpr int n = static_cast<int>(std::size(kTimerPresets));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (static_cast<int>(s.timerDuration) == kTimerPresets[i]) {
      idx = i;
      break;
    }
  s.timerDuration = kTimerPresets[(idx + direction + n) % n];
}

void adjustBracket(CameraSettings &s, int direction) {
  if (s.bracketEv.empty()) {
    if (direction > 0) {
      s.bracketEv = {-0.5f, 0.0f, 0.5f};
    } else {
      s.bracketEv = {0.0f};
    }
  } else {
    float maxEv = 0;
    for (float ev : s.bracketEv)
      maxEv = std::max(maxEv, std::abs(ev));
    if (direction > 0) {
      if (maxEv < 3.0f) {
        float newEv = maxEv + 0.5f;
        s.bracketEv = {-newEv, 0.0f, newEv};
      }
    } else {
      if (maxEv <= 0.5f) {
        s.bracketEv.clear();
      } else {
        float newEv = maxEv - 0.5f;
        s.bracketEv = {-newEv, 0.0f, newEv};
      }
    }
  }
}

void adjustInterval(CameraSettings &s, int direction) {
  s.timelapseInterval = std::clamp(s.timelapseInterval + direction, 1, 3600);
}

void adjustCount(CameraSettings &s, int direction) {
  s.timelapseCount = std::clamp(s.timelapseCount + direction, 0, 999);
}

void adjustImgFormat(CameraSettings &s, int direction) {
  static constexpr OutputFormat kFmtOrder[] = {
      OutputFormat::JPEG,    OutputFormat::DNG, OutputFormat::DngJpeg,
      OutputFormat::RawJpeg, OutputFormat::PNG, OutputFormat::PPM,
      OutputFormat::RAW_NV12};
  constexpr int n = static_cast<int>(std::size(kFmtOrder));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (s.captureFormat == kFmtOrder[i]) {
      idx = i;
      break;
    }
  s.captureFormat = kFmtOrder[(idx + direction + n) % n];
}

void adjustImgQuality(CameraSettings &s, int direction) {
  if (direction > 0) {
    if (s.jpegQuality < kJpegQualityMax)
      ++s.jpegQuality;
  } else {
    if (s.jpegQuality > kMinJpegQuality)
      --s.jpegQuality;
  }
}

void adjustImgAspect(CameraSettings &s, int direction) {
  s.aspectRatio = cycleEnum(s.aspectRatio, direction, kAspectRatioCount);
}

void adjustImgSize(CameraSettings &s, int direction) {
  s.imageSize = cycleEnum(s.imageSize, direction, kImageSizeCount);
}

void adjustImgFileNaming(CameraSettings &s, int direction) {
  s.fileNamingMode = cycleEnum(s.fileNamingMode, direction, kFileNamingCount);
}

void adjustImgDateSubfolders(CameraSettings &s, int direction) {
  s.useDateSubfolders = (direction > 0);
}

void adjustImgAwb(CameraSettings &s, int direction) {
  static constexpr std::string_view modes[] = {
      "auto",        "daylight", "cloudy", "incandescent", "tungsten",
      "fluorescent", "indoor",   "shade",  "flash"};
  constexpr int n = static_cast<int>(std::size(modes));
  if (!s.awbEnable) {
    s.awbEnable = true;
    s.awbMode =
        (direction > 0) ? std::string(modes[0]) : std::string(modes[n - 1]);
  } else {
    int idx = -1;
    for (int i = 0; i < n; ++i)
      if (s.awbMode == modes[i]) {
        idx = i;
        break;
      }
    if (direction > 0) {
      if (idx < 0)
        s.awbMode = std::string(modes[0]);
      else if (idx < n - 1)
        s.awbMode = std::string(modes[idx + 1]);
      else
        s.awbEnable = false;
    } else {
      if (idx < 0)
        s.awbMode = std::string(modes[0]);
      else if (idx > 0)
        s.awbMode = std::string(modes[idx - 1]);
      else
        s.awbEnable = false;
    }
  }
}

void adjustImgKelvin(CameraSettings &s, int direction) {
  s.wbKelvin = std::clamp(s.wbKelvin + direction * 100, kMinKelvin, kMaxKelvin);
}

void adjustImgWbRed(CameraSettings &s, int direction) {
  s.wbRedGain = std::clamp(s.wbRedGain + direction * kFineStep, 0.1f, 8.0f);
}

void adjustImgWbBlue(CameraSettings &s, int direction) {
  s.wbBlueGain = std::clamp(s.wbBlueGain + direction * kFineStep, 0.1f, 8.0f);
}

void adjustImgWbSet(CameraSettings &s, int /*direction*/) {
  // One-touch custom white balance: arm the measure flag (consumed by the
  // viewfinder, which reads the live frame's chroma) and switch to manual
  // R/B gain mode so the computed gains take effect.
  s.wbMeasurePending = true;
  s.awbEnable = false;
}

void adjustImgBrightness(CameraSettings &s, int direction) {
  s.brightness = std::clamp(s.brightness + direction * kFineStep, -1.0f, 1.0f);
}

void adjustImgContrast(CameraSettings &s, int direction) {
  s.contrast = std::clamp(s.contrast + direction * kFineStep, 0.0f, 2.0f);
}

void adjustImgSaturation(CameraSettings &s, int direction) {
  s.saturation = std::clamp(s.saturation + direction * kFineStep, 0.0f, 2.0f);
}

void adjustImgSharpness(CameraSettings &s, int direction) {
  s.sharpness =
      std::clamp(s.sharpness + direction * kSharpnessStep, 0.0f, kMaxSharpness);
}

void adjustImgNr(CameraSettings &s, int direction) {
  s.noiseReduction =
      cycleEnum(s.noiseReduction, direction, kNoiseReductionModeCount);
}

void adjustDispGrid(CameraSettings &s, int direction) {
  s.gridType = cycleEnum(s.gridType, direction, kGridTypeCount);
}

void adjustDispHist(CameraSettings &s, int direction) {
  s.showHistogram = (direction > 0);
}

void adjustDispZebra(CameraSettings &s, int direction) {
  s.zebraMode = cycleEnum(s.zebraMode, direction, kZebraModeCount);
}

void adjustDispPeak(CameraSettings &s, int direction) {
  s.focusPeaking = (direction > 0);
}

void adjustDispBright(CameraSettings &s, int direction) {
  if (direction > 0) {
    if (s.displayBrightness < kMaxDisplayBrightness)
      s.displayBrightness += kDisplayBrightnessStep;
  } else {
    if (s.displayBrightness > kMinDisplayBrightness)
      s.displayBrightness -= kDisplayBrightnessStep;
  }
}

void adjustSysBattery(CameraSettings &s, int direction) {
  s.enableBattery = (direction > 0);
}

void adjustSysPowerSave(CameraSettings &s, int direction) {
  static constexpr int steps[] = {30, 0, 300, 60};
  constexpr int n = static_cast<int>(std::size(steps));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (s.powerSaveTimeout == steps[i]) {
      idx = i;
      break;
    }
  if (direction > 0)
    s.powerSaveTimeout = steps[(idx + n - 1) % n];
  else
    s.powerSaveTimeout = steps[(idx + 1) % n];
}

void adjustExpMode(CameraSettings &s, int direction) {
  s.exposureMode = cycleEnum(s.exposureMode, direction, kExposureModeCount);
  // Apply exposure mode semantics:
  // P/Auto: AE on, shutter+ISO auto
  // S: AE on, shutter manual, ISO auto (shutter priority)
  // M: AE off, shutter+ISO manual
  switch (s.exposureMode) {
  case ExposureMode::Program:
  case ExposureMode::Auto:
    s.aeEnable = true;
    s.shutterUs = 0;
    s.analogueGain = 0;
    break;
  case ExposureMode::Shutter:
    s.aeEnable = true; // AE still handles ISO
    if (s.shutterUs == 0)
      s.shutterUs = 1000; // default 1/1000
    break;
  case ExposureMode::Manual:
    s.aeEnable = false;
    if (s.shutterUs == 0)
      s.shutterUs = 1000;
    if (s.analogueGain == 0)
      s.analogueGain = 1.0f;
    break;
  }
}

void adjustBracketType(CameraSettings &s, int direction) {
  s.bracketType = cycleEnum(s.bracketType, direction, kBracketTypeCount);
}

void adjustPictureStyle(CameraSettings &s, int direction) {
  s.pictureStyle = cycleEnum(s.pictureStyle, direction, kPictureStyleCount);
  auto p = pictureStyleParams(s.pictureStyle);
  s.brightness = p.brightness;
  s.contrast = p.contrast;
  s.saturation = p.saturation;
  s.sharpness = p.sharpness;
}

void adjustFocusMagnify(CameraSettings &s, int direction) {
  static constexpr int kValues[] = {0, 2, 4};
  constexpr int n = static_cast<int>(std::size(kValues));
  int idx = 0;
  for (int i = 0; i < n; ++i)
    if (s.focusMagnify == kValues[i]) {
      idx = i;
      break;
    }
  s.focusMagnify = kValues[(idx + direction + n) % n];
}

void adjustColorSpace(CameraSettings &s, int direction) {
  s.colorSpace = cycleEnum(s.colorSpace, direction, kColorSpaceCount);
}

void adjustWbGm(CameraSettings &s, int direction) {
  s.wbGmShift = std::clamp(s.wbGmShift + static_cast<float>(direction),
                           -9.0f, 9.0f);
}

void adjustMinShutter(CameraSettings &s, int direction) {
  int idx = findShutterIdx(s.minShutterUs);
  int newIdx = idx + direction;
  if (newIdx >= 0 && newIdx < kShutterStepCount)
    s.minShutterUs = kShutterSteps[newIdx];
}

void adjustLongExposureNr(CameraSettings &s, int direction) {
  s.longExposureNr = (direction > 0);
}

void adjustSilentShutter(CameraSettings &s, int direction) {
  s.silentShutter = (direction > 0);
}

void adjustAirplaneMode(CameraSettings &s, int direction) {
  s.airplaneMode = (direction > 0);
}

void adjustRotateTall(CameraSettings &s, int direction) {
  s.rotateTall = (direction > 0);
}

void adjustNightMode(CameraSettings &s, int direction) {
  s.nightMode = (direction > 0);
}

void adjustGrainEffect(CameraSettings &s, int direction) {
  s.grainEffect = (direction > 0);
}

void adjustHdrMerge(CameraSettings &s, int direction) {
  s.hdrMerge = (direction > 0);
}

void adjustCustomMode(CameraSettings &s, int direction) {
  s.customMode = cycleEnum(s.customMode, direction, kCustomModeCount);
}

const char *videoResolutionLabel(VideoResolution r) {
  switch (r) {
  case VideoResolution::Res320x240:
    return "320x240";
  case VideoResolution::Res640x480:
    return "640x480";
  case VideoResolution::Res1280x720:
    return "720P";
  case VideoResolution::Res1920x1080:
    return "1080P";
  }
  return "??";
}

void adjustVideoResolution(CameraSettings &s, int direction) {
  s.videoResolution =
      cycleEnum(s.videoResolution, direction, kVideoResolutionCount);
}

void adjustVideoFps(CameraSettings &s, int direction) {
  int idx = 0;
  for (int i = 0; i < kVideoFpsStepCount; ++i)
    if (kVideoFpsSteps[i] == s.videoFps) {
      idx = i;
      break;
    }
  idx = (idx + direction + kVideoFpsStepCount) % kVideoFpsStepCount;
  s.videoFps = kVideoFpsSteps[idx];
}

void adjustVideoCodec(CameraSettings &s, int direction) {
  s.videoCodec = cycleEnum(s.videoCodec, direction, kVideoCodecCount);
}

void adjustVideoBitrate(CameraSettings &s, int direction) {
  int idx = 0;
  for (int i = 0; i < kVideoBitrateStepCount; ++i)
    if (kVideoBitrateSteps[i] == s.videoBitrate) {
      idx = i;
      break;
    }
  idx = (idx + direction + kVideoBitrateStepCount) % kVideoBitrateStepCount;
  s.videoBitrate = kVideoBitrateSteps[idx];
}

void adjustSensorMode(CameraSettings &s, int direction) {
  s.sensorMode = cycleEnum(s.sensorMode, direction, kSensorModeCount);
}

std::string_view valDrive(const CameraSettings &s, std::string &) {
  return driveModeLabel(s.driveMode);
}

std::string_view valShutter(const CameraSettings &s, std::string &buf) {
  buf = formatShutter(s.shutterUs);
  return buf;
}

std::string_view valIso(const CameraSettings &s, std::string &buf) {
  if (s.analogueGain == 0)
    return "AUTO";
  buf = std::to_string(static_cast<int>(s.analogueGain * kIsoScaleFactor));
  return buf;
}

std::string_view valIsoMin(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.isoMin);
  return buf;
}

std::string_view valIsoMax(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.isoMax);
  return buf;
}

std::string_view valEv(const CameraSettings &s, std::string &buf) {
  buf = fmtEv(s.exposureValue);
  return buf;
}

std::string_view valMeter(const CameraSettings &s, std::string &) {
  return meteringLabel(s.meteringMode);
}

std::string_view valAeMode(const CameraSettings &s, std::string &) {
  return aeExposureLabel(s.aeExposureMode);
}

std::string_view valAeConst(const CameraSettings &s, std::string &) {
  return aeConstraintLabel(s.aeConstraintMode);
}

std::string_view valFlicker(const CameraSettings &s, std::string &buf) {
  if (!s.antiFlicker)
    return "OFF";
  buf = std::to_string(s.flickerHz) + "Hz";
  return buf;
}

std::string_view valTimer(const CameraSettings &s, std::string &buf) {
  if (s.timerDuration == 0)
    return "OFF";
  buf = std::to_string(s.timerDuration) + "S";
  return buf;
}

std::string_view valBracket(const CameraSettings &s, std::string &buf) {
  if (s.bracketEv.empty())
    return "OFF";
  float maxEv = 0;
  for (float ev : s.bracketEv)
    maxEv = std::max(maxEv, std::abs(ev));
  buf = "+-" + fmt1(maxEv) + "EV";
  return buf;
}

std::string_view valInterval(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.timelapseInterval) + "S";
  return buf;
}

std::string_view valCount(const CameraSettings &s, std::string &buf) {
  if (s.timelapseCount == 0)
    return "INF";
  buf = std::to_string(s.timelapseCount);
  return buf;
}

std::string_view valFormat(const CameraSettings &s, std::string &) {
  return formatLabel(s.captureFormat);
}

std::string_view valQuality(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.jpegQuality);
  return buf;
}

std::string_view valAspect(const CameraSettings &s, std::string &) {
  return aspectLabel(s.aspectRatio);
}

std::string_view valImgSize(const CameraSettings &s, std::string &) {
  return imageSizeLabel(s.imageSize);
}

std::string_view valFileNaming(const CameraSettings &s, std::string &) {
  return fileNamingLabel(s.fileNamingMode);
}

std::string_view valDateSubfolders(const CameraSettings &s, std::string &) {
  return s.useDateSubfolders ? "ON" : "OFF";
}

std::string_view valAwb(const CameraSettings &s, std::string &) {
  return s.awbEnable ? std::string_view(s.awbMode) : std::string_view("OFF");
}

std::string_view valKelvin(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.wbKelvin) + "K";
  return buf;
}

std::string_view valWbRed(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.wbRedGain);
  return buf;
}

std::string_view valWbBlue(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.wbBlueGain);
  return buf;
}

std::string_view valWbSet(const CameraSettings &s, std::string &) {
  // "SET" while a measurement is pending (armed in settings, awaiting the
  // next viewfinder frame); "OFF" once consumed.
  return s.wbMeasurePending ? "SET" : "OFF";
}

std::string_view valBrightness(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.brightness);
  return buf;
}

std::string_view valContrast(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.contrast);
  return buf;
}

std::string_view valSaturation(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.saturation);
  return buf;
}

std::string_view valSharpness(const CameraSettings &s, std::string &buf) {
  buf = fmt1(s.sharpness);
  return buf;
}

std::string_view valNr(const CameraSettings &s, std::string &) {
  return nrLabel(s.noiseReduction);
}

std::string_view valDispGrid(const CameraSettings &s, std::string &) {
  return gridLabel(s.gridType);
}

std::string_view valDispHist(const CameraSettings &s, std::string &) {
  return s.showHistogram ? "ON" : "OFF";
}

std::string_view valDispZebra(const CameraSettings &s, std::string &) {
  return zebraLabel(s.zebraMode);
}

std::string_view valDispPeak(const CameraSettings &s, std::string &) {
  return s.focusPeaking ? "ON" : "OFF";
}

std::string_view valDispBright(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.displayBrightness) + "%";
  return buf;
}

std::string_view valSysBattery(const CameraSettings &s, std::string &) {
  return s.enableBattery ? "ON" : "OFF";
}

std::string_view valPowerSave(const CameraSettings &s, std::string &buf) {
  if (s.powerSaveTimeout == 0)
    return "OFF";
  buf = std::to_string(s.powerSaveTimeout) + "S";
  return buf;
}

std::string_view valExit(const CameraSettings &, std::string &) { return ""; }

std::string_view valVideo(const CameraSettings &s, std::string &) {
  return s.driveMode == DriveMode::Video ? "ON" : "OFF";
}

std::string_view valExpMode(const CameraSettings &s, std::string &) {
  return exposureModeLabel(s.exposureMode);
}

std::string_view valBracketType(const CameraSettings &s, std::string &) {
  return bracketTypeLabel(s.bracketType);
}

std::string_view valPictureStyle(const CameraSettings &s, std::string &) {
  return pictureStyleLabel(s.pictureStyle);
}

std::string_view valFocusMagnify(const CameraSettings &s, std::string &) {
  return focusMagnifyLabel(s.focusMagnify);
}

std::string_view valColorSpace(const CameraSettings &s, std::string &) {
  return colorSpaceLabel(s.colorSpace);
}

std::string_view valWbGm(const CameraSettings &s, std::string &buf) {
  if (s.wbGmShift == 0.0f)
    return "0";
  buf = (s.wbGmShift > 0) ? "M" : "G";
  buf += std::to_string(static_cast<int>(std::abs(s.wbGmShift)));
  return buf;
}

std::string_view valMinShutter(const CameraSettings &s, std::string &buf) {
  buf = formatMinShutter(s.minShutterUs);
  return buf;
}

std::string_view valLongExposureNr(const CameraSettings &s, std::string &) {
  return s.longExposureNr ? "ON" : "OFF";
}

std::string_view valSilentShutter(const CameraSettings &s, std::string &) {
  return s.silentShutter ? "ON" : "OFF";
}

std::string_view valAirplaneMode(const CameraSettings &s, std::string &) {
  return s.airplaneMode ? "ON" : "OFF";
}

std::string_view valRotateTall(const CameraSettings &s, std::string &) {
  return s.rotateTall ? "ON" : "OFF";
}

std::string_view valNightMode(const CameraSettings &s, std::string &) {
  return s.nightMode ? "ON" : "OFF";
}

std::string_view valGrainEffect(const CameraSettings &s, std::string &) {
  return s.grainEffect ? "ON" : "OFF";
}

std::string_view valHdrMerge(const CameraSettings &s, std::string &) {
  return s.hdrMerge ? "ON" : "OFF";
}

std::string_view valCustomMode(const CameraSettings &s, std::string &) {
  return customModeLabel(s.customMode);
}

std::string_view valVideoResolution(const CameraSettings &s, std::string &) {
  return videoResolutionLabel(s.videoResolution);
}

std::string_view valVideoFps(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.videoFps) + "FPS";
  return buf;
}

std::string_view valVideoCodec(const CameraSettings &s, std::string &) {
  return videoCodecLabel(s.videoCodec);
}

std::string_view valVideoBitrate(const CameraSettings &s, std::string &buf) {
  buf = std::to_string(s.videoBitrate) + "MB";
  return buf;
}

std::string_view valSensorMode(const CameraSettings &s, std::string &) {
  return sensorModeLabel(s.sensorMode);
}

std::string_view valCopyright(const CameraSettings &s, std::string &) {
  return s.copyright.empty() ? std::string_view("OFF")
                             : std::string_view("SET");
}

struct SettingItem {
  std::string_view label;
  std::string_view (*valueFn)(const CameraSettings &, std::string &buf);
  void (*adjustFn)(CameraSettings &, int);
};

constexpr std::array<SettingItem, 20> kShootTab = {{
    {"DRIVE", valDrive, adjustDrive},
    {"SHUTTER", valShutter, adjustShutter},
    {"ISO", valIso, adjustIso},
    {"ISO MIN", valIsoMin, adjustIsoMin},
    {"ISO MAX", valIsoMax, adjustIsoMax},
    {"MIN SS", valMinShutter, adjustMinShutter},
    {"EV", valEv, adjustEv},
    {"METER", valMeter, adjustMeter},
    {"AEMODE", valAeMode, adjustAeMode},
    {"AECONST", valAeConst, adjustAeConst},
    {"FLICKER", valFlicker, adjustFlicker},
    {"TIMER", valTimer, adjustTimer},
    {"BRACKET", valBracket, adjustBracket},
    {"HDR", valHdrMerge, adjustHdrMerge},
    {"LENR", valLongExposureNr, adjustLongExposureNr},
    {"INTERVAL", valInterval, adjustInterval},
    {"COUNT", valCount, adjustCount},
    {"EXPMODE", valExpMode, adjustExpMode},
    {"BRKTYPE", valBracketType, adjustBracketType},
    {"SMODE", valSensorMode, adjustSensorMode},
}};

constexpr std::array<SettingItem, 21> kImgTab = {{
    {"FORMAT", valFormat, adjustImgFormat},
    {"QUALITY", valQuality, adjustImgQuality},
    {"SIZE", valImgSize, adjustImgSize},
    {"ASPECT", valAspect, adjustImgAspect},
    {"COLOR", valColorSpace, adjustColorSpace},
    {"AWB", valAwb, adjustImgAwb},
    {"KELVIN", valKelvin, adjustImgKelvin},
    {"WBRED", valWbRed, adjustImgWbRed},
    {"WBBLUE", valWbBlue, adjustImgWbBlue},
    {"WBGM", valWbGm, adjustWbGm},
    {"BRIGHT", valBrightness, adjustImgBrightness},
    {"CONTRAST", valContrast, adjustImgContrast},
    {"SAT", valSaturation, adjustImgSaturation},
    {"SHARP", valSharpness, adjustImgSharpness},
    {"NR", valNr, adjustImgNr},
    {"FILENAME", valFileNaming, adjustImgFileNaming},
    {"DATEFOLD", valDateSubfolders, adjustImgDateSubfolders},
    {"PSTYLE", valPictureStyle, adjustPictureStyle},
    {"GRAIN", valGrainEffect, adjustGrainEffect},
    {"COPYRIGHT", valCopyright, nullptr},
    {"WBSET", valWbSet, adjustImgWbSet},
}};

constexpr std::array<SettingItem, 8> kDispTab = {{
    {"GRID", valDispGrid, adjustDispGrid},
    {"HIST", valDispHist, adjustDispHist},
    {"ZEBRA", valDispZebra, adjustDispZebra},
    {"PEAK", valDispPeak, adjustDispPeak},
    {"BRIGHT", valDispBright, adjustDispBright},
    {"FOCUSMAG", valFocusMagnify, adjustFocusMagnify},
    {"NIGHT", valNightMode, adjustNightMode},
    {"ROTATE", valRotateTall, adjustRotateTall},
}};

constexpr std::array<SettingItem, 14> kSysTab = {{
    {"BATTERY", valSysBattery, adjustSysBattery},
    {"PWRSAVE", valPowerSave, adjustSysPowerSave},
    {"SILENT", valSilentShutter, adjustSilentShutter},
    {"AIRPLANE", valAirplaneMode, adjustAirplaneMode},
    {"C MODE", valCustomMode, adjustCustomMode},
    {"FORMAT", valExit, nullptr},
    {"RESET", valExit, nullptr},
    {"DATE", valExit, nullptr},
    {"VIDEO", valVideo, nullptr},
    {"VIDRES", valVideoResolution, adjustVideoResolution},
    {"VIDFPS", valVideoFps, adjustVideoFps},
    {"VIDCODEC", valVideoCodec, adjustVideoCodec},
    {"VIDBITRATE", valVideoBitrate, adjustVideoBitrate},
    {"EXIT", valExit, nullptr},
}};

std::span<const SettingItem> tabItems(SettingsTab tab) {
  switch (tab) {
  case SettingsTab::Shooting:
    return kShootTab;
  case SettingsTab::Image:
    return kImgTab;
  case SettingsTab::Display:
    return kDispTab;
  case SettingsTab::System:
    return kSysTab;
  }
  return {};
}

void adjustSetting(SettingsTab tab, int item, CameraSettings &s,
                   int direction) {
  auto items = tabItems(tab);
  if (item >= 0 && item < static_cast<int>(items.size()) &&
      items[item].adjustFn)
    items[item].adjustFn(s, direction);
}

} // namespace

const char *videoCodecLabel(VideoCodec c) {
  switch (c) {
  case VideoCodec::MJPEG:
    return "MJPEG";
  case VideoCodec::H264:
    // No HW H264 encoder via libcamera on Pi Zero 2 W; the recording
    // pipeline falls back to MJPEG encoding. Show the fallback in the
    // menu so the user knows H264 is not a true H.264 bitstream.
    return "MJPEG*";
  case VideoCodec::YUV:
    return "YUV";
  }
  return "??";
}

const char *sensorModeLabel(SensorMode m) {
  switch (m) {
  case SensorMode::Auto:
    return "AUTO";
  case SensorMode::Mode1332x990:
    return "1332x990";
  case SensorMode::Mode2028x1080:
    return "2028x1080";
  case SensorMode::Mode2028x1520:
    return "2028x1520";
  case SensorMode::Mode4056x3040:
    return "4056x3040";
  }
  return "??";
}

int settingsTabItemCount(SettingsTab tab) {
  return static_cast<int>(tabItems(tab).size());
}

std::string_view settingsItemLabel(SettingsTab tab, int item) {
  auto items = tabItems(tab);
  if (item >= 0 && item < static_cast<int>(items.size()))
    return items[item].label;
  return "??";
}

std::string settingsItemValue(SettingsTab tab, int item,
                              const CameraSettings &s) {
  auto items = tabItems(tab);
  if (item >= 0 && item < static_cast<int>(items.size())) {
    std::string buf;
    return std::string(items[item].valueFn(s, buf));
  }
  return "??";
}

void settingsItemAdjustLeft(SettingsTab tab, int item, CameraSettings &s) {
  adjustSetting(tab, item, s, -1);
}

void settingsItemAdjustRight(SettingsTab tab, int item, CameraSettings &s) {
  adjustSetting(tab, item, s, 1);
}

bool settingsNeedsReconfigure(const CameraSettings &before,
                              const CameraSettings &after) {
  return before.captureFormat != after.captureFormat ||
         before.aspectRatio != after.aspectRatio ||
         before.sensorMode != after.sensorMode;
}

CameraConfig settingsToCameraConfig(const CameraSettings &s,
                                    uint32_t captureWidth,
                                    uint32_t captureHeight) {
  CameraConfig cfg;
  cfg.width = captureWidth;
  cfg.height = captureHeight;
  cfg.exposureTime = s.shutterUs;
  cfg.analogueGain = s.analogueGain;
  cfg.digitalGain = s.digitalGain;
  cfg.awbMode = s.awbMode;
  cfg.aeEnable = s.aeEnable;
  cfg.awbEnable = s.awbEnable;
  cfg.format = s.captureFormat;
  cfg.jpegQuality = s.jpegQuality;
  cfg.pngLevel = s.pngLevel;
  cfg.bracketEv = s.bracketEv;

  cfg.exposureValue = s.exposureValue;
  cfg.meteringMode = s.meteringMode;
  cfg.aeExposureMode = s.aeExposureMode;
  cfg.aeConstraintMode = s.aeConstraintMode;
  cfg.brightness = s.brightness;
  cfg.contrast = s.contrast;
  cfg.saturation = s.saturation;
  cfg.sharpness = s.sharpness;
  cfg.antiFlicker = s.antiFlicker;
  cfg.flickerPeriodUs =
      s.antiFlicker
          ? (s.flickerHz == 50 ? kFlicker50HzPeriodUs : kFlicker60HzPeriodUs)
          : 0;
  cfg.wbRedGain = s.wbRedGain;
  cfg.wbBlueGain = s.wbBlueGain;
  cfg.wbKelvin = s.wbKelvin;
  cfg.noiseReductionMode = s.noiseReduction;
  cfg.imageSize = s.imageSize;
  cfg.aspectRatio = s.aspectRatio;
  cfg.isoMin = s.isoMin;
  cfg.isoMax = s.isoMax;
  cfg.colorSpace = static_cast<int>(s.colorSpace);
  cfg.copyright = s.copyright;
  cfg.minShutterUs = s.minShutterUs;
  cfg.wbGmShift = s.wbGmShift;
  cfg.grainEffect = s.grainEffect;

  // Apply exposure mode semantics to the config.
  // P/Auto: AE on, shutter+ISO auto (already defaults).
  // S: AE on, shutter manual, ISO auto (shutter priority).
  // M: AE off, shutter+ISO manual.
  switch (s.exposureMode) {
  case ExposureMode::Program:
  case ExposureMode::Auto:
    break;
  case ExposureMode::Shutter:
    cfg.aeEnable = true; // AE handles ISO
    if (s.shutterUs > 0)
      cfg.exposureTime = s.shutterUs;
    break;
  case ExposureMode::Manual:
    cfg.aeEnable = false;
    if (s.shutterUs > 0)
      cfg.exposureTime = s.shutterUs;
    if (s.analogueGain > 0)
      cfg.analogueGain = s.analogueGain;
    break;
  }

  return cfg;
}

PictureStyleParams pictureStyleParams(PictureStyle style) {
  switch (style) {
  case PictureStyle::Standard:
    return {0.0f, 1.0f, 1.0f, 1.0f};
  case PictureStyle::Vivid:
    return {0.0f, 1.2f, 1.3f, 1.2f};
  case PictureStyle::Natural:
    return {0.0f, 0.9f, 0.8f, 0.8f};
  case PictureStyle::Monochrome:
    return {0.0f, 1.1f, 0.0f, 1.0f};
  case PictureStyle::Portrait:
    return {0.1f, 0.95f, 0.9f, 0.8f};
  case PictureStyle::Landscape:
    return {0.0f, 1.15f, 1.2f, 1.3f};
  case PictureStyle::Sepia:
    return {-0.1f, 1.0f, 0.0f, 0.9f};
  case PictureStyle::Cool:
    return {0.0f, 1.05f, 1.0f, 1.0f};
  case PictureStyle::Warm:
    return {0.05f, 1.05f, 1.0f, 1.0f};
  case PictureStyle::Film:
    return {0.0f, 1.1f, 0.95f, 0.9f};
  case PictureStyle::HDR:
    return {0.0f, 1.2f, 1.1f, 1.0f};
  }
  return {0.0f, 1.0f, 1.0f, 1.0f};
}

CropRegion aspectRatioCrop(uint32_t srcW, uint32_t srcH, AspectRatio ratio) {
  CropRegion r{0, 0, srcW, srcH};
  if (srcW == 0 || srcH == 0)
    return r;
  switch (ratio) {
  case AspectRatio::Native:
    // IMX477 native is 4056x3040 = 1.336 ≈ 4:3. Keep as-is.
    break;
  case AspectRatio::Ratio43: {
    float target = static_cast<float>(srcW) / srcH;
    if (target > 1.333f) {
      uint32_t cw = static_cast<uint32_t>(srcH * 4.0f / 3.0f) & ~1u;
      r.w = std::min(cw, srcW);
      r.h = srcH;
      r.x = ((srcW - r.w) / 2) & ~1u;
      r.y = 0;
    } else {
      uint32_t ch = static_cast<uint32_t>(srcW * 3.0f / 4.0f) & ~1u;
      r.h = std::min(ch, srcH);
      r.w = srcW;
      r.x = 0;
      r.y = ((srcH - r.h) / 2) & ~1u;
    }
    break;
  }
  case AspectRatio::Ratio169: {
    uint32_t ch =
        static_cast<uint32_t>(static_cast<float>(srcW) * 9.0f / 16.0f) & ~1u;
    r.h = std::min(ch, srcH);
    r.w = srcW;
    r.x = 0;
    r.y = ((srcH - r.h) / 2) & ~1u;
    break;
  }
  case AspectRatio::Ratio11: {
    uint32_t cw = srcH & ~1u;
    r.w = std::min(cw, srcW);
    r.h = srcH;
    r.x = ((srcW - r.w) / 2) & ~1u;
    r.y = 0;
    break;
  }
  }
  return r;
}

VideoDimensions videoResolutionDims(VideoResolution r) {
  switch (r) {
  case VideoResolution::Res320x240:
    return {320, 240};
  case VideoResolution::Res640x480:
    return {640, 480};
  case VideoResolution::Res1280x720:
    return {1280, 720};
  case VideoResolution::Res1920x1080:
    return {1920, 1080};
  }
  return {320, 240};
}

VideoDimensions sensorModeDims(SensorMode m) {
  switch (m) {
  case SensorMode::Auto:
    return {0, 0};
  case SensorMode::Mode1332x990:
    return {1332, 990};
  case SensorMode::Mode2028x1080:
    return {2028, 1080};
  case SensorMode::Mode2028x1520:
    return {2028, 1520};
  case SensorMode::Mode4056x3040:
    return {4056, 3040};
  }
  return {0, 0};
}

namespace {

std::string_view trimSv(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                        s.front() == '\r' || s.front() == '\n'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\r' || s.back() == '\n'))
    s.remove_suffix(1);
  return s;
}

void writeKey(std::ofstream &f, std::string_view key, std::string_view val) {
  f << key << "=" << val << "\n";
}

void writeKeyInt(std::ofstream &f, std::string_view key, long long val) {
  f << key << "=" << val << "\n";
}

void writeKeyFloat(std::ofstream &f, std::string_view key, float val) {
  f << key << "=" << val << "\n";
}

void writeKeyBool(std::ofstream &f, std::string_view key, bool val) {
  f << key << "=" << (val ? "true" : "false") << "\n";
}

void applySettingsKey(CameraSettings &s, std::string_view key,
                      std::string_view val) {
  auto toInt = [](std::string_view v) -> long long {
    try {
      return std::stoll(std::string(v));
    } catch (...) {
      return 0;
    }
  };
  auto toFloat = [](std::string_view v) -> float {
    try {
      return std::stof(std::string(v));
    } catch (...) {
      return 0;
    }
  };
  auto toBool = [](std::string_view v) -> bool {
    return v == "true" || v == "1" || v == "yes" || v == "on";
  };
  if (key == "driveMode")
    s.driveMode = static_cast<DriveMode>(toInt(val));
  else if (key == "aeEnable")
    s.aeEnable = toBool(val);
  else if (key == "shutterUs")
    s.shutterUs = static_cast<uint64_t>(toInt(val));
  else if (key == "analogueGain")
    s.analogueGain = toFloat(val);
  else if (key == "digitalGain")
    s.digitalGain = toFloat(val);
  else if (key == "exposureValue")
    s.exposureValue = toFloat(val);
  else if (key == "isoMin")
    s.isoMin = static_cast<int>(toInt(val));
  else if (key == "isoMax")
    s.isoMax = static_cast<int>(toInt(val));
  else if (key == "meteringMode")
    s.meteringMode = static_cast<MeteringMode>(toInt(val));
  else if (key == "aeExposureMode")
    s.aeExposureMode = static_cast<AeExposureMode>(toInt(val));
  else if (key == "aeConstraintMode")
    s.aeConstraintMode = static_cast<AeConstraintMode>(toInt(val));
  else if (key == "antiFlicker")
    s.antiFlicker = toBool(val);
  else if (key == "flickerHz")
    s.flickerHz = static_cast<int>(toInt(val));
  else if (key == "timerDuration")
    s.timerDuration = static_cast<uint32_t>(toInt(val));
  else if (key == "timelapseInterval")
    s.timelapseInterval = static_cast<int>(toInt(val));
  else if (key == "timelapseCount")
    s.timelapseCount = static_cast<int>(toInt(val));
  else if (key == "captureFormat")
    s.captureFormat = static_cast<OutputFormat>(toInt(val));
  else if (key == "jpegQuality")
    s.jpegQuality = static_cast<int>(toInt(val));
  else if (key == "pngLevel")
    s.pngLevel = static_cast<int>(toInt(val));
  else if (key == "imageSize")
    s.imageSize = static_cast<ImageSize>(toInt(val));
  else if (key == "aspectRatio")
    s.aspectRatio = static_cast<AspectRatio>(toInt(val));
  else if (key == "awbEnable")
    s.awbEnable = toBool(val);
  else if (key == "awbMode")
    s.awbMode = std::string(val);
  else if (key == "wbKelvin")
    s.wbKelvin = static_cast<int>(toInt(val));
  else if (key == "wbRedGain")
    s.wbRedGain = toFloat(val);
  else if (key == "wbBlueGain")
    s.wbBlueGain = toFloat(val);
  else if (key == "brightness")
    s.brightness = toFloat(val);
  else if (key == "contrast")
    s.contrast = toFloat(val);
  else if (key == "saturation")
    s.saturation = toFloat(val);
  else if (key == "sharpness")
    s.sharpness = toFloat(val);
  else if (key == "noiseReduction")
    s.noiseReduction = static_cast<NoiseReductionMode>(toInt(val));
  else if (key == "gridType")
    s.gridType = static_cast<GridType>(toInt(val));
  else if (key == "showHistogram")
    s.showHistogram = toBool(val);
  else if (key == "zebraMode")
    s.zebraMode = static_cast<ZebraMode>(toInt(val));
  else if (key == "focusPeaking")
    s.focusPeaking = toBool(val);
  else if (key == "displayBrightness")
    s.displayBrightness = static_cast<int>(toInt(val));
  else if (key == "enableBattery")
    s.enableBattery = toBool(val);
  else if (key == "powerSaveTimeout")
    s.powerSaveTimeout = static_cast<int>(toInt(val));
  else if (key == "fileNamingMode")
    s.fileNamingMode = static_cast<FileNamingMode>(toInt(val));
  else if (key == "useDateSubfolders")
    s.useDateSubfolders = toBool(val);
  else if (key == "focusMagnify")
    s.focusMagnify = static_cast<int>(toInt(val));
  else if (key == "exposureMode")
    s.exposureMode = static_cast<ExposureMode>(toInt(val));
  else if (key == "bracketType")
    s.bracketType = static_cast<BracketType>(toInt(val));
  else if (key == "pictureStyle")
    s.pictureStyle = static_cast<PictureStyle>(toInt(val));
  else if (key == "colorSpace")
    s.colorSpace = static_cast<ColorSpace>(toInt(val));
  else if (key == "wbGmShift")
    s.wbGmShift = toFloat(val);
  else if (key == "minShutterUs")
    s.minShutterUs = static_cast<uint64_t>(toInt(val));
  else if (key == "longExposureNr")
    s.longExposureNr = toBool(val);
  else if (key == "silentShutter")
    s.silentShutter = toBool(val);
  else if (key == "airplaneMode")
    s.airplaneMode = toBool(val);
  else if (key == "rotateTall")
    s.rotateTall = toBool(val);
  else if (key == "nightMode")
    s.nightMode = toBool(val);
  else if (key == "copyright")
    s.copyright = std::string(val);
  else if (key == "grainEffect")
    s.grainEffect = toBool(val);
  else if (key == "hdrMerge")
    s.hdrMerge = toBool(val);
  else if (key == "customMode")
    s.customMode = static_cast<CustomMode>(toInt(val));
  else if (key == "videoResolution")
    s.videoResolution = static_cast<VideoResolution>(toInt(val));
  else if (key == "videoFps")
    s.videoFps = static_cast<int>(toInt(val));
  else if (key == "videoCodec")
    s.videoCodec = static_cast<VideoCodec>(toInt(val));
  else if (key == "videoBitrate")
    s.videoBitrate = static_cast<int>(toInt(val));
  else if (key == "sensorMode")
    s.sensorMode = static_cast<SensorMode>(toInt(val));
}

} // namespace

bool saveSettings(const CameraSettings &s, const std::string &path) {
  namespace fs = std::filesystem;
  try {
    fs::path p(path);
    fs::create_directories(p.parent_path());
  } catch (const std::exception &e) {
    std::cerr << "saveSettings: cannot create config dir: " << e.what() << "\n";
    return false;
  }
  std::ofstream f(path);
  if (!f.is_open())
    return false;
  f << "# picamera settings\n";
  writeKeyInt(f, "driveMode", static_cast<int>(s.driveMode));
  writeKeyBool(f, "aeEnable", s.aeEnable);
  writeKeyInt(f, "shutterUs", static_cast<long long>(s.shutterUs));
  writeKeyFloat(f, "analogueGain", s.analogueGain);
  writeKeyFloat(f, "digitalGain", s.digitalGain);
  writeKeyFloat(f, "exposureValue", s.exposureValue);
  writeKeyInt(f, "isoMin", s.isoMin);
  writeKeyInt(f, "isoMax", s.isoMax);
  writeKeyInt(f, "meteringMode", static_cast<int>(s.meteringMode));
  writeKeyInt(f, "aeExposureMode", static_cast<int>(s.aeExposureMode));
  writeKeyInt(f, "aeConstraintMode", static_cast<int>(s.aeConstraintMode));
  writeKeyBool(f, "antiFlicker", s.antiFlicker);
  writeKeyInt(f, "flickerHz", s.flickerHz);
  writeKeyInt(f, "timerDuration", s.timerDuration);
  writeKeyInt(f, "timelapseInterval", s.timelapseInterval);
  writeKeyInt(f, "timelapseCount", s.timelapseCount);
  writeKeyInt(f, "captureFormat", static_cast<int>(s.captureFormat));
  writeKeyInt(f, "jpegQuality", s.jpegQuality);
  writeKeyInt(f, "pngLevel", s.pngLevel);
  writeKeyInt(f, "imageSize", static_cast<int>(s.imageSize));
  writeKeyInt(f, "aspectRatio", static_cast<int>(s.aspectRatio));
  writeKeyBool(f, "awbEnable", s.awbEnable);
  writeKey(f, "awbMode", s.awbMode);
  writeKeyInt(f, "wbKelvin", s.wbKelvin);
  writeKeyFloat(f, "wbRedGain", s.wbRedGain);
  writeKeyFloat(f, "wbBlueGain", s.wbBlueGain);
  writeKeyFloat(f, "brightness", s.brightness);
  writeKeyFloat(f, "contrast", s.contrast);
  writeKeyFloat(f, "saturation", s.saturation);
  writeKeyFloat(f, "sharpness", s.sharpness);
  writeKeyInt(f, "noiseReduction", static_cast<int>(s.noiseReduction));
  writeKeyInt(f, "gridType", static_cast<int>(s.gridType));
  writeKeyBool(f, "showHistogram", s.showHistogram);
  writeKeyInt(f, "zebraMode", static_cast<int>(s.zebraMode));
  writeKeyBool(f, "focusPeaking", s.focusPeaking);
  writeKeyInt(f, "displayBrightness", s.displayBrightness);
  writeKeyBool(f, "enableBattery", s.enableBattery);
  writeKeyInt(f, "powerSaveTimeout", s.powerSaveTimeout);
  writeKeyInt(f, "fileNamingMode", static_cast<int>(s.fileNamingMode));
  writeKeyBool(f, "useDateSubfolders", s.useDateSubfolders);
  writeKeyInt(f, "focusMagnify", s.focusMagnify);
  writeKeyInt(f, "exposureMode", static_cast<int>(s.exposureMode));
  writeKeyInt(f, "bracketType", static_cast<int>(s.bracketType));
  writeKeyInt(f, "pictureStyle", static_cast<int>(s.pictureStyle));
  writeKeyInt(f, "colorSpace", static_cast<int>(s.colorSpace));
  writeKeyFloat(f, "wbGmShift", s.wbGmShift);
  writeKeyInt(f, "minShutterUs", static_cast<long long>(s.minShutterUs));
  writeKeyBool(f, "longExposureNr", s.longExposureNr);
  writeKeyBool(f, "silentShutter", s.silentShutter);
  writeKeyBool(f, "airplaneMode", s.airplaneMode);
  writeKeyBool(f, "rotateTall", s.rotateTall);
  writeKeyBool(f, "nightMode", s.nightMode);
  writeKey(f, "copyright", s.copyright);
  writeKeyBool(f, "grainEffect", s.grainEffect);
  writeKeyBool(f, "hdrMerge", s.hdrMerge);
  writeKeyInt(f, "customMode", static_cast<int>(s.customMode));
  writeKeyInt(f, "videoResolution", static_cast<int>(s.videoResolution));
  writeKeyInt(f, "videoFps", s.videoFps);
  writeKeyInt(f, "videoCodec", static_cast<int>(s.videoCodec));
  writeKeyInt(f, "videoBitrate", s.videoBitrate);
  writeKeyInt(f, "sensorMode", static_cast<int>(s.sensorMode));
  // bracketEv is a vector — serialize as a comma-separated list
  f << "bracketEv=";
  for (size_t i = 0; i < s.bracketEv.size(); ++i) {
    if (i > 0)
      f << ",";
    f << s.bracketEv[i];
  }
  f << "\n";
  return f.good();
}

bool loadSettings(CameraSettings &s, const std::string &path) {
  std::ifstream f(path);
  if (!f.is_open())
    return false;
  std::string line;
  while (std::getline(f, line)) {
    std::string_view sv(line);
    sv = trimSv(sv);
    if (sv.empty() || sv.front() == '#')
      continue;
    auto eq = sv.find('=');
    if (eq == std::string_view::npos)
      continue;
    std::string_view key = trimSv(sv.substr(0, eq));
    std::string_view val = trimSv(sv.substr(eq + 1));
    if (key.empty())
      continue;
    if (key == "bracketEv") {
      s.bracketEv.clear();
      size_t start = 0;
      while (start <= val.size()) {
        auto comma = val.find(',', start);
        std::string_view tok = (comma == std::string_view::npos)
                                   ? val.substr(start)
                                   : val.substr(start, comma - start);
        if (!tok.empty()) {
          try {
            s.bracketEv.push_back(std::stof(std::string(tok)));
          } catch (...) {
            continue;
          } // skip unparseable token
        }
        if (comma == std::string_view::npos)
          break;
        start = comma + 1;
      }
    } else {
      applySettingsKey(s, key, val);
    }
  }
  // Re-apply picture style preset to B/C/S/Sharp — selecting a style resets
  // those values to the style defaults, matching real camera behavior.
  auto params = pictureStyleParams(s.pictureStyle);
  s.brightness = params.brightness;
  s.contrast = params.contrast;
  s.saturation = params.saturation;
  s.sharpness = params.sharpness;
  return true;
}

std::string defaultSettingsPath() {
  const char *home = std::getenv("HOME");
  if (!home)
    home = "/tmp";
  return std::string(home) + "/.config/picamera/settings.conf";
}

std::string customModePath(int slot) {
  const char *home = std::getenv("HOME");
  if (!home)
    home = "/tmp";
  return std::string(home) + "/.config/picamera/custom_c" +
         std::to_string(slot) + ".conf";
}

bool saveCustomMode(const CameraSettings &s, int slot) {
  if (slot < 1 || slot > 3)
    return false;
  return saveSettings(s, customModePath(slot));
}

bool loadCustomMode(CameraSettings &s, int slot) {
  if (slot < 1 || slot > 3)
    return false;
  return loadSettings(s, customModePath(slot));
}

} // namespace picamera
