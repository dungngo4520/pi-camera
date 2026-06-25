#include "camera.h"

#include <iostream>
#include <string>
#include <cstring>

static void printUsage(const char *prog) {
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

/// CLI entry point.
///
/// Parses command-line arguments into a CameraConfig struct, dispatches
/// to the appropriate mode (capture, timelapse, or list-controls), and
/// manages the CameraApp lifecycle (init → configure → capture → shutdown).
///
/// Modes:
///   --capture     Single still frame capture with optional manual controls.
///   --timelapse   Repeated capture at fixed intervals.
///   --list-controls  Print all sensor controls and properties.
int main(int argc, char **argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    CameraConfig cfg;
    std::string mode;              // "capture", "timelapse", or "list-controls"
    std::string captureFile;
    std::string outputPattern = "capture_%04d.ppm";
    int timelapseInterval = 0;
    int timelapseCount = 1;

    // Parse command-line arguments sequentially.
    // Each flag consumes either 0 or 1 subsequent argument.
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);

        if (arg == "--capture") {
            mode = "capture";
            if (i + 1 < argc) captureFile = argv[++i];
        } else if (arg == "--list-controls") {
            mode = "list-controls";
        } else if (arg == "--timelapse") {
            mode = "timelapse";
            if (i + 1 < argc) timelapseInterval = std::stoi(argv[++i]);
        } else if (arg == "--output") {
            if (i + 1 < argc) outputPattern = argv[++i];
        } else if (arg == "--count") {
            if (i + 1 < argc) timelapseCount = std::stoi(argv[++i]);
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
                    return 1;
                }
            }
        } else if (arg == "--awb-disable") {
            cfg.awbEnable = false;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }

    if (mode.empty()) {
        printUsage(argv[0]);
        return 1;
    }

    // Create the camera application and run the requested mode.
    CameraApp app;
    if (!app.init()) return 1;

    if (mode == "list-controls") {
        app.listControls();
        return 0;
    }

    app.configure(cfg);

    if (mode == "capture") {
        app.capture(captureFile.empty() ? "capture.ppm" : captureFile);
    } else if (mode == "timelapse") {
        app.timelapse(timelapseInterval, timelapseCount, outputPattern);
    }

    app.shutdown();
    return 0;
}
