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
    auto boolKey = [&](const char *name, bool &field) {
        auto b = parseBool(val);
        if (b) field = *b;
        else std::cerr << path << ":" << lineNum
                      << ": invalid " << name << " (true/false)\n";
    };
    auto uintKey = [&](const char *name, uint32_t &field, uint32_t lo, uint32_t hi) {
        auto u = parseUint32(val);
        if (u && *u >= lo && *u <= hi) field = *u;
        else std::cerr << path << ":" << lineNum << ": invalid " << name << "\n";
    };

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
        else std::cerr << path << ":" << lineNum << ": invalid battery_addr\n";
    } else if (key == "capture_dir") {
        cfg.captureDir = std::string(val);
    } else if (key == "capture_prefix") {
        cfg.capturePrefix = std::string(val);
    } else if (key == "preview_width") {
        uintKey("preview_width", cfg.previewWidth, 1, kMaxSensorWidth);
    } else if (key == "preview_height") {
        uintKey("preview_height", cfg.previewHeight, 1, kMaxSensorHeight);
    } else if (key == "preview_fps" || key == "max_fps") {
        auto fps = parseUint32(val);
        if (fps && *fps >= 1 && *fps <= 120) cfg.maxFps = *fps;
        else std::cerr << path << ":" << lineNum
                      << ": invalid preview_fps (1-120)\n";
    } else if (key == "capture_width") {
        uintKey("capture_width", cfg.captureWidth, 1, kMaxSensorWidth);
    } else if (key == "capture_height") {
        uintKey("capture_height", cfg.captureHeight, 1, kMaxSensorHeight);
    } else if (key == "enable_battery") {
        boolKey("enable_battery", cfg.enableBattery);
    } else if (key == "wifi_enabled") {
        boolKey("wifi_enabled", cfg.wifiEnabled);
    } else if (key == "bt_enabled") {
        boolKey("bt_enabled", cfg.btEnabled);
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
            std::cerr << path << ":" << lineNum << ": missing '=' — skipping line\n";
            continue;
        }
        std::string_view key = trim(sv.substr(0, eq));
        std::string_view val = trim(sv.substr(eq + 1));
        if (key.empty() || val.empty()) {
            std::cerr << path << ":" << lineNum << ": empty key or value — skipping line\n";
            continue;
        }

        applyHardwareKey(cfg, key, val, path, lineNum);
    }
    cfg.loaded = true;
    return true;
}

HardwareConfig loadHardwareConfig(const std::string &path) {
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    return cfg;
}

}
