#pragma once

// White-balance pure-logic helpers (libcamera-free).
//
// lookupAwbMode() maps an AWB mode name to an integer enum value mirroring
// libcamera::controls::AwbModeEnum (see libcamera/control_ids.h).
// kelvinToGains() converts a colour temperature to approximate R/B gains. Both
// are pure logic with no libcamera dependency, so they live in this header —
// which is part of the picamera_core static library — and are unit-testable on
// x86 builds that do not have libcamera installed.
//
// controls.cpp wraps lookupAwbMode() in lookupAwb() (returning the real
// libcamera enum) for use by the hardware TUs that link libcamera.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace picamera {

// AWB mode values mirroring libcamera::controls::AwbModeEnum (see
// libcamera/control_ids.h). The integer values must match so the static_cast
// in controls.cpp's lookupAwb() preserves the original behaviour exactly.
enum AwbModeValue : int32_t {
  AwbAuto = 0,
  AwbIncandescent = 1,
  AwbTungsten = 2,
  AwbFluorescent = 3,
  AwbIndoor = 4,
  AwbDaylight = 5,
  AwbCloudy = 6,
  AwbCustom = 7,
};

// Map an AWB mode name to its enum value, or std::nullopt if unknown.
// "shade" and "flash" both map to AwbDaylight (matching the original
// controls.cpp lookupAwb() table). Pure logic (unit-testable on x86).
inline std::optional<AwbModeValue> lookupAwbMode(std::string_view name) {
  struct AwbEntry {
    const char *name;
    AwbModeValue mode;
  };
  constexpr AwbEntry kAwbTable[] = {
      {"auto", AwbModeValue::AwbAuto},
      {"incandescent", AwbModeValue::AwbIncandescent},
      {"tungsten", AwbModeValue::AwbTungsten},
      {"fluorescent", AwbModeValue::AwbFluorescent},
      {"indoor", AwbModeValue::AwbIndoor},
      {"daylight", AwbModeValue::AwbDaylight},
      {"cloudy", AwbModeValue::AwbCloudy},
      {"shade", AwbModeValue::AwbDaylight},
      {"flash", AwbModeValue::AwbDaylight},
  };
  for (const auto &e : kAwbTable) {
    if (name == e.name)
      return e.mode;
  }
  return std::nullopt;
}

// Convert a colour temperature (Kelvin) to approximate R/B gains using a
// simplified blackbody curve. D65 (6500K) ≈ neutral (1.0, 1.0).
// Lower K (warm) -> more red, less blue. Higher K (cool) -> less red, more
// blue. Gains are clamped to [0.1, 8.0]. Pure logic (unit-testable on x86).
inline void kelvinToGains(int kelvin, float &redGain, float &blueGain) {
  float t = static_cast<float>(kelvin) / 100.0f;
  float r;
  float b;
  if (t <= 66.0f) {
    r = 255.0f;
  } else {
    r = 329.6987f * std::pow(t - 60.0f, -0.1332047f);
  }
  if (t >= 66.0f) {
    b = 255.0f;
  } else if (t <= 19.0f) {
    b = 0.0f;
  } else {
    b = 138.5177f * std::pow(t - 10.0f, -0.0755148f);
  }
  // Normalize to 0-1 relative to green (≈1.0); 6500K gives ~1.0 for both.
  r = std::clamp(r, 0.0f, 255.0f) / 255.0f;
  b = std::clamp(b, 0.0f, 255.0f) / 255.0f;
  redGain = std::clamp(r, 0.1f, 8.0f);
  blueGain = std::clamp(b, 0.1f, 8.0f);
}

} // namespace picamera
