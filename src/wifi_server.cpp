#include "wifi_server.h"
#include "camera_config.h"
#include "util.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace picamera {

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

namespace {

// --- HTTP response helpers ---

std::string httpResponse(int status, const std::string &contentType,
                         const std::string &body) {
    const char *reason = (status == 200) ? "OK"
                         : (status == 404) ? "Not Found"
                         : (status == 405) ? "Method Not Allowed"
                         : "Internal Server Error";
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    return oss.str();
}

// --- JSON helpers (simple string formatting, no JSON library) ---

std::string jsonStr(std::string_view key, std::string_view val) {
    std::ostringstream oss;
    oss << '"' << key << "\":\"" << jsonEscape(val) << '"';
    return oss.str();
}

template <typename T>
std::string jsonNum(std::string_view key, T val) {
    std::ostringstream oss;
    oss << '"' << key << "\":" << val;
    return oss.str();
}

std::string jsonBool(std::string_view key, bool val) {
    std::ostringstream oss;
    oss << '"' << key << "\":" << (val ? "true" : "false");
    return oss.str();
}

// --- Enum to string (for JSON serialization) ---

const char *driveModeStr(DriveMode d) {
    switch (d) {
        case DriveMode::Single:     return "single";
        case DriveMode::SelfTimer:  return "selftimer";
        case DriveMode::Bracket:    return "bracket";
        case DriveMode::Timelapse:  return "timelapse";
        case DriveMode::Continuous: return "continuous";
        case DriveMode::Bulb:       return "bulb";
    }
    return "unknown";
}

const char *formatStr(OutputFormat f) {
    switch (f) {
        case OutputFormat::JPEG:     return "jpeg";
        case OutputFormat::PNG:      return "png";
        case OutputFormat::DNG:      return "dng";
        case OutputFormat::RAW_NV12: return "raw";
        case OutputFormat::PPM:      return "ppm";
        case OutputFormat::RawJpeg:  return "rawjpeg";
        case OutputFormat::DngJpeg:  return "dngjpeg";
    }
    return "unknown";
}

const char *meteringStr(MeteringMode m) {
    switch (m) {
        case MeteringMode::Matrix:  return "matrix";
        case MeteringMode::Centre:  return "centre";
        case MeteringMode::Spot:    return "spot";
    }
    return "unknown";
}

const char *imageSizeStr(ImageSize s) {
    switch (s) {
        case ImageSize::Large:  return "large";
        case ImageSize::Medium: return "medium";
        case ImageSize::Small:  return "small";
    }
    return "unknown";
}

const char *aspectStr(AspectRatio a) {
    switch (a) {
        case AspectRatio::Native:  return "native";
        case AspectRatio::Ratio43: return "4:3";
        case AspectRatio::Ratio169: return "16:9";
        case AspectRatio::Ratio11: return "1:1";
    }
    return "unknown";
}

const char *exposureModeStr(ExposureMode m) {
    switch (m) {
        case ExposureMode::Program: return "program";
        case ExposureMode::Shutter: return "shutter";
        case ExposureMode::Manual:  return "manual";
        case ExposureMode::Auto:    return "auto";
    }
    return "unknown";
}

const char *pictureStyleStr(PictureStyle p) {
    switch (p) {
        case PictureStyle::Standard:  return "standard";
        case PictureStyle::Vivid:     return "vivid";
        case PictureStyle::Neutral:   return "neutral";
        case PictureStyle::Monochrome:return "monochrome";
        case PictureStyle::Portrait:  return "portrait";
        case PictureStyle::Landscape: return "landscape";
    }
    return "unknown";
}

const char *bracketTypeStr(BracketType b) {
    switch (b) {
        case BracketType::AE: return "ae";
        case BracketType::WB: return "wb";
        case BracketType::ISO: return "iso";
    }
    return "unknown";
}

// --- JSON value extraction (simple parser for flat key-value objects) ---

// Find the value for a key in a JSON object string. Handles string and
// numeric/bool values. Returns nullopt if the key is not found.
std::optional<std::string> jsonFindValue(std::string_view json,
                                         std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = json.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == ':'))
        ++pos;
    if (pos >= json.size()) return std::nullopt;
    if (json[pos] == '"') {
        ++pos;
        std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
                if (json[pos] == 'n') val += '\n';
                else if (json[pos] == 'r') val += '\r';
                else if (json[pos] == 't') val += '\t';
                else val += json[pos];
            } else {
                val += json[pos];
            }
            ++pos;
        }
        return val;
    }
    // Numeric/bool value — read until comma or closing brace
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != '\n' && json[pos] != '\r') {
        val += json[pos];
        ++pos;
    }
    while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
        val.pop_back();
    return val;
}

