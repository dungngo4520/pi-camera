#pragma once

#include <cstdint>
#include <string>

namespace picamera {

struct PreviewConfig {
    std::string fbDevice = "/dev/fb0";
    uint32_t width = 320;       // preview capture resolution
    uint32_t height = 240;
    bool fullscreen = true;     // stretch to fill framebuffer
    uint32_t maxFps = 15;       // cap to avoid burning CPU on Pi Zero
};

// Run a live preview loop: capture frames at low resolution, convert to
// RGB, and write them to the Linux framebuffer. Blocks until SIGINT/SIGTERM.
// Returns true on clean shutdown, false on error.
bool runPreview(const PreviewConfig &pcfg);

} // namespace picamera
