#include "cli.h"
#include <iostream>
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
} // namespace

void printUsage(const char *prog) {
    std::cout << "Pi Zero 2 WH + HQ Camera — libcamera C++ capture\n\n"
              << "Usage:\n"
              << "  " << prog << " --capture <file>              Capture a still\n"
              << "  " << prog << " --list-controls               List camera controls\n"
              << "  " << prog << " --timelapse <sec> [options]   Timelapse mode\n"
              << "  " << prog << " --preview [options]           Live preview to SPI LCD (Waveshare 1.44\" HAT)\n\n"
              << "Options:\n"
              << "  --format <type>         Output format: ppm, raw, png, jpeg, dng (default: ppm)\n"
              << "                          jpeg = ISP hardware-encoded (Pi only), ~10x faster\n"
              << "                          dng  = raw Bayer DNG (10-bit, for raw development)\n"
              << "  --png-level <0-9>       PNG compression level (0=none, 1=fast, 6=default, 9=best)\n"
              << "  --bracket <n,ev...>     HDR bracketing: capture N frames at EV offsets\n"
              << "                          e.g. --bracket 3,-2,0,+2 (3 frames: -2EV, 0EV, +2EV)\n"
              << "  --output <pattern>      Output filename pattern (default: capture_%04d.ppm)\n"
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
              << "  --display-rotate <deg>  Display rotation: 0, 90, 180, 270 (default: 0)\n"
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
                             [](const char *s) { return std::stoi(s); },
                             opts.timelapseInterval)) return false;
        } else if (arg == "--output") {
            if (i + 1 < argc) opts.outputPattern = argv[++i];
        } else if (arg == "--count") {
            if (!parseIntArg(argc, argv, i, "--count",
                             [](const char *s) { return std::stoi(s); },
                             opts.timelapseCount)) return false;
        } else if (arg == "--width") {
            if (!parseIntArg(argc, argv, i, "--width",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             cfg.width)) return false;
        } else if (arg == "--height") {
            if (!parseIntArg(argc, argv, i, "--height",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             cfg.height)) return false;
        } else if (arg == "--iso") {
            if (!parseIntArg(argc, argv, i, "--iso",
                             [](const char *s) { return std::stof(s); },
                             cfg.analogueGain)) return false;
        } else if (arg == "--digital-gain") {
            if (!parseIntArg(argc, argv, i, "--digital-gain",
                             [](const char *s) { return std::stof(s); },
                             cfg.digitalGain)) return false;
        } else if (arg == "--shutter") {
            if (!parseIntArg(argc, argv, i, "--shutter",
                             [](const char *s) { return static_cast<uint64_t>(std::stoull(s)); },
                             cfg.exposureTime)) return false;
        } else if (arg == "--awb") {
            if (i + 1 < argc) cfg.awbMode = argv[++i];
        } else if (arg == "--ae-disable") {
            cfg.aeEnable = false;
        } else if (arg == "--format") {
            if (i + 1 < argc) {
                std::string fmt = argv[++i];
                auto parsed = parseOutputFormat(fmt);
                if (!parsed) {
                    std::cerr << "Unknown format: " << fmt << " (options: ppm, raw, png, jpeg, dng)\n";
                    return false;
                }
                cfg.format = *parsed;
            }
        } else if (arg == "--png-level") {
            if (!parseIntArg(argc, argv, i, "--png-level",
                             [](const char *s) { return std::stoi(s); },
                             cfg.pngLevel))
                return false;
            if (cfg.pngLevel < 0 || cfg.pngLevel > 9) {
                std::cerr << "--png-level must be 0-9, got " << cfg.pngLevel << "\n";
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
                count = std::stoi(spec.substr(0, comma));
                pos = comma + 1;
                while (pos <= spec.size()) {
                    size_t next = spec.find(',', pos);
                    std::string token = (next == std::string::npos)
                                        ? spec.substr(pos)
                                        : spec.substr(pos, next - pos);
                    if (!token.empty())
                        evs.push_back(std::stof(token));
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
            if (i + 1 < argc) opts.spiDevice = argv[++i];
        } else if (arg == "--display-rotate") {
            if (!parseIntArg(argc, argv, i, "--display-rotate",
                             [](const char *s) { return std::stoi(s); },
                             opts.displayRotation)) return false;
        } else if (arg == "--preview-w") {
            if (!parseIntArg(argc, argv, i, "--preview-w",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             opts.previewWidth)) return false;
        } else if (arg == "--preview-h") {
            if (!parseIntArg(argc, argv, i, "--preview-h",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             opts.previewHeight)) return false;
        } else if (arg == "--preview-fps") {
            if (!parseIntArg(argc, argv, i, "--preview-fps",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             opts.previewFps)) return false;
        } else if (arg == "--capture-w") {
            if (!parseIntArg(argc, argv, i, "--capture-w",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             opts.captureWidth)) return false;
        } else if (arg == "--capture-h") {
            if (!parseIntArg(argc, argv, i, "--capture-h",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             opts.captureHeight)) return false;
        } else if (arg == "--capture-format") {
            if (i + 1 < argc) opts.captureFormat = argv[++i];
        } else if (arg == "--capture-dir") {
            if (i + 1 < argc) opts.captureDir = argv[++i];
        } else if (arg == "--capture-prefix") {
            if (i + 1 < argc) opts.capturePrefix = argv[++i];
        } else if (arg == "--warmup") {
            if (!parseIntArg(argc, argv, i, "--warmup",
                             [](const char *s) { return static_cast<uint32_t>(std::stoul(s)); },
                             cfg.warmupFrames)) return false;
        } else if (arg == "--battery") {
            opts.enableBattery = true;
        } else if (arg == "--battery-i2c") {
            if (i + 1 < argc) opts.batteryI2cDevice = argv[++i];
        } else if (arg == "--battery-addr") {
            if (!parseIntArg(argc, argv, i, "--battery-addr",
                             [](const char *s) { return static_cast<uint8_t>(std::stoul(s, nullptr, 16)); },
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
    return true;
}

}