template <typename T>
std::optional<T> jsonNumValue(std::string_view s) {
    if (s.empty()) return std::nullopt;
    try {
        if constexpr (std::is_integral_v<T>) {
            if constexpr (std::is_signed_v<T>) {
                return static_cast<T>(std::stoll(std::string(s)));
            } else {
                return static_cast<T>(std::stoull(std::string(s)));
            }
        } else {
            return static_cast<T>(std::stof(std::string(s)));
        }
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> jsonBoolValue(std::string_view s) {
    if (s == "true") return true;
    if (s == "false") return false;
    return std::nullopt;
}

// --- Enum from string (for JSON deserialization) ---

std::optional<DriveMode> parseDriveModeJson(std::string_view s) {
    if (s == "single") return DriveMode::Single;
    if (s == "selftimer") return DriveMode::SelfTimer;
    if (s == "bracket") return DriveMode::Bracket;
    if (s == "timelapse") return DriveMode::Timelapse;
    if (s == "continuous") return DriveMode::Continuous;
    if (s == "bulb") return DriveMode::Bulb;
    return std::nullopt;
}

std::optional<ExposureMode> parseExposureModeJson(std::string_view s) {
    if (s == "program") return ExposureMode::Program;
    if (s == "shutter") return ExposureMode::Shutter;
    if (s == "manual") return ExposureMode::Manual;
    if (s == "auto") return ExposureMode::Auto;
    return std::nullopt;
}

std::optional<PictureStyle> parsePictureStyleJson(std::string_view s) {
    if (s == "standard") return PictureStyle::Standard;
    if (s == "vivid") return PictureStyle::Vivid;
    if (s == "neutral") return PictureStyle::Neutral;
    if (s == "monochrome") return PictureStyle::Monochrome;
    if (s == "portrait") return PictureStyle::Portrait;
    if (s == "landscape") return PictureStyle::Landscape;
    return std::nullopt;
}

std::optional<BracketType> parseBracketTypeJson(std::string_view s) {
    if (s == "ae") return BracketType::AE;
    if (s == "wb") return BracketType::WB;
    if (s == "iso") return BracketType::ISO;
    return std::nullopt;
}

std::optional<ImageSize> parseImageSizeJson(std::string_view s) {
    if (s == "large") return ImageSize::Large;
    if (s == "medium") return ImageSize::Medium;
    if (s == "small") return ImageSize::Small;
    return std::nullopt;
}

std::optional<AspectRatio> parseAspectJson(std::string_view s) {
    if (s == "native") return AspectRatio::Native;
    if (s == "4:3") return AspectRatio::Ratio43;
    if (s == "16:9") return AspectRatio::Ratio169;
    if (s == "1:1") return AspectRatio::Ratio11;
    return std::nullopt;
}

} // namespace

// --- Pure-logic helper implementations ---

bool parseHttpRequest(const std::string &raw, HttpRequest &out) {
    auto lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) lineEnd = raw.find('\n');
    if (lineEnd == std::string::npos) return false;

    std::string_view line(raw.data(), lineEnd);
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) return false;
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return false;

    out.method = std::string(line.substr(0, sp1));
    out.path = std::string(line.substr(sp1 + 1, sp2 - sp1 - 1));

    // POST body follows the blank line (\r\n\r\n or \n\n)
    if (out.method == "POST") {
        std::string_view headers(raw.data(), raw.size());
        auto hdrEnd = headers.find("\r\n\r\n");
        size_t bodyStart = (hdrEnd != std::string_view::npos) ? hdrEnd + 4 : 0;
        if (bodyStart == 0) {
            hdrEnd = headers.find("\n\n");
            bodyStart = (hdrEnd != std::string_view::npos) ? hdrEnd + 2 : 0;
        }
        if (bodyStart > 0 && bodyStart < raw.size()) {
            out.body = raw.substr(bodyStart);
        }
    }

    return !out.method.empty() && !out.path.empty();
}

