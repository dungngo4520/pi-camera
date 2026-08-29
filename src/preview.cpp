#include "preview.h"
#include "camera.h"
#include "image.h"
#include "stream.h"

#ifdef HAVE_GPIOD
#include "display.h"
#include "buttons.h"
#include "font.h"
#endif

#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <ctime>
#include <sys/stat.h>

namespace picamera {

namespace {
std::atomic<bool> g_previewStop{false};

void previewSignalHandler(int) {
    g_previewStop.store(true);
}

// Generate a timestamped filename: prefix_YYYYMMDD-HHMMSS.ext
std::string makeCaptureFilename(const std::string &dir,
                                const std::string &prefix,
                                const std::string &fmt) {
    auto now = std::time(nullptr);
    auto *tm = std::localtime(&now);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", tm);
    std::string ext = (fmt == "jpeg" || fmt == "jpg") ? "jpg" :
                      (fmt == "png") ? "png" :
                      (fmt == "dng") ? "dng" : "ppm";
    return dir + "/" + prefix + "_" + buf + "." + ext;
}

} // namespace

bool runPreview(const PreviewConfig &pcfg) {
#ifdef HAVE_GPIOD
    // --- Initialize SPI display ---
    St7735Display display;
    if (!display.init(pcfg.displayCfg)) {
        return false;
    }

    // --- Initialize buttons ---
    ButtonInput buttons;
    if (!buttons.init()) {
        display.shutdown();
        return false;
    }

    // --- Initialize camera stream ---
    CameraStream stream;
    if (!stream.init()) {
        buttons.shutdown();
        display.shutdown();
        return false;
    }
    if (!stream.start(pcfg.previewWidth, pcfg.previewHeight)) {
        stream.shutdown();
        buttons.shutdown();
        display.shutdown();
        return false;
    }

    // --- Initialize battery monitor (optional) ---
    BatteryMonitor battery;
    bool batteryOk = false;
    if (pcfg.enableBattery) {
        batteryOk = battery.init(pcfg.batteryCfg);
        if (!batteryOk) {
            std::cerr << "Preview: battery monitor init failed — continuing without overlay\n";
        }
    }
    BatteryReading lastBattery;
    auto lastBatteryRead = std::chrono::steady_clock::time_point::min();
    const auto batteryReadInterval = std::chrono::seconds(3);

    // Install signal handler for clean shutdown
    g_previewStop.store(false);
    auto oldInt = std::signal(SIGINT, previewSignalHandler);
    auto oldTerm = std::signal(SIGTERM, previewSignalHandler);

    // Allocate RGB565 framebuffer for the display
    size_t dispPixels = static_cast<size_t>(display.width()) * display.height();
    std::vector<uint8_t> rgb565(dispPixels * 2);

    auto frameDelay = std::chrono::microseconds(1000000 / pcfg.maxFps);
    uint32_t frameCount = 0;
    uint32_t captureCount = 0;
    bool ok = true;

    std::cout << "Preview: streaming " << stream.width() << "x" << stream.height()
              << " -> display " << display.width() << "x" << display.height()
              << " (max " << pcfg.maxFps << " fps)\n";
    std::cout << "Preview: press joystick to capture, Ctrl+C to exit\n";

    while (!g_previewStop.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        // Grab a frame from the camera stream
        auto frame = stream.grabFrame(2000);
        if (!frame.y) {
            if (g_previewStop.load()) break;
            std::cerr << "Preview: frame timeout\n";
            continue;
        }

        // Convert NV12 to RGB565 with center-crop + scaling
        nv12ToRgb565Scaled(frame.y, frame.uv,
                           frame.width, frame.height, frame.stride,
                           rgb565.data(), display.width(), display.height());

        // --- Battery overlay (drawn on framebuffer before blit) ---
        if (batteryOk) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastBatteryRead > batteryReadInterval) {
                lastBattery = battery.read();
                lastBatteryRead = now;
            }
            if (lastBattery.valid) {
                // Battery icon in top-right corner
                int iconX = static_cast<int>(display.width()) - 22;
                int iconY = 2;
                drawBatteryIcon(rgb565.data(), display.width(), display.height(),
                                iconX, iconY, lastBattery.percent);
                // Percentage text below the icon
                std::string pctStr = std::to_string(lastBattery.percent) + "%";
                drawText(rgb565.data(), display.width(), display.height(),
                         iconX - 2, iconY + 11, pctStr,
                         COLOR_WHITE, COLOR_BLACK, false);
            }
        }

        // Blit to display
        display.blit(rgb565.data());

        ++frameCount;
        if (frameCount % 60 == 0) {
            std::cout << "Preview: " << frameCount << " frames, "
                      << captureCount << " captures\n";
        }

        // Check for button press (non-blocking)
        ButtonEvent evt = buttons.poll(0);
        if (evt.pressed && evt.id == ButtonId::Shutter) {
            ++captureCount;
            std::cout << "Preview: shutter pressed — capturing full-res...\n";

            // Flash the display for visual feedback
            display.flash();

            // Stop the stream to release the camera
            stream.stop();
            stream.shutdown();

            // Give the kernel time to free the V4L2 buffers before
            // re-allocating for full-res capture (avoids ENOMEM on 512MB Pi)
            usleep(500000);

            // Capture full-res still using CameraApp
            {
                CameraApp app;
                if (app.init()) {
                    CameraConfig cfg;
                    cfg.width = pcfg.captureWidth;
                    cfg.height = pcfg.captureHeight;
                    cfg.warmupFrames = 5;
                    cfg.aeEnable = true;
                    cfg.awbEnable = true;

                    if (pcfg.captureFormat == "jpeg" || pcfg.captureFormat == "jpg") {
                        cfg.format = OutputFormat::JPEG;
                    } else if (pcfg.captureFormat == "png") {
                        cfg.format = OutputFormat::PNG;
                    } else if (pcfg.captureFormat == "dng") {
                        cfg.format = OutputFormat::DNG;
                    } else {
                        cfg.format = OutputFormat::PPM;
                    }

                    if (app.configure(cfg)) {
                        std::string filename = makeCaptureFilename(
                            pcfg.captureDir, pcfg.capturePrefix, pcfg.captureFormat);
                        if (app.capture(filename)) {
                            std::cout << "Preview: saved " << filename << "\n";
                        } else {
                            std::cerr << "Preview: capture failed\n";
                            ok = false;
                        }
                    }
                    app.shutdown();
                }
            }

            // Restart the stream
            if (!stream.init() || !stream.start(pcfg.previewWidth, pcfg.previewHeight)) {
                std::cerr << "Preview: failed to restart stream\n";
                ok = false;
                break;
            }
        }

        // Frame rate limiting
        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < frameDelay) {
            std::this_thread::sleep_for(frameDelay - elapsed);
        }
    }

    // Restore signal handlers
    std::signal(SIGINT, oldInt);
    std::signal(SIGTERM, oldTerm);

    stream.shutdown();
    buttons.shutdown();
    display.shutdown();
    if (batteryOk) battery.shutdown();

    std::cout << "Preview: stopped after " << frameCount << " frames, "
              << captureCount << " captures\n";
    return ok;
#else
    (void)pcfg;
    std::cerr << "Preview: libgpiod was not available at build time.\n"
              << "Rebuild on the Pi with libgpiod-dev installed.\n";
    return false;
#endif
}

} // namespace picamera
