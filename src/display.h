#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Forward declarations of the libgpiod C structs so the header doesn't need
// to pull in <gpiod.h> (libgpiod is a required dependency). The full
// definitions are only needed in display.cpp, which includes <gpiod.h>
// directly.
struct gpiod_line_request;

namespace picamera {

struct DisplayConfig {
  std::string spiDevice = "/dev/spidev0.0";
  int dcPin = 25;               // BCM pin for D/C
  int resetPin = 27;            // BCM pin for RST
  int backlightPin = 24;        // BCM pin for BL
  uint32_t spiSpeed = 16000000; // 16 MHz (stable for ST7735S HAT)
  uint32_t width = 128;
  uint32_t height = 128;
  int rotation = 90; // 0, 90, 180, 270 (90 = upright for Waveshare 1.44" HAT —
                     // panel is physically mounted rotated 90° on the PCB).
                     // Rotation is done entirely in software; no MV bit is
                     // used (MADCTL=0x08, BGR only).
  bool bgr = true;   // ST7735S typically needs BGR
  int colOffset = 2; // Column offset (ST7735S 128x128: 2)
  int rowOffset = 1; // Row offset (ST7735S 128x128: 1)
};

// ST7735S SPI display driver via spidev + libgpiod.
// Drives the Waveshare 1.44" LCD HAT (128x128, ST7735S) directly from
// userspace — no kernel overlay or framebuffer required.
class St7735Display {
public:
  bool init(const DisplayConfig &cfg);
  void shutdown();
  ~St7735Display() noexcept { shutdown(); }

  // Blit an RGB565 frame (big-endian, width*height*2 bytes) to the display.
  bool blit(const uint8_t *rgb565);

  // Blit a sub-region of an RGB565 framebuffer to the display.
  // (srcX, srcY) = top-left in the source framebuffer
  // (dispX, dispY) = top-left on the display
  // (w, h) = region dimensions
  // srcW/srcH = source framebuffer dimensions (for bounds checking)
  // Useful for updating only the overlay area without redrawing the full frame.
  bool blitRegion(const uint8_t *rgb565, uint32_t srcW, uint32_t srcH, int srcX,
                  int srcY, int dispX, int dispY, int w, int h);

  // Flash the display white then black (capture feedback).
  void flash();

  void setBacklight(bool on);

  uint32_t width() const { return cfg_.width; }
  uint32_t height() const { return cfg_.height; }

private:
  bool sendCommand(uint8_t cmd);
  bool sendData(const uint8_t *data, size_t len);
  void reset();
  bool setAddrWindow();
  bool setAddrWindow(int x, int y, int w, int h);

  DisplayConfig cfg_;
  int spiFd_ = -1;
  gpiod_line_request *gpioReq_ = nullptr;
  // Set for rotation 90/270 to enable software pre-rotation in blit()/
  // blitRegion(). No MV bit is used — MADCTL stays at 0x08 (BGR only),
  // so the controller always auto-increments row-by-row.
  bool needsTranspose_ = false;
  std::vector<uint8_t> transposeBuf_;
  // Pre-allocated flash buffers (white/black) to avoid heap churn on
  // every shutter press. Lazily filled on first flash() call.
  std::vector<uint8_t> flashWhite_;
  std::vector<uint8_t> flashBlack_;
};

} // namespace picamera