std::string settingsToJson(const CameraSettings &s) {
    std::ostringstream oss;
    oss << '{';
    oss << jsonStr("driveMode", driveModeStr(s.driveMode)) << ',';
    oss << jsonStr("exposureMode", exposureModeStr(s.exposureMode)) << ',';
    oss << jsonBool("aeEnable", s.aeEnable) << ',';
    oss << jsonNum("shutterUs", s.shutterUs) << ',';
    oss << jsonNum("analogueGain", s.analogueGain) << ',';
    oss << jsonNum("digitalGain", s.digitalGain) << ',';
    oss << jsonNum("exposureValue", s.exposureValue) << ',';
    oss << jsonNum("isoMin", s.isoMin) << ',';
    oss << jsonNum("isoMax", s.isoMax) << ',';
    oss << jsonStr("meteringMode", meteringStr(s.meteringMode)) << ',';
    oss << jsonBool("antiFlicker", s.antiFlicker) << ',';
    oss << jsonNum("flickerHz", s.flickerHz) << ',';
    oss << jsonNum("timerDuration", s.timerDuration) << ',';
    oss << jsonStr("bracketType", bracketTypeStr(s.bracketType)) << ',';
    oss << jsonNum("timelapseInterval", s.timelapseInterval) << ',';
    oss << jsonNum("timelapseCount", s.timelapseCount) << ',';
    oss << jsonStr("captureFormat", formatStr(s.captureFormat)) << ',';
    oss << jsonNum("jpegQuality", s.jpegQuality) << ',';
    oss << jsonNum("pngLevel", s.pngLevel) << ',';
    oss << jsonStr("imageSize", imageSizeStr(s.imageSize)) << ',';
    oss << jsonStr("aspectRatio", aspectStr(s.aspectRatio)) << ',';
    oss << jsonBool("awbEnable", s.awbEnable) << ',';
    oss << jsonStr("awbMode", s.awbMode) << ',';
    oss << jsonNum("wbKelvin", s.wbKelvin) << ',';
    oss << jsonNum("wbRedGain", s.wbRedGain) << ',';
    oss << jsonNum("wbBlueGain", s.wbBlueGain) << ',';
    oss << jsonStr("pictureStyle", pictureStyleStr(s.pictureStyle)) << ',';
    oss << jsonNum("brightness", s.brightness) << ',';
    oss << jsonNum("contrast", s.contrast) << ',';
    oss << jsonNum("saturation", s.saturation) << ',';
    oss << jsonNum("sharpness", s.sharpness) << ',';
    oss << jsonNum("displayBrightness", s.displayBrightness) << ',';
    oss << jsonNum("focusMagnify", s.focusMagnify) << ',';
    oss << jsonBool("enableBattery", s.enableBattery) << ',';
    oss << jsonNum("powerSaveTimeout", s.powerSaveTimeout);
    oss << '}';
    return oss.str();
}

std::string statusToJson(int batteryPercent, const CameraSettings &s,
                         uint32_t captureCount) {
    std::ostringstream oss;
    oss << '{';
    oss << jsonNum("batteryPercent", batteryPercent) << ',';
    oss << jsonNum("captureCount", captureCount) << ',';
    int iso = (s.analogueGain > 0)
        ? static_cast<int>(s.analogueGain * 100.0f) : 0;
    oss << jsonNum("iso", iso) << ',';
    oss << jsonNum("shutterUs", s.shutterUs) << ',';
    oss << jsonNum("ev", s.exposureValue) << ',';
    oss << jsonStr("wb", s.awbMode) << ',';
    oss << jsonStr("driveMode", driveModeStr(s.driveMode)) << ',';
    oss << jsonStr("format", formatStr(s.captureFormat));
    oss << '}';
    return oss.str();
}

std::string fileListingHtml(const std::vector<std::string> &files) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html><html><head><title>picamera captures</title>"
        << "<style>body{font-family:monospace;margin:1em}"
        << "a{text-decoration:none;color:#006}"
        << "h1{font-size:1.2em}</style></head><body>"
        << "<h1>picamera captures</h1><ul>";
    for (const auto &f : files) {
        std::string base = f;
        auto slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        oss << "<li><a href=\"/file/" << base << "\">" << base
            << "</a></li>";
    }
    if (files.empty()) {
        oss << "<li>No captures yet</li>";
    }
    oss << "</ul></body></html>";
    return oss.str();
}

