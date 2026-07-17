#pragma once

#include "camera.h"
#include <string>

namespace picamera {

struct CliOptions {
    std::string mode;
    std::string captureFile;
    std::string outputPattern = "capture_%04d.ppm";
    int timelapseInterval = 0;
    int timelapseCount = 1;
    // Preview mode
    std::string fbDevice = "/dev/fb0";
    uint32_t previewWidth = 320;
    uint32_t previewHeight = 240;
    uint32_t previewFps = 15;
};

void printUsage(const char *prog);
bool parseArgs(int argc, char **argv, CliOptions &opts, CameraConfig &cfg);

}
