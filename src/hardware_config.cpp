#include "hardware_config.h"
#include "camera_config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

namespace picamera {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

std::optional<bool> parseBool(std::string_view v) {
    auto ieq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    };
    if (ieq(v, "true") || ieq(v, "1") || ieq(v, "yes") || ieq(v, "on"))
        return true;
    if (ieq(v, "false") || ieq(v, "0") || ieq(v, "no") || ieq(v, "off"))
        return false;
    return std::nullopt;
}

std::optional<uint32_t> parseUint32(std::string_view v) {
    if (v.empty()) return std::nullopt;
    uint32_t result = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return std::nullopt;
        uint32_t digit = static_cast<uint32_t>(c - '0');
        if (result > (UINT32_MAX - digit) / 10) return std::nullopt;
        result = result * 10 + digit;
    }
    return result;
}

std::optional<uint8_t> parseAddr(std::string_view v) {
    if (v.empty()) return std::nullopt;
    std::string s(v);
    errno = 0;
    char *end = nullptr;
    const char *start;
    if (s.size() >= 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        start = s.c_str() + 2;
    } else {
        start = s.c_str();
    }
    if (*start == '\0') return std::nullopt;
    long val = std::strtol(start, &end, (start != s.c_str()) ? 16 : 10);
    if (errno != 0 || *end != '\0' || val < 0 || val > 0xFF) return std::nullopt;
    return static_cast<uint8_t>(val);
}

std::optional<int> parseRotation(std::string_view v) {
    auto u = parseUint32(v);
    if (!u) return std::nullopt;
    switch (*u) {
        case 0: case 90: case 180: case 270: return static_cast<int>(*u);
        default: return std::nullopt;
    }
}

}

void applyHardwareKey(HardwareConfig &cfg, std::string_view key,
                      std::string_view val, const std::string &path, int lineNum) {
    if (key == "spi_device") {
        cfg.spiDevice = std::string(val);
    } else if (key == "display_rotate") {
        auto r = parseRotation(val);
        if (r) cfg.displayRotation = *r;
        else std::cerr << path << ":" << lineNum
                      << ": invalid display_rotate (must be 0/90/180/270)\n";
    } else if (key == "battery_i2c") {
        cfg.batteryI2cDevice = std::string(val);
    } else if (key == "battery_addr") {
        auto a = parseAddr(val);
        if (a) cfg.batteryI2cAddress = *a;
        else std::cerr << path << ":" << lineNum
                      << ": invalid battery_addr\n";
    } else if (key == "capture_dir") {
        cfg.captureDir = std::string(val);
    } else if (key == "capture_prefix") {
        cfg.capturePrefix = std::string(val);
    } else if (key == "preview_width") {
        auto w = parseUint32(val);
        if (w && *w > 0 && *w <= kMaxSensorWidth) cfg.previewWidth = *w;
        else std::cerr << path << ":" << lineNum
                      << ": invalid preview_width\n";
    } else if (key == "preview_height") {
        auto h = parseUint32(val);
        if (h && *h > 0 && *h <= kMaxSensorHeight) cfg.previewHeight = *h;
        else std::cerr << path << ":" << lineNum
                      << ": invalid preview_height\n";
    } else if (key == "preview_fps") {
        auto fps = parseUint32(val);
        if (fps && *fps >= 1 && *fps <= 120) cfg.previewFps = *fps;
        else std::cerr << path << ":" << lineNum
                      << ": invalid preview_fps (1-120)\n";
    } else if (key == "capture_width") {
        auto w = parseUint32(val);
        if (w && *w > 0 && *w <= kMaxSensorWidth) cfg.captureWidth = *w;
        else std::cerr << path << ":" << lineNum
                      << ": invalid capture_width\n";
    } else if (key == "capture_height") {
        auto h = parseUint32(val);
        if (h && *h > 0 && *h <= kMaxSensorHeight) cfg.captureHeight = *h;
        else std::cerr << path << ":" << lineNum
                      << ": invalid capture_height\n";
    } else if (key == "enable_battery") {
        auto b = parseBool(val);
        if (b) cfg.enableBattery = *b;
        else std::cerr << path << ":" << lineNum
                      << ": invalid enable_battery (true/false)\n";
    }
}

bool loadHardwareConfig(const std::string &path, HardwareConfig &cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return false;

    std::string line;
    int lineNum = 0;
    while (std::getline(f, line)) {
        ++lineNum;
        std::string_view sv(line);
        sv = trim(sv);
        if (sv.empty() || sv.front() == '#') continue;

        auto eq = sv.find('=');
        if (eq == std::string_view::npos) {
            std::cerr << path << ":" << lineNum
                      << ": missing '=' — skipping line\n";
            continue;
        }
        std::string_view key = trim(sv.substr(0, eq));
        std::string_view val = trim(sv.substr(eq + 1));
        if (key.empty() || val.empty()) {
            std::cerr << path << ":" << lineNum
                      << ": empty key or value — skipping line\n";
            continue;
        }

        applyHardwareKey(cfg, key, val, path, lineNum);
    }
    return true;
}

}