void applySettingsJson(const std::string &json, CameraSettings &s) {
    if (auto v = jsonFindValue(json, "driveMode"))
        if (auto d = parseDriveModeJson(*v)) s.driveMode = *d;
    if (auto v = jsonFindValue(json, "exposureMode"))
        if (auto e = parseExposureModeJson(*v)) s.exposureMode = *e;
    if (auto v = jsonBoolValue(jsonFindValue(json, "aeEnable").value_or("")))
        s.aeEnable = *v;
    if (auto v = jsonNumValue<uint64_t>(jsonFindValue(json, "shutterUs").value_or("")))
        s.shutterUs = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "analogueGain").value_or("")))
        s.analogueGain = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "exposureValue").value_or("")))
        s.exposureValue = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "isoMin").value_or("")))
        s.isoMin = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "isoMax").value_or("")))
        s.isoMax = *v;
    if (auto v = jsonBoolValue(jsonFindValue(json, "antiFlicker").value_or("")))
        s.antiFlicker = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "flickerHz").value_or("")))
        s.flickerHz = *v;
    if (auto v = jsonNumValue<uint32_t>(jsonFindValue(json, "timerDuration").value_or("")))
        s.timerDuration = *v;
    if (auto v = jsonFindValue(json, "bracketType"))
        if (auto b = parseBracketTypeJson(*v)) s.bracketType = *b;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "timelapseInterval").value_or("")))
        s.timelapseInterval = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "timelapseCount").value_or("")))
        s.timelapseCount = *v;
    if (auto v = jsonFindValue(json, "captureFormat"))
        if (auto f = parseOutputFormat(*v)) s.captureFormat = *f;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "jpegQuality").value_or("")))
        s.jpegQuality = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "pngLevel").value_or("")))
        s.pngLevel = *v;
    if (auto v = jsonFindValue(json, "imageSize"))
        if (auto is = parseImageSizeJson(*v)) s.imageSize = *is;
    if (auto v = jsonFindValue(json, "aspectRatio"))
        if (auto a = parseAspectJson(*v)) s.aspectRatio = *a;
    if (auto v = jsonBoolValue(jsonFindValue(json, "awbEnable").value_or("")))
        s.awbEnable = *v;
    if (auto v = jsonFindValue(json, "awbMode")) s.awbMode = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "wbKelvin").value_or("")))
        s.wbKelvin = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "wbRedGain").value_or("")))
        s.wbRedGain = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "wbBlueGain").value_or("")))
        s.wbBlueGain = *v;
    if (auto v = jsonFindValue(json, "pictureStyle"))
        if (auto p = parsePictureStyleJson(*v)) s.pictureStyle = *p;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "brightness").value_or("")))
        s.brightness = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "contrast").value_or("")))
        s.contrast = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "saturation").value_or("")))
        s.saturation = *v;
    if (auto v = jsonNumValue<float>(jsonFindValue(json, "sharpness").value_or("")))
        s.sharpness = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "displayBrightness").value_or("")))
        s.displayBrightness = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "focusMagnify").value_or("")))
        s.focusMagnify = *v;
    if (auto v = jsonBoolValue(jsonFindValue(json, "enableBattery").value_or("")))
        s.enableBattery = *v;
    if (auto v = jsonNumValue<int>(jsonFindValue(json, "powerSaveTimeout").value_or("")))
        s.powerSaveTimeout = *v;
}

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += s[i];
            }
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string extractFileName(const std::string &path) {
    constexpr std::string_view prefix = "/file/";
    if (path.size() <= prefix.size() ||
        path.compare(0, prefix.size(), prefix) != 0)
        return {};
    std::string raw = path.substr(prefix.size());
    auto q = raw.find('?');
    if (q != std::string::npos) raw = raw.substr(0, q);
    return urlDecode(raw);
}

// --- WifiServer implementation ---

WifiServer::WifiServer() = default;

WifiServer::~WifiServer() { stop(); }

