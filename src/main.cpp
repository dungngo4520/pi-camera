#include "camera.h"
#include "cli.h"

int main(int argc, char **argv) {
    using namespace picamera;

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    CliOptions opts;
    CameraConfig cfg;
    if (!parseArgs(argc, argv, opts, cfg)) {
        return 1;
    }

    CameraApp app;
    if (!app.init()) return 1;

    if (opts.mode == "list-controls") {
        app.listControls();
        return 0;
    }

    app.configure(cfg);

    bool ok = false;
    if (opts.mode == "capture") {
        ok = app.capture(opts.captureFile.empty() ? "capture.ppm" : opts.captureFile);
    } else if (opts.mode == "timelapse") {
        ok = app.timelapse(opts.timelapseInterval, opts.timelapseCount, opts.outputPattern);
    }

    app.shutdown();
    return ok ? 0 : 1;
}
