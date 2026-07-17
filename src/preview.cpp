#include "preview.h"
#include "camera.h"
#include "image.h"

#include <iostream>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

namespace picamera {

namespace {
std::atomic<bool> g_previewStop{false};

void previewSignalHandler(int) {
    g_previewStop.store(true);
}
} // namespace

bool runPreview(const PreviewConfig &pcfg) {
    // --- Open and query the framebuffer ---
    int fbFd = open(pcfg.fbDevice.c_str(), O_RDWR);
    if (fbFd < 0) {
        std::cerr << "Preview: cannot open " << pcfg.fbDevice
                  << ": " << strerror(errno) << "\n";
        return false;
    }

    fb_var_screeninfo vinfo;
    fb_fix_screeninfo finfo;
    if (ioctl(fbFd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        std::cerr << "Preview: FBIOGET_VSCREENINFO failed: " << strerror(errno) << "\n";
        close(fbFd);
        return false;
    }
    if (ioctl(fbFd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        std::cerr << "Preview: FBIOGET_FSCREENINFO failed: " << strerror(errno) << "\n";
        close(fbFd);
        return false;
    }

    uint32_t fbW = vinfo.xres;
    uint32_t fbH = vinfo.yres;
    uint32_t fbBpp = vinfo.bits_per_pixel;
    uint32_t fbStride = finfo.line_length;
    size_t fbSize = static_cast<size_t>(finfo.smem_len);

    std::cout << "Preview: framebuffer " << pcfg.fbDevice
              << " " << fbW << "x" << fbH << " " << fbBpp << "bpp"
              << " (stride=" << fbStride << ")\n";

    // mmap the framebuffer
    void *fbMap = mmap(nullptr, fbSize, PROT_READ | PROT_WRITE, MAP_SHARED, fbFd, 0);
    if (fbMap == MAP_FAILED) {
        std::cerr << "Preview: mmap framebuffer failed: " << strerror(errno) << "\n";
        close(fbFd);
        return false;
    }
    auto *fb = static_cast<uint8_t *>(fbMap);

    // --- Configure camera for low-res preview ---
    CameraConfig camCfg;
    camCfg.width = pcfg.width;
    camCfg.height = pcfg.height;
    camCfg.format = OutputFormat::RAW_NV12;  // we'll convert manually
    camCfg.warmupFrames = 3;  // quick convergence for preview
    camCfg.aeEnable = true;
    camCfg.awbEnable = true;

    CameraApp app;
    if (!app.init()) {
        munmap(fbMap, fbSize);
        close(fbFd);
        return false;
    }
    if (!app.configure(camCfg)) {
        app.shutdown();
        munmap(fbMap, fbSize);
        close(fbFd);
        return false;
    }

    // Install signal handler for clean shutdown
    g_previewStop.store(false);
    auto oldInt = std::signal(SIGINT, previewSignalHandler);
    auto oldTerm = std::signal(SIGTERM, previewSignalHandler);

    // --- Preview capture loop ---
    // We use the camera's capture infrastructure but in a continuous loop.
    // For simplicity, we call capture() repeatedly with a temp file, but
    // that's wasteful. Instead, we should use a streaming approach.
    //
    // For now, we use a simpler approach: configure the camera for video-
    // like capture and grab frames directly. Since our CameraApp is designed
    // for still capture, we'll use a lightweight loop that captures NV12
    // frames and converts them.

    // Actually, let's use the camera's internal capture mechanism but
    // instead of saving to file, we'll grab the NV12 data and convert it.
    // The cleanest way is to add a method to CameraApp that captures a
    // frame and returns the RGB data. But to avoid modifying camera.h
    // extensively, let's use a different approach: capture to a raw NV12
    // file, read it back, convert, and display. This is not ideal for
    // latency but works as a first implementation.

    // TODO: For better performance, add a CameraApp::captureRgb() method
    // that returns the RGB data directly without file I/O.

    auto frameDelay = std::chrono::microseconds(1000000 / pcfg.maxFps);
    uint32_t frameCount = 0;
    bool ok = true;

    // Compute display dimensions
    uint32_t dispW = pcfg.fullscreen ? fbW : pcfg.width;
    uint32_t dispH = pcfg.fullscreen ? fbH : pcfg.height;
    // Centering offset
    uint32_t offX = (fbW - dispW) / 2;
    uint32_t offY = (fbH - dispH) / 2;

    // Scaling factors (if preview res != display res)
    float scaleX = static_cast<float>(pcfg.width) / dispW;
    float scaleY = static_cast<float>(pcfg.height) / dispH;

    while (!g_previewStop.load()) {
        auto frameStart = std::chrono::steady_clock::now();

        // Capture a frame to a temp file (NV12 raw)
        std::string tmpFile = "/tmp/picamera_preview.raw";
        if (!app.capture(tmpFile)) {
            std::cerr << "Preview: capture failed\n";
            ok = false;
            break;
        }

        // Read the NV12 data back
        // Y plane: width * height bytes
        // UV plane: width * (height/2) bytes
        size_t ySize = static_cast<size_t>(pcfg.width) * pcfg.height;
        size_t uvSize = static_cast<size_t>(pcfg.width) * (pcfg.height / 2);
        std::vector<uint8_t> nv12(ySize + uvSize);
        FILE *f = fopen(tmpFile.c_str(), "rb");
        if (!f) { ok = false; break; }
        size_t read = fread(nv12.data(), 1, ySize + uvSize, f);
        fclose(f);
        unlink(tmpFile.c_str());
        if (read < ySize + uvSize) {
            std::cerr << "Preview: short read\n";
            continue;
        }

        // Convert NV12 to RGB
        auto rgb = nv12ToRgb(nv12.data(), nv12.data() + ySize,
                             pcfg.width, pcfg.height, pcfg.width);

        // Write RGB to framebuffer with scaling
        // For RGB888 framebuffer (24/32 bpp)
        if (fbBpp == 16) {
            // RGB565: convert and write
            for (uint32_t dy = 0; dy < dispH; ++dy) {
                uint32_t sy = static_cast<uint32_t>(dy * scaleY);
                if (sy >= pcfg.height) sy = pcfg.height - 1;
                for (uint32_t dx = 0; dx < dispW; ++dx) {
                    uint32_t sx = static_cast<uint32_t>(dx * scaleX);
                    if (sx >= pcfg.width) sx = pcfg.width - 1;
                    size_t srcOff = (static_cast<size_t>(sy) * pcfg.width + sx) * 3;
                    uint16_t r5 = (rgb[srcOff] >> 3) & 0x1F;
                    uint16_t g6 = (rgb[srcOff + 1] >> 2) & 0x3F;
                    uint16_t b5 = (rgb[srcOff + 2] >> 3) & 0x1F;
                    uint16_t pixel = (r5 << 11) | (g6 << 5) | b5;
                    size_t fbOff = static_cast<size_t>(offY + dy) * fbStride +
                                   static_cast<size_t>(offX + dx) * 2;
                    fb[fbOff] = pixel & 0xFF;
                    fb[fbOff + 1] = (pixel >> 8) & 0xFF;
                }
            }
        } else if (fbBpp == 24 || fbBpp == 32) {
            uint32_t bytesPerPixel = fbBpp / 8;
            for (uint32_t dy = 0; dy < dispH; ++dy) {
                uint32_t sy = static_cast<uint32_t>(dy * scaleY);
                if (sy >= pcfg.height) sy = pcfg.height - 1;
                for (uint32_t dx = 0; dx < dispW; ++dx) {
                    uint32_t sx = static_cast<uint32_t>(dx * scaleX);
                    if (sx >= pcfg.width) sx = pcfg.width - 1;
                    size_t srcOff = (static_cast<size_t>(sy) * pcfg.width + sx) * 3;
                    size_t fbOff = static_cast<size_t>(offY + dy) * fbStride +
                                   static_cast<size_t>(offX + dx) * bytesPerPixel;
                    // Handle BGR vs RGB ordering (most fb devices are BGR)
                    if (vinfo.red.offset > vinfo.blue.offset) {
                        // RGB order
                        fb[fbOff] = rgb[srcOff];
                        fb[fbOff + 1] = rgb[srcOff + 1];
                        fb[fbOff + 2] = rgb[srcOff + 2];
                    } else {
                        // BGR order (common for Linux fb)
                        fb[fbOff] = rgb[srcOff + 2];
                        fb[fbOff + 1] = rgb[srcOff + 1];
                        fb[fbOff + 2] = rgb[srcOff];
                    }
                    if (bytesPerPixel == 4) fb[fbOff + 3] = 0;
                }
            }
        } else {
            std::cerr << "Preview: unsupported framebuffer bpp " << fbBpp << "\n";
            ok = false;
            break;
        }

        ++frameCount;
        if (frameCount % 30 == 0) {
            std::cout << "Preview: " << frameCount << " frames\n";
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

    app.shutdown();
    munmap(fbMap, fbSize);
    close(fbFd);

    std::cout << "Preview: stopped after " << frameCount << " frames\n";
    return ok;
}

} // namespace picamera
