#include "settings_menu.h"
#include "util.h"

#include <algorithm>
#include <array>
#include <cmath>
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

constexpr int kDriveModeCount = 4;
constexpr int kMeteringModeCount = 3;
constexpr int kAeExposureModeCount = 3;
constexpr int kAeConstraintModeCount = 3;
constexpr int kNoiseReductionModeCount = 4;
constexpr int kAspectRatioCount = 4;
constexpr int kGridTypeCount = 3;
constexpr int kZebraModeCount = 4;

template <typename E>
E cycleEnum(E value, int direction, int count) {
    return static_cast<E>((static_cast<int>(value) + direction + count) % count);
}

constexpr uint64_t kShutterSteps[] = {
    0,
    60,
    125,
    250,
    500,
    1000,
    2000,
    4000,
    8000,
    16667,
    33333,
    66667,
    125000,
    250000,
    500000,
    1000000,
    2000000,
    4000000,
    8000000,
    15000000,
    30000000,
    60000000,
    120000000,
    300000000,
};
constexpr int kShutterStepCount = static_cast<int>(std::size(kShutterSteps));

constexpr float kIsoSteps[] = {
    0,
    100,
    200,
    400,
    800,
    1600,
    3200,
};
constexpr int kIsoStepCount = static_cast<int>(std::size(kIsoSteps));

constexpr float kEvSteps[] = {
    -3.0f, -2.67f, -2.33f, -2.0f, -1.67f, -1.33f, -1.0f,
    -0.67f, -0.33f, 0.0f,
    0.33f, 0.67f, 1.0f, 1.33f, 1.67f, 2.0f, 2.33f, 2.67f, 3.0f,
};
constexpr int kEvStepCount = static_cast<int>(std::size(kEvSteps));

std::string formatShutter(uint64_t us) {
    if (us == 0) return "AUTO";
    if (us < kMicrosPerMillis) return std::to_string(us) + "us";
    if (us < kMicrosPerSec) {
        uint64_t denom = kMicrosPerSec / us;
        if (kMicrosPerSec % us > us / 2) ++denom;
        if (denom <= 1) return "1\"";
        return "1/" + std::to_string(denom);
    }
    uint64_t secs = us / kMicrosPerSec;
    if (secs < 60) return std::to_string(secs) + "\"";
    uint64_t mins = secs / 60;
    return std::to_string(mins) + "'";
}

std::string fmt1(float v) {
    int tenths = static_cast<int>(std::round(v * 10.0f));
    int sign = tenths < 0 ? -1 : 1;
    tenths = std::abs(tenths);
    std::string s;
    if (sign < 0) s += '-';
    s += std::to_string(tenths / 10);
    s += '.';
    s += std::to_string(tenths % 10);
    return s;
}

std::string fmtEv(float ev) {
    if (ev == 0) return "0";
    std::string s = (ev > 0) ? "+" : "-";
    s += fmt1(std::abs(ev));
    return s;
}

template <typename T>
int nearestIndex(const T *steps, int count, T value) {
    for (int i = 0; i < count; ++i)
        if (steps[i] == value) return i;
    int best = 0;
    auto bestDiff = std::numeric_limits<T>::max();
    for (int i = 0; i < count; ++i) {
        auto diff = (steps[i] > value) ? steps[i] - value : value - steps[i];
        if (diff < bestDiff) { bestDiff = diff; best = i; }
    }
    return best;
}

int findShutterIdx(uint64_t us) {
    return nearestIndex(kShutterSteps, kShutterStepCount, us);
}

int findIsoIdx(float gain) {
    return nearestIndex(kIsoSteps, kIsoStepCount, gain * kIsoScaleFactor);
}

int findEvIdx(float ev) {
    return nearestIndex(kEvSteps, kEvStepCount, ev);
}

const char *driveModeLabel(DriveMode d) {
    switch (d) {
        case DriveMode::Single:    return "SINGLE";
        case DriveMode::SelfTimer: return "SELF";
        case DriveMode::Bracket:   return "BRACKET";
        case DriveMode::Timelapse: return "LAPSE";
    }
    return "??";
}

