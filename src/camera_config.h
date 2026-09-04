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
};

inline std::optional<OutputFormat> parseOutputFormat(std::string_view name) {
    auto ieq = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(),
                          [](char x, char y) {
                              return std::tolower(static_cast<unsigned char>(x)) ==
                                     std::tolower(static_cast<unsigned char>(y));
                          });
    };
    if (ieq(name, "ppm")) return OutputFormat::PPM;
    if (ieq(name, "raw")) return OutputFormat::RAW_NV12;
    if (ieq(name, "png")) return OutputFormat::PNG;
    if (ieq(name, "jpeg") || ieq(name, "jpg")) return OutputFormat::JPEG;
    if (ieq(name, "dng")) return OutputFormat::DNG;
    return std::nullopt;
}

inline std::string_view extensionFor(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::PPM:      return "ppm";
        case OutputFormat::RAW_NV12: return "raw";
        case OutputFormat::PNG:      return "png";
        case OutputFormat::JPEG:     return "jpg";
        case OutputFormat::DNG:      return "dng";
    }
    return "bin";
}

inline constexpr std::string_view kAwbModes[] = {
    "auto", "incandescent", "tungsten", "fluorescent",
    "indoor", "daylight", "cloudy",
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
    NoiseReductionMode noiseReductionMode = NoiseReductionMode::Fast;
};

} // namespace picamera
