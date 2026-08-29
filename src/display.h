#pragma once

#include <cstdint>
#include <string>

// Forward declarations of the libgpiod C structs so the header doesn't need
// to pull in <gpiod.h> (kept optional via HAVE_GPIOD). The full definitions
// are only needed in display.cpp, which includes <gpiod.h> directly.
struct gpiod_line_request;
struct gpiod_chip;

namespace picamera {

struct DisplayConfig {
    std::string spiDevice = "/dev/spidev0.0";
    int dcPin = 25;        // BCM pin for D/C
    int resetPin = 27;     // BCM pin for RST
    int backlightPin = 24; // BCM pin for BL
    uint32_t spiSpeed = 16000000; // 16 MHz (stable for ST7735S HAT)
    uint32_t width = 128;
    uint32_t height = 128;
    int rotation = 180;     // 0, 90, 180, 270 (180 = upside-up for Waveshare HAT)
    bool bgr = true;       // ST7735S typically needs BGR
    int colOffset = 2;     // Column offset (ST7735S 128x128: 2)
    int rowOffset = 1;     // Row offset (ST7735S 128x128: 1)
};

// ST7735S SPI display driver via spidev + libgpiod.
// Drives the Waveshare 1.44" LCD HAT (128x128, ST7735S) directly from
// userspace — no kernel overlay or framebuffer required.
class St7735Display {
public:
    bool init(const DisplayConfig &cfg);
    void shutdown();

    // Blit an RGB565 frame (big-endian, width*height*2 bytes) to the display.
    bool blit(const uint8_t *rgb565);

    // Flash the display white then black (capture feedback).
    void flash();

    void setBacklight(bool on);

    uint32_t width() const { return cfg_.width; }
    uint32_t height() const { return cfg_.height; }

private:
    void sendCommand(uint8_t cmd);
    void sendData(const uint8_t *data, size_t len);
    void reset();
    void setAddrWindow();

    DisplayConfig cfg_;
    int spiFd_ = -1;
    gpiod_line_request *gpioReq_ = nullptr;
    gpiod_chip *gpioChip_ = nullptr;
};

} // namespace picamera