const char *meteringLabel(MeteringMode m) {
    switch (m) {
        case MeteringMode::Matrix:  return "MATRIX";
        case MeteringMode::Centre:  return "CENTRE";
        case MeteringMode::Spot:    return "SPOT";
    }
    return "??";
}

const char *aeExposureLabel(AeExposureMode m) {
    switch (m) {
        case AeExposureMode::Normal: return "NORMAL";
        case AeExposureMode::Short:  return "SHORT";
        case AeExposureMode::Long:   return "LONG";
    }
    return "??";
}

const char *aeConstraintLabel(AeConstraintMode m) {
    switch (m) {
        case AeConstraintMode::Normal:    return "NORMAL";
        case AeConstraintMode::Highlight: return "HIGHLIGHT";
        case AeConstraintMode::Shadows:   return "SHADOWS";
    }
    return "??";
}

const char *formatLabel(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::JPEG:     return "JPEG";
        case OutputFormat::PNG:      return "PNG";
        case OutputFormat::DNG:      return "DNG";
        case OutputFormat::RAW_NV12: return "RAW";
        case OutputFormat::PPM:      return "PPM";
    }
    return "??";
}

const char *gridLabel(GridType g) {
    switch (g) {
        case GridType::Off:     return "OFF";
        case GridType::Thirds:  return "3RD";
        case GridType::Square:  return "SQ";
    }
    return "??";
}

const char *zebraLabel(ZebraMode z) {
    switch (z) {
        case ZebraMode::Off:          return "OFF";
        case ZebraMode::Threshold70:  return "70%";
        case ZebraMode::Threshold80:  return "80%";
        case ZebraMode::Threshold100: return "100%";
    }
    return "??";
}

const char *aspectLabel(AspectRatio a) {
    switch (a) {
        case AspectRatio::Native:  return "3:2";
        case AspectRatio::Ratio43: return "4:3";
        case AspectRatio::Ratio169:return "16:9";
        case AspectRatio::Ratio11: return "1:1";
    }
    return "??";
}

const char *nrLabel(NoiseReductionMode n) {
    switch (n) {
        case NoiseReductionMode::Off:         return "OFF";
        case NoiseReductionMode::Fast:        return "FAST";
        case NoiseReductionMode::HighQuality: return "HQ";
        case NoiseReductionMode::Minimal:     return "MIN";
    }
    return "??";
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
    s.aeExposureMode = cycleEnum(s.aeExposureMode, direction, kAeExposureModeCount);
}

void adjustAeConst(CameraSettings &s, int direction) {
    s.aeConstraintMode = cycleEnum(s.aeConstraintMode, direction, kAeConstraintModeCount);
}

void adjustFlicker(CameraSettings &s, int direction) {
    enum FlickerState : int { Off, Hz50, Hz60 };
    FlickerState state = s.antiFlicker ? (s.flickerHz == 60 ? Hz60 : Hz50) : Off;
    if (direction > 0) state = static_cast<FlickerState>((state + 1) % 3);
    else if (state > Off) state = static_cast<FlickerState>(state - 1);
    if (state == Off) s.antiFlicker = false;
    else { s.antiFlicker = true; s.flickerHz = (state == Hz50) ? 50 : 60; }
}

void adjustTimer(CameraSettings &s, int direction) {
    if (direction > 0) { if (s.timerDuration < 10) ++s.timerDuration; }
    else { if (s.timerDuration > 0) --s.timerDuration; }
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
        for (float ev : s.bracketEv) maxEv = std::max(maxEv, std::abs(ev));
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
        OutputFormat::JPEG, OutputFormat::DNG, OutputFormat::PNG,
        OutputFormat::PPM, OutputFormat::RAW_NV12
    };
    constexpr int n = static_cast<int>(std::size(kFmtOrder));
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (s.captureFormat == kFmtOrder[i]) { idx = i; break; }
    s.captureFormat = kFmtOrder[(idx + direction + n) % n];
}

