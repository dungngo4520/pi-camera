#include "controls.h"

#include <algorithm>
#include <climits>
#include <optional>
#include <string_view>

namespace picamera {

using libcamera::ControlList;
using libcamera::Span;
namespace controls = libcamera::controls;

namespace {

struct AwbEntry {
    const char *name;
    controls::AwbModeEnum mode;
};
constexpr AwbEntry kAwbTable[] = {
    {"auto",         controls::AwbAuto},
    {"incandescent", controls::AwbIncandescent},
    {"tungsten",     controls::AwbTungsten},
    {"fluorescent",  controls::AwbFluorescent},
    {"indoor",       controls::AwbIndoor},
    {"daylight",     controls::AwbDaylight},
    {"cloudy",       controls::AwbCloudy},
};

}

std::optional<libcamera::controls::AwbModeEnum> lookupAwb(std::string_view name) {
    for (const auto &e : kAwbTable) {
        if (name == e.name) return e.mode;
    }
    return std::nullopt;
}

void applyCameraControls(ControlList &ctrls, const CameraConfig &cfg,
                         const ExposureOverride &ovr) {
    const bool manualExposure = ovr.force || !cfg.aeEnable ||
                                cfg.exposureTime > 0 || cfg.analogueGain > 0.0f;
    if (manualExposure) {
        ctrls.set(controls::AeEnable, false);
        int32_t expTime = ovr.force
                              ? ovr.expTime
                              : static_cast<int32_t>(
                                    std::min<uint64_t>(cfg.exposureTime, INT32_MAX));
        if (expTime > 0) {
            ctrls.set(controls::ExposureTime, expTime);
        }
        float gain = ovr.force ? ovr.gain : cfg.analogueGain;
        if (gain > 0.0f) {
            ctrls.set(controls::AnalogueGain, gain);
        }
    } else {
        ctrls.set(controls::AeEnable, true);
        ctrls.set(controls::AeMeteringMode,
                  static_cast<int32_t>(cfg.meteringMode));
        ctrls.set(controls::AeExposureMode,
                  static_cast<int32_t>(cfg.aeExposureMode));
        ctrls.set(controls::AeConstraintMode,
                  static_cast<int32_t>(cfg.aeConstraintMode));
        if (cfg.exposureValue != 0.0f) {
            ctrls.set(controls::ExposureValue, cfg.exposureValue);
        }
        if (cfg.antiFlicker && cfg.flickerPeriodUs > 0) {
            ctrls.set(controls::AeFlickerMode, static_cast<int32_t>(1));
            ctrls.set(controls::AeFlickerPeriod,
                      static_cast<int32_t>(cfg.flickerPeriodUs));
        }
    }
    if (cfg.digitalGain > 0.0f) {
        ctrls.set(controls::DigitalGain, cfg.digitalGain);
    }
    if (!cfg.awbEnable) {
        ctrls.set(controls::AwbEnable, false);
        const float gains[2] = {cfg.wbRedGain, cfg.wbBlueGain};
        ctrls.set(controls::ColourGains, Span<const float, 2>(gains, 2));
    } else {
        ctrls.set(controls::AwbEnable, true);
        if (auto mode = lookupAwb(cfg.awbMode)) {
            ctrls.set(controls::AwbMode, *mode);
        }
    }
    ctrls.set(controls::Brightness, cfg.brightness);
    ctrls.set(controls::Contrast, cfg.contrast);
    ctrls.set(controls::Saturation, cfg.saturation);
    ctrls.set(controls::Sharpness, cfg.sharpness);
    ctrls.set(controls::draft::NoiseReductionMode,
              static_cast<int32_t>(cfg.noiseReductionMode));
}

}
