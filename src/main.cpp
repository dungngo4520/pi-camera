#include "camera.h"
#include "cli.h"
#include "preview.h"

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

    // Preview mode has its own camera lifecycle.
    if (opts.mode == "preview") {
        PreviewConfig pcfg;
        pcfg.fbDevice = opts.fbDevice;
        pcfg.width = opts.previewWidth;
        pcfg.height = opts.previewHeight;
        pcfg.maxFps = opts.previewFps;
        return runPreview(pcfg) ? 0 : 1;
    }

    CameraApp app;
    if (!app.init()) return 1;

    if (opts.mode == "list-controls") {
        app.listControls();
        return 0;
    }

    if (!app.configure(cfg)) {
        app.shutdown();
        return 1;
    }

    bool ok = false;
    if (opts.mode == "capture") {
        if (!cfg.bracketEv.empty()) {
            ok = app.captureBracket(opts.captureFile.empty() ? "capture.ppm" : opts.captureFile);
        } else {
            ok = app.capture(opts.captureFile.empty() ? "capture.ppm" : opts.captureFile);
        }
    } else if (opts.mode == "timelapse") {
        ok = app.timelapse(opts.timelapseInterval, opts.timelapseCount, opts.outputPattern);
    }

    app.shutdown();
    return ok ? 0 : 1;
}
