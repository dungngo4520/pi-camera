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
};

void printUsage(const char *prog);
bool parseArgs(int argc, char **argv, CliOptions &opts, CameraConfig &cfg);

}