void adjustImgQuality(CameraSettings &s, int direction) {
    if (direction > 0) { if (s.jpegQuality < kJpegQualityMax) ++s.jpegQuality; }
    else { if (s.jpegQuality > kMinJpegQuality) --s.jpegQuality; }
}

void adjustImgAspect(CameraSettings &s, int direction) {
    s.aspectRatio = cycleEnum(s.aspectRatio, direction, kAspectRatioCount);
}

void adjustImgAwb(CameraSettings &s, int direction) {
    static constexpr std::string_view modes[] = {
        "auto", "daylight", "cloudy", "incandescent",
        "tungsten", "fluorescent", "indoor"
    };
    constexpr int n = static_cast<int>(std::size(modes));
    if (!s.awbEnable) {
        s.awbEnable = true;
        s.awbMode = (direction > 0)
            ? std::string(modes[0]) : std::string(modes[n - 1]);
    } else {
        int idx = -1;
        for (int i = 0; i < n; ++i)
            if (s.awbMode == modes[i]) { idx = i; break; }
        if (direction > 0) {
            if (idx < 0) s.awbMode = std::string(modes[0]);
            else if (idx < n - 1) s.awbMode = std::string(modes[idx + 1]);
            else s.awbEnable = false;
        } else {
            if (idx < 0) s.awbMode = std::string(modes[0]);
            else if (idx > 0) s.awbMode = std::string(modes[idx - 1]);
            else s.awbEnable = false;
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
    s.sharpness = std::clamp(s.sharpness + direction * kSharpnessStep, 0.0f, kMaxSharpness);
}

void adjustImgNr(CameraSettings &s, int direction) {
    s.noiseReduction = cycleEnum(s.noiseReduction, direction, kNoiseReductionModeCount);
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
    if (direction > 0) { if (s.displayBrightness < kMaxDisplayBrightness) s.displayBrightness += kDisplayBrightnessStep; }
    else { if (s.displayBrightness > kMinDisplayBrightness) s.displayBrightness -= kDisplayBrightnessStep; }
}

void adjustSysBattery(CameraSettings &s, int direction) {
    s.enableBattery = (direction > 0);
}

void adjustSysPowerSave(CameraSettings &s, int direction) {
    static constexpr int steps[] = {30, 0, 300, 60};
    constexpr int n = static_cast<int>(std::size(steps));
    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (s.powerSaveTimeout == steps[i]) { idx = i; break; }
    if (direction > 0)
        s.powerSaveTimeout = steps[(idx + n - 1) % n];
    else
        s.powerSaveTimeout = steps[(idx + 1) % n];
}

std::string_view valDrive(const CameraSettings &s, std::string &) {
    return driveModeLabel(s.driveMode);
}

std::string_view valShutter(const CameraSettings &s, std::string &buf) {
    buf = formatShutter(s.shutterUs);
    return buf;
}

std::string_view valIso(const CameraSettings &s, std::string &buf) {
    if (s.analogueGain == 0) return "AUTO";
    buf = std::to_string(static_cast<int>(s.analogueGain * kIsoScaleFactor));
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
    if (!s.antiFlicker) return "OFF";
    buf = std::to_string(s.flickerHz) + "Hz";
    return buf;
}

std::string_view valTimer(const CameraSettings &s, std::string &buf) {
    if (s.timerDuration == 0) return "OFF";
    buf = std::to_string(s.timerDuration) + "S";
    return buf;
}

std::string_view valBracket(const CameraSettings &s, std::string &buf) {
    if (s.bracketEv.empty()) return "OFF";
    float maxEv = 0;
    for (float ev : s.bracketEv) maxEv = std::max(maxEv, std::abs(ev));
    buf = "+-" + fmt1(maxEv) + "EV";
    return buf;
}

std::string_view valInterval(const CameraSettings &s, std::string &buf) {
    buf = std::to_string(s.timelapseInterval) + "S";
    return buf;
}

std::string_view valCount(const CameraSettings &s, std::string &buf) {
    if (s.timelapseCount == 0) return "INF";
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
    if (s.powerSaveTimeout == 0) return "OFF";
    buf = std::to_string(s.powerSaveTimeout) + "S";
    return buf;
}

std::string_view valExit(const CameraSettings &, std::string &) {
    return "";
}

struct SettingItem {
    std::string_view label;
    std::string_view (*valueFn)(const CameraSettings &, std::string &buf);
    void (*adjustFn)(CameraSettings &, int);
};

constexpr std::array<SettingItem, 12> kShootTab = {{
    {"DRIVE",    valDrive,    adjustDrive},
    {"SHUTTER",  valShutter,  adjustShutter},
    {"ISO",      valIso,      adjustIso},
    {"EV",       valEv,       adjustEv},
    {"METER",    valMeter,    adjustMeter},
    {"AEMODE",   valAeMode,   adjustAeMode},
    {"AECONST",  valAeConst,  adjustAeConst},
    {"FLICKER",  valFlicker,  adjustFlicker},
    {"TIMER",    valTimer,    adjustTimer},
    {"BRACKET",  valBracket,  adjustBracket},
    {"INTERVAL", valInterval, adjustInterval},
    {"COUNT",    valCount,    adjustCount},
}};

constexpr std::array<SettingItem, 12> kImgTab = {{
    {"FORMAT",   valFormat,     adjustImgFormat},
    {"QUALITY",  valQuality,    adjustImgQuality},
    {"ASPECT",   valAspect,     adjustImgAspect},
    {"AWB",      valAwb,        adjustImgAwb},
    {"KELVIN",   valKelvin,     adjustImgKelvin},
    {"WBRED",    valWbRed,      adjustImgWbRed},
    {"WBBLUE",   valWbBlue,     adjustImgWbBlue},
    {"BRIGHT",   valBrightness, adjustImgBrightness},
    {"CONTRAST", valContrast,   adjustImgContrast},
    {"SAT",      valSaturation, adjustImgSaturation},
    {"SHARP",    valSharpness,  adjustImgSharpness},
    {"NR",       valNr,         adjustImgNr},
}};

constexpr std::array<SettingItem, 5> kDispTab = {{
    {"GRID",   valDispGrid,   adjustDispGrid},
    {"HIST",   valDispHist,   adjustDispHist},
    {"ZEBRA",  valDispZebra,  adjustDispZebra},
    {"PEAK",   valDispPeak,   adjustDispPeak},
    {"BRIGHT", valDispBright, adjustDispBright},
}};

constexpr std::array<SettingItem, 3> kSysTab = {{
    {"BATTERY", valSysBattery,  adjustSysBattery},
    {"PWRSAVE", valPowerSave,   adjustSysPowerSave},
    {"EXIT",    valExit,        nullptr},
}};

std::span<const SettingItem> tabItems(SettingsTab tab) {
    switch (tab) {
        case SettingsTab::Shooting: return kShootTab;
        case SettingsTab::Image:    return kImgTab;
        case SettingsTab::Display:  return kDispTab;
        case SettingsTab::System:   return kSysTab;
    }
    return {};
}

void adjustSetting(SettingsTab tab, int item, CameraSettings &s, int direction) {
    auto items = tabItems(tab);
    if (item >= 0 && item < static_cast<int>(items.size()) && items[item].adjustFn)
        items[item].adjustFn(s, direction);
}

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

std::string settingsItemValue(SettingsTab tab, int item, const CameraSettings &s) {
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
           before.aspectRatio != after.aspectRatio;
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
    cfg.flickerPeriodUs = s.antiFlicker ?
        (s.flickerHz == 50 ? kFlicker50HzPeriodUs : kFlicker60HzPeriodUs) : 0;
    cfg.wbRedGain = s.wbRedGain;
    cfg.wbBlueGain = s.wbBlueGain;
    cfg.noiseReductionMode = s.noiseReduction;

    return cfg;
}

}
