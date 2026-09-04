#include "cli.h"
#include "safe_path.h"
#include "timelapse.h"

#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace picamera {

namespace {
// Parse argv[i+1] into T; on error prints and returns false, advancing i.
template <typename T, typename Conv>
bool parseIntArg(int argc, char **argv, int &i,
                 const char *flag, Conv conv, T &out) {
    if (i + 1 >= argc) {
        std::cerr << flag << " requires a value\n";
        return false;
    }
    try {
        out = conv(argv[++i]);
    } catch (const std::exception &e) {
        std::cerr << flag << " invalid value '" << argv[i] << "': " << e.what() << "\n";
        return false;
    }
    return true;
}

// Parse argv[i+1] into a std::string; on missing value prints and returns false.
bool parseStrArg(int argc, char **argv, int &i,
                 const char *flag, std::string &out) {
    if (i + 1 >= argc) {
        std::cerr << flag << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

// Parse a string into uint32_t with range checking. std::stoul returns
// unsigned long (64-bit on x86-64), so a naive static_cast<uint32_t>
// silently wraps values >= 2^32 and bypasses downstream max-value checks.
// This helper parses into uint64_t and rejects anything > UINT32_MAX.
// Also verifies the entire token was consumed (no trailing garbage).
uint32_t parseUint32(const char *s) {
    if (!s || !s[0])
        throw std::invalid_argument("empty value");
    if (std::isspace(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument(std::string("leading whitespace not allowed: '") + s + "'");
    if (s[0] == '-' || s[0] == '+')
        throw std::invalid_argument(std::string("sign not allowed: '") + s + "'");
    std::size_t pos = 0;
    uint64_t v = std::stoull(s, &pos);
    if (s[pos] != '\0') {
        throw std::invalid_argument(std::string("trailing characters after value: '") + s + "'");
    }
    if (v > std::numeric_limits<uint32_t>::max()) {
        throw std::out_of_range("value exceeds uint32_t range");
    }
    return static_cast<uint32_t>(v);
}

// Parse a hex string into uint8_t with range checking.
// Also verifies the entire token was consumed. Accepts an optional
// "0x"/"0X" prefix (commonly typed by users for hex values).
uint8_t parseUint8Hex(const char *s) {
    if (!s || !s[0])
        throw std::invalid_argument("empty value");
    if (std::isspace(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument(std::string("leading whitespace not allowed: '") + s + "'");
    if (s[0] == '-' || s[0] == '+')
        throw std::invalid_argument(std::string("sign not allowed: '") + s + "'");
    // Skip optional 0x/0X prefix — std::stoull with base 16 does not
    // accept it, but users commonly type --battery-addr 0x48.
    const char *hex = s;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) hex = s + 2;
    if (*hex == '\0')
        throw std::invalid_argument(std::string("no digits after prefix: '") + s + "'");
    std::size_t pos = 0;
    uint64_t v = std::stoull(hex, &pos, 16);
    if (hex[pos] != '\0') {
        throw std::invalid_argument(std::string("trailing characters after value: '") + s + "'");
    }
    if (v > std::numeric_limits<uint8_t>::max()) {
        throw std::out_of_range("value exceeds uint8_t range");
    }
    return static_cast<uint8_t>(v);
}

// Parse a string into uint64_t with range checking (shutter/exposure).
// Also verifies the entire token was consumed.
uint64_t parseUint64(const char *s) {
    if (!s || !s[0])
        throw std::invalid_argument("empty value");
    if (std::isspace(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument(std::string("leading whitespace not allowed: '") + s + "'");
    if (s[0] == '-' || s[0] == '+')
        throw std::invalid_argument(std::string("sign not allowed: '") + s + "'");
    std::size_t pos = 0;
    uint64_t v = std::stoull(s, &pos);
    if (s[pos] != '\0') {
        throw std::invalid_argument(std::string("trailing characters after value: '") + s + "'");
    }
    return v;
}

// Parse a string into int with full-token validation.
int parseInt32(const char *s) {
    if (!s || !s[0])
        throw std::invalid_argument("empty value");
    if (std::isspace(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument(std::string("leading whitespace not allowed: '") + s + "'");
    std::size_t pos = 0;
    int v = std::stoi(s, &pos);
    if (s[pos] != '\0') {
        throw std::invalid_argument(std::string("trailing characters after value: '") + s + "'");
    }
    return v;
}

// Parse a string into float with full-token validation.
// Rejects inf/nan — libcamera controls expect finite values.
float parseFloat(const char *s) {
    if (!s || !s[0])
        throw std::invalid_argument("empty value");
    if (std::isspace(static_cast<unsigned char>(s[0])))
        throw std::invalid_argument(std::string("leading whitespace not allowed: '") + s + "'");
    std::size_t pos = 0;
    float v = std::stof(s, &pos);
    if (s[pos] != '\0') {
        throw std::invalid_argument(std::string("trailing characters after value: '") + s + "'");
    }
    if (!std::isfinite(v)) {
        throw std::invalid_argument(std::string("value must be finite, got: ") + s);
    }
    return v;
}
} // namespace

void printUsage(const char *prog) {
    std::cerr << "Pi Zero 2 WH + HQ Camera — libcamera C++ capture\n\n"
              << "Usage:\n"
              << "  " << prog << " --capture [file]              Capture a still (default: capture.<ext>)\n"
              << "  " << prog << " --list-controls               List camera controls\n"
              << "  " << prog << " --timelapse <sec> [options]   Timelapse mode\n"
              << "  " << prog << " --preview [options]           Live preview to SPI LCD (Waveshare 1.44\" HAT)\n\n"
              << "Options:\n"
              << "  --format <type>         Output format for --capture/--timelapse: ppm, raw, png, jpeg, dng\n"
              << "                          (default: ppm). For --preview stills, use --capture-format.\n"
              << "                          jpeg = ISP hardware-encoded (Pi only), ~10x faster\n"
              << "                          dng  = raw Bayer DNG (10-bit, for raw development)\n"
              << "  --png-level <0-9>       PNG compression level (0=none, 1=fast, 6=default, 9=best)\n"
              << "  --jpeg-quality <1-100>  JPEG quality for software encode (default: 90)\n"
              << "  --bracket <n,ev...>     HDR bracketing: capture N frames at EV offsets\n"
              << "                          e.g. --bracket 3,-2,0,+2 (3 frames: -2EV, 0EV, +2EV)\n"
              << "  --output <pattern>      Filename pattern: %04d (sequence) or strftime (e.g. %F_%H%M%S)\n"
              << "                          Default: capture_%04d.ppm\n"
              << "  --count <n>             Number of shots (0 = infinite)\n"
              << "  --width <px>            Image width (default: 4056)\n"
              << "  --height <px>           Image height (default: 3040)\n"
              << "  --iso <gain>            Analogue gain (e.g. 1.0, 2.0, 4.0)\n"
              << "  --digital-gain <gain>   Digital gain (e.g. 1.0, 2.0)\n"
              << "  --shutter <us>          Exposure time in microseconds\n"
              << "  --awb <mode>            White balance: auto, daylight, cloudy,\n"
              << "                          incandescent, tungsten, fluorescent, indoor\n"
              << "  --ae-disable            Disable auto exposure\n"
              << "  --awb-disable           Disable auto white balance\n"
              << "  --warmup <n>            Frames to let AE/AWB converge (default: 8)\n"
              << "  --preview-w <px>        Preview stream width (default: 320)\n"
              << "  --preview-h <px>        Preview stream height (default: 240)\n"
              << "  --preview-fps <n>       Preview max frame rate (default: 20)\n"
              << "  --capture-w <px>        Still capture width (default: 4056)\n"
              << "  --capture-h <px>        Still capture height (default: 3040)\n"
              << "  --capture-format <type> Still capture format: jpeg, png, ppm, dng (default: jpeg)\n"
              << "  --capture-dir <path>    Directory for captured images (default: .)\n"
              << "  --capture-prefix <str>  Filename prefix for captures (default: capture)\n"
              << "  --spi-device <path>     SPI device for display (default: /dev/spidev0.0)\n"
              << "  --display-rotate <deg>  Display rotation: 0, 90, 180, 270 (default: 90)\n"
              << "  --battery               Show battery level overlay on preview (ADS1115 ADC)\n"
              << "  --battery-i2c <path>    I2C device for ADS1115 (default: /dev/i2c-1)\n"
              << "  --battery-addr <hex>    ADS1115 I2C address (default: 0x48)\n\n"
              << "Examples:\n"
              << "  " << prog << " --capture photo.png --format png\n"
              << "  " << prog << " --capture photo.jpg --format jpeg       (ISP hardware encode, ~10x faster)\n"
              << "  " << prog << " --capture photo.raw --format raw\n"
              << "  " << prog << " --capture photo.ppm --iso 2.0 --shutter 30000\n"
              << "  " << prog << " --timelapse 60 --count 10 --output timelapse_%04d.png --format png\n"
              << "  " << prog << " --preview                              (live preview, joystick=capture)\n"
              << "  " << prog << " --preview --display-rotate 90          (rotated display)\n"
              << "  " << prog << " --preview --capture-format png         (capture PNG on button press)\n"
              << "  " << prog << " --preview --battery                   (show battery % on LCD)\n";
}

bool parseArgs(int argc, char **argv, CliOptions &opts, CameraConfig &cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        }
        if (arg == "--capture") {
            opts.mode = "capture";
            if (i + 1 < argc) opts.captureFile = argv[++i];
        } else if (arg == "--list-controls") {
            opts.mode = "list-controls";
        } else if (arg == "--preview") {
            opts.mode = "preview";
        } else if (arg == "--timelapse") {
            opts.mode = "timelapse";
            if (!parseIntArg(argc, argv, i, "--timelapse",
                             [](const char *s) { return parseInt32(s); },
                             opts.timelapseInterval)) return false;
        } else if (arg == "--output") {
            if (!parseStrArg(argc, argv, i, "--output", opts.outputPattern)) return false;
        } else if (arg == "--count") {
            if (!parseIntArg(argc, argv, i, "--count",
                             [](const char *s) { return parseInt32(s); },
                             opts.timelapseCount)) return false;
        } else if (arg == "--width") {
            if (!parseIntArg(argc, argv, i, "--width", parseUint32,
                             cfg.width)) return false;
        } else if (arg == "--height") {
            if (!parseIntArg(argc, argv, i, "--height", parseUint32,
                             cfg.height)) return false;
        } else if (arg == "--iso") {
            if (!parseIntArg(argc, argv, i, "--iso",
                             [](const char *s) { return parseFloat(s); },
                             cfg.analogueGain)) return false;
            if (cfg.analogueGain < 0.0f) {
                std::cerr << "--iso: must be non-negative\n";
                return false;
            }
        } else if (arg == "--digital-gain") {
            if (!parseIntArg(argc, argv, i, "--digital-gain",
                             [](const char *s) { return parseFloat(s); },
                             cfg.digitalGain)) return false;
            if (cfg.digitalGain < 0.0f) {
                std::cerr << "--digital-gain: must be non-negative\n";
                return false;
            }
        } else if (arg == "--shutter") {
            if (!parseIntArg(argc, argv, i, "--shutter", parseUint64,
                             cfg.exposureTime)) return false;
            // libcamera's ExposureTime control is int32_t microseconds.
            // applyControls clamps to INT32_MAX, but the capture timeout
            // is derived from config_.exposureTime — reject values that
            // would produce an unbounded timeout on a pipeline stall.
            constexpr uint64_t kMaxShutterUs =
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
            if (cfg.exposureTime > kMaxShutterUs) {
                std::cerr << "--shutter must be <= " << kMaxShutterUs
                          << " us (INT32_MAX)\n";
                return false;
            }
        } else if (arg == "--awb") {
            std::string mode;
            if (!parseStrArg(argc, argv, i, "--awb", mode)) return false;
            // Normalize to lowercase — kAwbModes and kAwbTable use lowercase
            // keys, so case-insensitive input (e.g. "Daylight") would fail
            // validation without this. parseOutputFormat is already
            // case-insensitive; this makes --awb consistent with --format.
            for (auto &c : mode) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!isValidAwbMode(mode)) {
                std::cerr << "--awb: unknown mode '" << mode
                          << "' (options: auto, daylight, cloudy, incandescent, "
                          << "tungsten, fluorescent, indoor)\n";
                return false;
            }
            cfg.awbMode = mode;
        } else if (arg == "--ae-disable") {
            cfg.aeEnable = false;
        } else if (arg == "--format") {
            std::string fmt;
            if (!parseStrArg(argc, argv, i, "--format", fmt)) return false;
            auto parsed = parseOutputFormat(fmt);
            if (!parsed) {
                std::cerr << "Unknown format: " << fmt << " (options: ppm, raw, png, jpeg, dng)\n";
                return false;
            }
            cfg.format = *parsed;
        } else if (arg == "--png-level") {
            if (!parseIntArg(argc, argv, i, "--png-level",
                             [](const char *s) { return parseInt32(s); },
                             cfg.pngLevel))
                return false;
            if (cfg.pngLevel < 0 || cfg.pngLevel > 9) {
                std::cerr << "--png-level must be 0-9, got " << cfg.pngLevel << "\n";
                return false;
            }
        } else if (arg == "--jpeg-quality") {
            if (!parseIntArg(argc, argv, i, "--jpeg-quality",
                             [](const char *s) { return parseInt32(s); },
                             cfg.jpegQuality))
                return false;
            if (cfg.jpegQuality < 1 || cfg.jpegQuality > 100) {
                std::cerr << "--jpeg-quality must be 1-100, got " << cfg.jpegQuality << "\n";
                return false;
            }
        } else if (arg == "--awb-disable") {
            cfg.awbEnable = false;
        } else if (arg == "--bracket") {
            // Format: --bracket count,ev1,ev2,...
            // e.g. --bracket 3,-2,0,+2  (3 frames at -2EV, 0EV, +2EV)
            if (i + 1 >= argc) {
                std::cerr << "--bracket requires an argument\n";
                return false;
            }
            std::string spec = argv[++i];
            // Parse comma-separated values: first is count, rest are EV offsets.
            std::vector<float> evs;
            size_t pos = 0;
            int count = 0;
            try {
                size_t comma = spec.find(',');
                if (comma == std::string::npos) {
                    std::cerr << "--bracket format: count,ev1,ev2,... (e.g. 3,-2,0,+2)\n";
                    return false;
                }
                count = parseInt32(spec.substr(0, comma).c_str());
                pos = comma + 1;
                while (pos <= spec.size()) {
                    size_t next = spec.find(',', pos);
                    std::string token = (next == std::string::npos)
                                        ? spec.substr(pos)
                                        : spec.substr(pos, next - pos);
                    if (!token.empty())
                        evs.push_back(parseFloat(token.c_str()));
                    if (next == std::string::npos) break;
                    pos = next + 1;
                }
            } catch (const std::exception &e) {
                std::cerr << "--bracket parse error: " << e.what() << "\n";
                return false;
            }
            if (count <= 0 || count > 9) {
                std::cerr << "--bracket count must be 1-9, got " << count << "\n";
                return false;
            }
            if (static_cast<int>(evs.size()) != count) {
                std::cerr << "--bracket: expected " << count << " EV values, got "
                          << evs.size() << "\n";
                return false;
            }
            cfg.bracketEv = evs;
        } else if (arg == "--spi-device") {
            if (!parseStrArg(argc, argv, i, "--spi-device", opts.spiDevice)) return false;
        } else if (arg == "--display-rotate") {
            if (!parseIntArg(argc, argv, i, "--display-rotate",
                             [](const char *s) { return parseInt32(s); },
                             opts.displayRotation)) return false;
        } else if (arg == "--preview-w") {
            if (!parseIntArg(argc, argv, i, "--preview-w", parseUint32,
                             opts.previewWidth)) return false;
        } else if (arg == "--preview-h") {
            if (!parseIntArg(argc, argv, i, "--preview-h", parseUint32,
                             opts.previewHeight)) return false;
        } else if (arg == "--preview-fps") {
            if (!parseIntArg(argc, argv, i, "--preview-fps", parseUint32,
                             opts.previewFps)) return false;
        } else if (arg == "--capture-w") {
            if (!parseIntArg(argc, argv, i, "--capture-w", parseUint32,
                             opts.captureWidth)) return false;
        } else if (arg == "--capture-h") {
            if (!parseIntArg(argc, argv, i, "--capture-h", parseUint32,
                             opts.captureHeight)) return false;
        } else if (arg == "--capture-format") {
            std::string fmt;
            if (!parseStrArg(argc, argv, i, "--capture-format", fmt)) return false;
            if (!parseOutputFormat(fmt)) {
                std::cerr << "Unknown --capture-format: " << fmt
                          << " (options: jpeg, png, ppm, raw, dng)\n";
                return false;
            }
            opts.captureFormat = fmt;
        } else if (arg == "--capture-dir") {
            if (!parseStrArg(argc, argv, i, "--capture-dir", opts.captureDir)) return false;
        } else if (arg == "--capture-prefix") {
            if (!parseStrArg(argc, argv, i, "--capture-prefix", opts.capturePrefix)) return false;
        } else if (arg == "--warmup") {
            if (!parseIntArg(argc, argv, i, "--warmup", parseUint32,
                             cfg.warmupFrames)) return false;
            // Cap to a sane maximum to prevent infinite warmup loops.
            constexpr uint32_t kMaxWarmup = 1000;
            if (cfg.warmupFrames > kMaxWarmup) {
                std::cerr << "picamera: --warmup capped to " << kMaxWarmup << "\n";
                cfg.warmupFrames = kMaxWarmup;
            }
        } else if (arg == "--battery") {
            opts.enableBattery = true;
        } else if (arg == "--battery-i2c") {
            if (!parseStrArg(argc, argv, i, "--battery-i2c", opts.batteryI2cDevice)) return false;
        } else if (arg == "--battery-addr") {
            if (!parseIntArg(argc, argv, i, "--battery-addr", parseUint8Hex,
                             opts.batteryI2cAddress)) return false;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }
    }

    if (opts.mode.empty()) {
        printUsage(argv[0]);
        return false;
    }

    // Validate dimensions to prevent integer overflow in buffer calculations.
    if (cfg.width == 0 || cfg.width > kMaxSensorWidth) {
        std::cerr << "--width must be 1-" << kMaxSensorWidth << ", got " << cfg.width << "\n";
        return false;
    }
    if (cfg.height == 0 || cfg.height > kMaxSensorHeight) {
        std::cerr << "--height must be 1-" << kMaxSensorHeight << ", got " << cfg.height << "\n";
        return false;
    }
    if (opts.previewWidth == 0 || opts.previewWidth > kMaxSensorWidth) {
        std::cerr << "--preview-w must be 1-" << kMaxSensorWidth << ", got " << opts.previewWidth << "\n";
        return false;
    }
    if (opts.previewHeight == 0 || opts.previewHeight > kMaxSensorHeight) {
        std::cerr << "--preview-h must be 1-" << kMaxSensorHeight << ", got " << opts.previewHeight << "\n";
        return false;
    }
    if (opts.captureWidth == 0 || opts.captureWidth > kMaxSensorWidth) {
        std::cerr << "--capture-w must be 1-" << kMaxSensorWidth << ", got " << opts.captureWidth << "\n";
        return false;
    }
    if (opts.captureHeight == 0 || opts.captureHeight > kMaxSensorHeight) {
        std::cerr << "--capture-h must be 1-" << kMaxSensorHeight << ", got " << opts.captureHeight << "\n";
        return false;
    }

    // Validate device paths
    if (!isSafeDevicePath(opts.spiDevice)) {
        std::cerr << "--spi-device must be a /dev/ path without '..'\n";
        return false;
    }
    if (!isSafeDevicePath(opts.batteryI2cDevice)) {
        std::cerr << "--battery-i2c must be a /dev/ path without '..'\n";
        return false;
    }

    // Validate I2C address (7-bit range, 0x03–0x77 per I2C spec)
    if (opts.batteryI2cAddress < 0x03 || opts.batteryI2cAddress > 0x77) {
        std::cerr << "--battery-addr must be 0x03-0x77, got 0x"
                  << std::hex << static_cast<int>(opts.batteryI2cAddress) << std::dec << "\n";
        return false;
    }

    // Validate preview FPS (must be > 0 to prevent division by zero in
    // frame-delay calculation; cap at a reasonable max to avoid busy-looping).
    if (opts.previewFps == 0 || opts.previewFps > 120) {
        std::cerr << "--preview-fps must be 1-120, got " << opts.previewFps << "\n";
        return false;
    }

    // Validate timelapse parameters
    if (opts.mode == "timelapse") {
        if (opts.timelapseInterval <= 0) {
            std::cerr << "--timelapse must be > 0, got " << opts.timelapseInterval << "\n";
            return false;
        }
        if (opts.timelapseCount < 0) {
            std::cerr << "--count must be >= 0, got " << opts.timelapseCount << "\n";
            return false;
        }
        // --bracket is only supported in --capture mode (captureBracket).
        // runTimelapse uses single-shot capture, so bracket settings would
        // be silently ignored — reject explicitly to avoid user confusion.
        if (!cfg.bracketEv.empty()) {
            std::cerr << "--bracket cannot be used with --timelapse\n";
            return false;
        }
    }

    // Validate display rotation
    if (opts.displayRotation != 0 && opts.displayRotation != 90 &&
        opts.displayRotation != 180 && opts.displayRotation != 270) {
        std::cerr << "--display-rotate must be 0, 90, 180, or 270, got "
                  << opts.displayRotation << "\n";
        return false;
    }

    // Validate capture prefix for path safety
    if (!isSafePathComponent(opts.capturePrefix)) {
        std::cerr << "--capture-prefix contains unsafe characters (.., /, control chars)\n";
        return false;
    }

    // Validate capture file path (may contain subdirectory separators but
    // must not escape via ".." or be absolute).
    if (!opts.captureFile.empty() && !isSafeFilePath(opts.captureFile)) {
        std::cerr << "--capture contains unsafe path components (.., absolute, control chars)\n";
        return false;
    }

    // --output is a timelapse-only flag. Reject it in other modes to
    // avoid silently ignoring a user-supplied pattern (e.g. in --capture
    // mode the filename comes from --capture, not --output).
    if (!opts.outputPattern.empty() && opts.mode != "timelapse") {
        std::cerr << "--output only applies to --timelapse mode\n";
        return false;
    }

    // Validate output pattern for path safety (timelapse filenames).
    if (!opts.outputPattern.empty() && !isSafeFilePath(opts.outputPattern)) {
        std::cerr << "--output contains unsafe path components (.., absolute, control chars)\n";
        return false;
    }

    // Derive default timelapse output pattern from --format so the
    // filename extension matches the actual file content (e.g. --format
    // jpeg -> capture_%04d.jpeg). If the user explicitly set --output,
    // their pattern is used as-is.
    if (opts.mode == "timelapse" && opts.outputPattern.empty()) {
        opts.outputPattern = std::string("capture_%04d.") +
                             std::string(extensionFor(cfg.format));
    }

    // Validate output pattern format at parse time so format-string errors
    // (stray %, unsupported specifiers) are reported immediately rather than
    // on the first timelapse capture.
    if (!opts.outputPattern.empty()) {
        try {
            (void)formatTimelapseName(opts.outputPattern, 0);
        } catch (const std::exception &e) {
            std::cerr << "--output pattern invalid: " << e.what() << "\n";
            return false;
        }
    }

    // Validate and canonicalize capture directory for path safety.
    // First reject ".." as a path component (defense in depth), then
    // canonicalize to resolve symlinks so that later isPathInside
    // checks can't be bypassed via symlinks to sensitive directories.
    if (!opts.captureDir.empty()) {
        // Check for ".." as a path component, not as a substring —
        // legitimate directory names like "a..b" should be allowed.
        bool hasDotDot = false;
        size_t pos = 0;
        while (pos < opts.captureDir.size()) {
            size_t slash = opts.captureDir.find('/', pos);
            std::string comp = (slash == std::string::npos)
                ? opts.captureDir.substr(pos)
                : opts.captureDir.substr(pos, slash - pos);
            if (comp == "..") { hasDotDot = true; break; }
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (hasDotDot) {
            std::cerr << "--capture-dir must not contain '..' as a path component\n";
            return false;
        }
        std::string canon = canonicalizeDir(opts.captureDir);
        if (canon.empty()) {
            std::cerr << "--capture-dir contains unsafe path components"
                      << " (control chars or unresolvable symlinks)\n";
            return false;
        }
        opts.captureDir = canon;
    }

    return true;
}

}
