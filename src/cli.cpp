#include "cli.h"
#include <iostream>

namespace picamera {

void printUsage(const char *prog) {
    std::cout << "Pi Zero 2 WH + HQ Camera — libcamera C++ capture\n\n"
              << "Usage:\n"
              << "  " << prog << " --capture <file>              Capture a still\n"
              << "  " << prog << " --list-controls               List camera controls\n"
              << "  " << prog << " --timelapse <sec> [options]   Timelapse mode\n\n"
              << "Options:\n"
              << "  --format <type>         Output format: ppm, raw, png (default: ppm)\n"
              << "  --output <pattern>      Output filename pattern (default: capture_%04d.ppm)\n"
              << "  --count <n>             Number of shots (0 = infinite)\n"
              << "  --width <px>            Image width (default: 4056)\n"
              << "  --height <px>           Image height (default: 3040)\n"
              << "  --iso <gain>            Analogue gain (e.g. 1.0, 2.0, 4.0)\n"
              << "  --shutter <us>          Exposure time in microseconds\n"
              << "  --awb <mode>            White balance: auto, daylight, cloudy,\n"
              << "                          incandescent, tungsten, fluorescent, indoor\n"
              << "  --ae-disable            Disable auto exposure\n"
              << "  --awb-disable           Disable auto white balance\n\n"
              << "Examples:\n"
              << "  " << prog << " --capture photo.png --format png\n"
              << "  " << prog << " --capture photo.raw --format raw\n"
              << "  " << prog << " --capture photo.ppm --iso 2.0 --shutter 30000\n"
              << "  " << prog << " --timelapse 60 --count 10 --output timelapse_%04d.png --format png\n";
}

bool parseArgs(int argc, char **argv, CliOptions &opts, CameraConfig &cfg) {
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--capture") {
            opts.mode = "capture";
            if (i + 1 < argc) opts.captureFile = argv[++i];
        } else if (arg == "--list-controls") {
            opts.mode = "list-controls";
        } else if (arg == "--timelapse") {
            opts.mode = "timelapse";
            if (i + 1 < argc) opts.timelapseInterval = std::stoi(argv[++i]);
        } else if (arg == "--output") {
            if (i + 1 < argc) opts.outputPattern = argv[++i];
        } else if (arg == "--count") {
            if (i + 1 < argc) opts.timelapseCount = std::stoi(argv[++i]);
        } else if (arg == "--width") {
            if (i + 1 < argc) cfg.width = std::stoul(argv[++i]);
        } else if (arg == "--height") {
            if (i + 1 < argc) cfg.height = std::stoul(argv[++i]);
        } else if (arg == "--iso") {
            if (i + 1 < argc) cfg.analogueGain = std::stof(argv[++i]);
        } else if (arg == "--shutter") {
            if (i + 1 < argc) cfg.exposureTime = std::stoul(argv[++i]);
        } else if (arg == "--awb") {
            if (i + 1 < argc) cfg.awbMode = argv[++i];
        } else if (arg == "--ae-disable") {
            cfg.aeEnable = false;
        } else if (arg == "--format") {
            if (i + 1 < argc) {
                std::string fmt = argv[++i];
                if (fmt == "ppm")       cfg.format = OutputFormat::PPM;
                else if (fmt == "raw")  cfg.format = OutputFormat::RAW_NV12;
                else if (fmt == "png")  cfg.format = OutputFormat::PNG;
                else {
                    std::cerr << "Unknown format: " << fmt << " (options: ppm, raw, png)\n";
                    return false;
                }
            }
        } else if (arg == "--awb-disable") {
            cfg.awbEnable = false;
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
