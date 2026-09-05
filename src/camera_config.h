#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace picamera {

constexpr uint32_t kMaxSensorWidth = 4056;
constexpr uint32_t kMaxSensorHeight = 3040;

constexpr int kJpegQualityMin = 1;
constexpr int kJpegQualityMax = 100;

enum class MeteringMode {
  Centre = 0,
  Spot = 1,
  Matrix = 2,
};

enum class AeExposureMode {
  Normal = 0,
  Short = 1,
  Long = 2,
};

enum class AeConstraintMode {
  Normal = 0,
  Highlight = 1,
  Shadows = 2,
};

enum class NoiseReductionMode {
  Off = 0,
  Fast = 1,
  HighQuality = 2,
  Minimal = 3,
};

enum class OutputFormat {
  PPM,
  RAW_NV12,
  PNG,
  JPEG,
  DNG,
  RawJpeg,
  DngJpeg, // Sequential DNG + JPEG (true RAW+JPEG like a mirrorless camera)
};

enum class ImageSize {
  Large = 0,
  Medium = 1,
  Small = 2,
};

enum class AspectRatio {
  Native,
  Ratio43,
  Ratio169,
  Ratio11,
};

inline std::optional<OutputFormat> parseOutputFormat(std::string_view name) {
  auto ieq = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) ==
                    std::tolower(static_cast<unsigned char>(y));
           });
  };
  if (ieq(name, "ppm"))
    return OutputFormat::PPM;
  if (ieq(name, "raw"))
    return OutputFormat::RAW_NV12;
  if (ieq(name, "png"))
    return OutputFormat::PNG;
  if (ieq(name, "jpeg") || ieq(name, "jpg"))
    return OutputFormat::JPEG;
  if (ieq(name, "dng"))
    return OutputFormat::DNG;
  if (ieq(name, "raw+jpeg") || ieq(name, "rawjpeg"))
    return OutputFormat::RawJpeg;
  if (ieq(name, "dng+jpeg") || ieq(name, "dngjpeg"))
    return OutputFormat::DngJpeg;
  return std::nullopt;
}

inline std::string_view extensionFor(OutputFormat fmt) {
  switch (fmt) {
  case OutputFormat::PPM:
    return "ppm";
  case OutputFormat::RAW_NV12:
    return "raw";
  case OutputFormat::PNG:
    return "png";
  case OutputFormat::JPEG:
    return "jpg";
  case OutputFormat::DNG:
    return "dng";
  case OutputFormat::RawJpeg:
  case OutputFormat::DngJpeg:
    // Both produce a JPEG primary file. RawJpeg also saves raw NV12;
    // DngJpeg also saves a DNG (with a different stem extension).
    return "jpg";
  }
  return "bin";
}

inline constexpr std::string_view kAwbModes[] = {
    "auto",     "incandescent", "tungsten", "fluorescent", "indoor",
    "daylight", "cloudy",       "shade",    "flash",
};

inline bool isValidAwbMode(std::string_view name) {
  return std::any_of(std::begin(kAwbModes), std::end(kAwbModes),
                     [&](std::string_view m) { return name == m; });
}

struct CameraConfig {
  uint32_t width = kMaxSensorWidth;
  uint32_t height = kMaxSensorHeight;
  uint64_t exposureTime = 0;
  float analogueGain = 0;
  float digitalGain = 0;
  std::string awbMode = "auto";
  bool aeEnable = true;
  bool awbEnable = true;
  OutputFormat format = OutputFormat::PPM;
  int pngLevel = 6;
  int jpegQuality = 90;
  uint32_t warmupFrames = 8;
  std::vector<float> bracketEv;

  float exposureValue = 0;
  MeteringMode meteringMode = MeteringMode::Matrix;
  AeExposureMode aeExposureMode = AeExposureMode::Normal;
  AeConstraintMode aeConstraintMode = AeConstraintMode::Normal;
  float brightness = 0;
  float contrast = 1.0;
  float saturation = 1.0;
  float sharpness = 1.0;
  bool antiFlicker = false;
  int flickerPeriodUs = 0;
  float wbRedGain = 1.0;
  float wbBlueGain = 1.0;
  int wbKelvin = 0;
  NoiseReductionMode noiseReductionMode = NoiseReductionMode::Fast;
  ImageSize imageSize = ImageSize::Large;
  AspectRatio aspectRatio = AspectRatio::Native;

  // Auto-ISO range (min/max ISO when AE is on). libcamera does not expose
  // AeAnalogueGainMin/Max controls, so these cannot constrain the AE
  // algorithm's gain selection directly. They ARE applied, however, when
  // manual gain is set: analogueGain is clamped to
  // [isoMin/100.0, isoMax/100.0] in applyControls()/applyCameraControls()
  // (ISO = analogueGain * 100, so ISO 100 = 1.0x gain). This enforces the
  // user's chosen range in Manual exposure mode. See clampGainToIsoRange().
  int isoMin = 100;
  int isoMax = 3200;

  // Color space tag for JPEG EXIF / DNG metadata (output is effectively
  // sRGB on this pipeline; AdobeRGB tags the metadata accordingly).
  int colorSpace = 0; // 0 = sRGB, 1 = AdobeRGB
  std::string copyright; // embedded in EXIF Copyright tag / DNG
  uint64_t minShutterUs = 0; // minimum shutter for auto ISO (0 = auto)
  float wbGmShift = 0.0f; // green-magenta WB shift (-9 to +9)
  bool grainEffect = false; // film grain overlay on JPEG encode
};

// Clamp a manual analogue gain to the [isoMin, isoMax] ISO range.
// ISO = analogueGain * 100 (1.0x = ISO 100, 4.0x = ISO 400), so the gain
// bounds are isoMin/100.0 and isoMax/100.0. A non-positive gain (meaning
// "auto/unset") is returned unchanged — only explicit manual gains are
// constrained. Pure logic (unit-testable, no libcamera dependency).
inline float clampGainToIsoRange(float gain, int isoMin, int isoMax) {
  if (gain <= 0.0f)
    return gain;
  float lo = static_cast<float>(isoMin) / 100.0f;
  float hi = static_cast<float>(isoMax) / 100.0f;
  hi = std::max(hi, lo); // defensive: ensure a valid range
  return std::clamp(gain, lo, hi);
}

} // namespace picamera