bool WifiServer::start(int port, const std::string &captureDir,
                       CameraSettings &settings, std::mutex &settingsMtx,
                       std::atomic<bool> &captureRequest,
                       const std::atomic<int> &batteryPercent,
                       const std::atomic<uint32_t> &captureCount) {
    if (running_.load(std::memory_order_acquire)) return false;

    port_ = port;
    captureDir_ = captureDir;
    settings_ = &settings;
    settingsMtx_ = &settingsMtx;
    captureRequest_ = &captureRequest;
    batteryPercent_ = &batteryPercent;
    captureCount_ = &captureCount;

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "WifiServer: socket() failed: " << errnoString(errno) << "\n";
        return false;
    }

    // Allow rapid restart (avoid "Address already in use" after reboot)
    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listenFd_, reinterpret_cast<struct sockaddr *>(&addr),
               sizeof(addr)) < 0) {
        std::cerr << "WifiServer: bind() failed on port " << port_
                  << ": " << errnoString(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    if (::listen(listenFd_, 4) < 0) {
        std::cerr << "WifiServer: listen() failed: " << errnoString(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    stopFlag_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { serverLoop(); });

    std::cout << "WifiServer: listening on port " << port_
              << ", serving " << captureDir_ << "\n";
    return true;
}

void WifiServer::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stopFlag_.store(true, std::memory_order_release);
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void WifiServer::serverLoop() {
    while (!stopFlag_.load(std::memory_order_acquire)) {
        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = ::accept(listenFd_,
                                reinterpret_cast<struct sockaddr *>(&clientAddr),
                                &addrLen);
        if (clientFd < 0) {
            if (stopFlag_.load(std::memory_order_acquire)) break;
            // Transient error — brief backoff to avoid busy-looping
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        handleConnection(clientFd);
        ::close(clientFd);
    }
}

void WifiServer::handleConnection(int clientFd) {
    // Best-effort read — HTTP headers fit in one recv usually
    char buf[8192];
    std::string raw;
    raw.reserve(4096);

    // Timeout so a slow/stuck client doesn't block the server forever
    struct timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (raw.size() < sizeof(buf)) {
        ssize_t n = ::recv(clientFd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") != std::string::npos ||
            raw.find("\n\n") != std::string::npos) {
            // One more read for POST body data
            if (raw.find("POST") == 0) {
                n = ::recv(clientFd, buf, sizeof(buf), 0);
                if (n > 0) raw.append(buf, static_cast<size_t>(n));
            }
            break;
        }
    }

    if (raw.empty()) return;

    HttpRequest req;
    if (!parseHttpRequest(raw, req)) {
        std::string resp = httpResponse(400, "text/plain", "Bad Request");
        ::send(clientFd, resp.data(), resp.size(), 0);
        return;
    }

    std::string response;

    if (req.method == "GET" && req.path == "/") {
        std::vector<std::string> files;
        try {
            namespace fs = std::filesystem;
            if (fs::exists(captureDir_)) {
                for (const auto &entry : fs::directory_iterator(captureDir_)) {
                    if (entry.is_regular_file())
                        files.push_back(entry.path().string());
                }
                std::sort(files.begin(), files.end());
            }
        } catch (const std::exception &e) {
            std::cerr << "WifiServer: directory listing failed: " << e.what() << "\n";
        }
        std::string html = fileListingHtml(files);
        response = httpResponse(200, "text/html", html);
    } else if (req.method == "GET" && req.path == "/status") {
        CameraSettings snap;
        {
            std::lock_guard<std::mutex> lk(*settingsMtx_);
            snap = *settings_;
        }
        int bat = batteryPercent_ ? batteryPercent_->load(std::memory_order_acquire) : 0;
        uint32_t cap = captureCount_ ? captureCount_->load(std::memory_order_acquire) : 0;
        std::string json = statusToJson(bat, snap, cap);
        response = httpResponse(200, "application/json", json);
    } else if (req.method == "GET" && req.path == "/settings") {
        CameraSettings snap;
        {
            std::lock_guard<std::mutex> lk(*settingsMtx_);
            snap = *settings_;
        }
        std::string json = settingsToJson(snap);
        response = httpResponse(200, "application/json", json);
    } else if (req.method == "POST" && req.path == "/settings") {
        {
            std::lock_guard<std::mutex> lk(*settingsMtx_);
            applySettingsJson(req.body, *settings_);
        }
        response = httpResponse(200, "application/json", "{\"ok\":true}");
    } else if (req.method == "POST" && req.path == "/capture") {
        if (captureRequest_)
            captureRequest_->store(true, std::memory_order_release);
        response = httpResponse(200, "application/json", "{\"ok\":true}");
    } else if (req.method == "GET") {
        std::string filename = extractFileName(req.path);
        // Path-traversal guard — only serve files directly in captureDir
        if (filename.empty() || !isSafeFileName(filename)) {
            response = httpResponse(404, "text/plain", "Not Found");
        } else {
            std::string filePath = captureDir_ + "/" + filename;
            std::ifstream in(filePath, std::ios::binary);
            if (!in.good()) {
                response = httpResponse(404, "text/plain", "Not Found");
            } else {
                std::string body((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
                response = httpResponse(200, "application/octet-stream", body);
            }
        }
    } else {
        response = httpResponse(405, "text/plain", "Method Not Allowed");
    }

    ::send(clientFd, response.data(), response.size(), 0);
}

} // namespace picamera
