#include "camera.h"
#include "cli.h"
#include "preview.h"
#include "timelapse.h"

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
        pcfg.displayCfg.spiDevice = opts.spiDevice;
        pcfg.displayCfg.rotation = opts.displayRotation;
        pcfg.previewWidth = opts.previewWidth;
        pcfg.previewHeight = opts.previewHeight;
        pcfg.maxFps = opts.previewFps;
        pcfg.captureWidth = opts.captureWidth;
        pcfg.captureHeight = opts.captureHeight;
        pcfg.captureFormat = opts.captureFormat;
        pcfg.captureDir = opts.captureDir;
        pcfg.capturePrefix = opts.capturePrefix;
        pcfg.enableBattery = opts.enableBattery;
        pcfg.batteryCfg.i2cDevice = opts.batteryI2cDevice;
        pcfg.batteryCfg.i2cAddress = opts.batteryI2cAddress;
        // Use ±6.144V PGA for direct LiPo measurement (3.0-4.2V)
        pcfg.batteryCfg.pgaGain = 0x0000;
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
        ok = runTimelapse(app, opts.timelapseInterval, opts.timelapseCount, opts.outputPattern);
    }

    app.shutdown();
    return ok ? 0 : 1;
}
