#include "display.h"
#include "safe_path.h" // checkedMul

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <gpiod.h>
#include <iostream>
#include <limits>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace picamera {

namespace {

// ST7735S command definitions
enum St7735Cmd : uint8_t {
  NOP = 0x00,
  SWRESET = 0x01,
  SLPOUT = 0x11,
  INVOFF = 0x20,
  INVON = 0x21,
  GAMSET = 0x26,
  DISPOFF = 0x28,
  DISPON = 0x29,
  CASET = 0x2A,
  RASET = 0x2B,
  RAMWR = 0x2C,
  COLMOD = 0x3A,
  MADCTL = 0x36,
  FRMCTR1 = 0xB1,
  INVCTR = 0xB4,
  PWCTR1 = 0xC0,
  PWCTR2 = 0xC1,
  PWCTR3 = 0xC2,
  PWCTR4 = 0xC3,
  PWCTR5 = 0xC4,
  VMCTR1 = 0xC5,
  GMCTRP1 = 0xE0,
  GMCTRN1 = 0xE1,
};

// Retry SPI ioctl on EINTR (signals from StopFlag can interrupt).
// Returns true on success, false on permanent failure.
bool spiTransfer(int fd, struct spi_ioc_transfer &tr) {
  int rc;
  do {
    rc = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    std::cerr << "Display: SPI transfer failed: " << errnoString(errno) << "\n";
    return false;
  }
  return true;
}

} // namespace

bool St7735Display::init(const DisplayConfig &cfg) {
  cfg_ = cfg;

  // --- Open gpiochip and request DC, RST, BL as outputs ---
  struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
  if (!chip) {
    std::cerr << "Display: cannot open gpiochip0: " << errnoString(errno)
              << "\n";
    return false;
  }

  struct gpiod_line_settings *settings = gpiod_line_settings_new();
  if (!settings) {
    std::cerr << "Display: gpiod_line_settings_new failed (OOM?)\n";
    gpiod_chip_close(chip);
    return false;
  }
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_PUSH_PULL);

  struct gpiod_line_config *lineCfg = gpiod_line_config_new();
  if (!lineCfg) {
    std::cerr << "Display: gpiod_line_config_new failed (OOM?)\n";
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return false;
  }
  unsigned int pins[] = {
      static_cast<unsigned>(cfg_.dcPin),
      static_cast<unsigned>(cfg_.resetPin),
      static_cast<unsigned>(cfg_.backlightPin),
  };
  for (size_t i = 0; i < 3; ++i)
    gpiod_line_config_add_line_settings(lineCfg, &pins[i], 1, settings);

  struct gpiod_request_config *reqCfg = gpiod_request_config_new();
  if (!reqCfg) {
    std::cerr << "Display: gpiod_request_config_new failed (OOM?)\n";
    gpiod_line_config_free(lineCfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return false;
  }
  gpiod_request_config_set_consumer(reqCfg, "picamera-display");

  struct gpiod_line_request *req =
      gpiod_chip_request_lines(chip, reqCfg, lineCfg);
  gpiod_line_config_free(lineCfg);
  gpiod_request_config_free(reqCfg);
  gpiod_line_settings_free(settings);

  if (!req) {
    std::cerr << "Display: cannot request GPIO lines: " << errnoString(errno)
              << "\n";
    gpiod_chip_close(chip);
    return false;
  }
  gpioReq_ = req;
  // The request holds its own reference to the chip; safe to close
  // the local handle now that we know the request succeeded.
  gpiod_chip_close(chip);

  // --- Open spidev ---
  spiFd_ = open(cfg_.spiDevice.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
  if (spiFd_ < 0) {
    std::cerr << "Display: cannot open " << cfg_.spiDevice << ": "
              << errnoString(errno) << "\n";
    gpiod_line_request_release(req);
    gpioReq_ = nullptr;
    return false;
  }

  uint8_t mode = SPI_MODE_0;
  uint8_t bits = 8;
  uint32_t speed = cfg_.spiSpeed;
  // Force SPI mode 0 — don't read back the current mode and re-write it,
  // as the driver default may not be mode 0.
  if (ioctl(spiFd_, SPI_IOC_WR_MODE, &mode) < 0 ||
      ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
      ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
    std::cerr << "Display: SPI config failed: " << errnoString(errno) << "\n";
    close(spiFd_);
    spiFd_ = -1;
    gpiod_line_request_release(req);
    gpioReq_ = nullptr;
    return false;
  }

  // --- Hardware reset ---
  reset();

  // --- ST7735S initialization sequence (minimal, proven working) ---
  bool ok = true;
  if (ok && !sendCommand(SWRESET))
    ok = false;
  if (ok)
    std::this_thread::sleep_for(std::chrono::milliseconds(150)); // 150ms
  if (ok && !sendCommand(SLPOUT))
    ok = false;
  if (ok)
    std::this_thread::sleep_for(std::chrono::milliseconds(120)); // 120ms

  // Pixel format: 16-bit RGB565
  uint8_t colmod = 0x05;
  if (ok && !sendCommand(COLMOD))
    ok = false;
  if (ok && !sendData(&colmod, 1))
    ok = false;

  // MADCTL: rotation + color order
  // software rotation; MADCTL=0 (no MV/MX/MY bits).
  // The Waveshare panel is physically rotated 90° CCW; we rotate the
  // framebuffer in software instead of using MV (which transposes pixels).
  uint8_t madctl = 0;
  if (ok && !sendCommand(MADCTL))
    ok = false;
  if (ok) {
    if (cfg_.bgr)
      madctl |= 0x08;
    // No MV/MX/MY bits — rotation handled in software.
    if (cfg_.rotation == 90 || cfg_.rotation == 270) {
      needsTranspose_ = true;
    }
    if (!sendData(&madctl, 1))
      ok = false;
  }

  // Set address window
  if (ok && !setAddrWindow())
    ok = false;

  // Display on
  if (ok && !sendCommand(DISPON))
    ok = false;
  if (ok)
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 100ms

  if (!ok) {
    std::cerr << "Display: init SPI transfer failed\n";
    close(spiFd_);
    spiFd_ = -1;
    gpiod_line_request_release(req);
    gpioReq_ = nullptr;
    return false;
  }

  // Backlight on
  setBacklight(true);

  std::cout << "Display: ST7735S " << cfg_.width << "x" << cfg_.height
            << " initialized (SPI " << cfg_.spiSpeed / 1000000 << "MHz"
            << ", rotation " << cfg_.rotation << ")\n";
  return true;
}

void St7735Display::reset() {
  if (!gpioReq_)
    return;
  auto *req = gpioReq_;
  // RST low → high → wait
  gpiod_line_request_set_value(req, cfg_.resetPin, GPIOD_LINE_VALUE_INACTIVE);
  std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 10ms
  gpiod_line_request_set_value(req, cfg_.resetPin, GPIOD_LINE_VALUE_ACTIVE);
  std::this_thread::sleep_for(std::chrono::milliseconds(120)); // 120ms
}

bool St7735Display::sendCommand(uint8_t cmd) {
  if (spiFd_ < 0 || !gpioReq_)
    return false;
  auto *req = gpioReq_;
  // DC=0 for command
  gpiod_line_request_set_value(req, cfg_.dcPin, GPIOD_LINE_VALUE_INACTIVE);

  struct spi_ioc_transfer tr = {};
  tr.tx_buf = reinterpret_cast<uintptr_t>(&cmd);
  tr.len = 1;
  tr.speed_hz = cfg_.spiSpeed;
  tr.bits_per_word = 8;

  if (!spiTransfer(spiFd_, tr))
    return false;
  return true;
}

bool St7735Display::sendData(const uint8_t *data, size_t len) {
  if (spiFd_ < 0 || !gpioReq_)
    return false;
  // len must fit in __u32
  if (len > std::numeric_limits<uint32_t>::max()) {
    std::cerr << "Display: SPI transfer too large (" << len << " bytes)\n";
    return false;
  }
  auto *req = gpioReq_;
  // DC=1 for data
  gpiod_line_request_set_value(req, cfg_.dcPin, GPIOD_LINE_VALUE_ACTIVE);

  struct spi_ioc_transfer tr = {};
  tr.tx_buf = reinterpret_cast<uintptr_t>(data);
  tr.len = static_cast<uint32_t>(len);
  tr.speed_hz = cfg_.spiSpeed;
  tr.bits_per_word = 8;

  return spiTransfer(spiFd_, tr);
}

bool St7735Display::setAddrWindow() {
  return setAddrWindow(0, 0, cfg_.width, cfg_.height);
}

bool St7735Display::setAddrWindow(int x, int y, int w, int h) {
  // guard 8-bit address window bounds
  int64_t colStart = static_cast<int64_t>(cfg_.colOffset) + x;
  int64_t rowStart = static_cast<int64_t>(cfg_.rowOffset) + y;
  int64_t colEnd = colStart + w - 1;
  int64_t rowEnd = rowStart + h - 1;
  if (colStart < 0 || colEnd > 255 || rowStart < 0 || rowEnd > 255 ||
      colEnd > static_cast<int64_t>(cfg_.colOffset) + cfg_.width - 1 ||
      rowEnd > static_cast<int64_t>(cfg_.rowOffset) + cfg_.height - 1) {
    std::cerr << "Display: setAddrWindow out of range (col=" << colStart << "-"
              << colEnd << " row=" << rowStart << "-" << rowEnd << ")\n";
    return false;
  }

  // Column address set (CASET): start_col, end_col
  uint8_t colData[] = {
      0x00,
      static_cast<uint8_t>(colStart),
      0x00,
      static_cast<uint8_t>(colEnd),
  };
  if (!sendCommand(CASET))
    return false;
  if (!sendData(colData, 4))
    return false;

  // Row address set (RASET): start_row, end_row
  uint8_t rowData[] = {
      0x00,
      static_cast<uint8_t>(rowStart),
      0x00,
      static_cast<uint8_t>(rowEnd),
  };
  if (!sendCommand(RASET))
    return false;
  if (!sendData(rowData, 4))
    return false;
  return true;
}

bool St7735Display::blit(const uint8_t *rgb565) {
  if (spiFd_ < 0)
    return false;

  // Set address window + start memory write
  if (!setAddrWindow())
    return false;
  if (!sendCommand(RAMWR))
    return false;

  size_t totalBytes = 0;
  if (!checkedMul(static_cast<size_t>(cfg_.width),
                  static_cast<size_t>(cfg_.height), totalBytes) ||
      !checkedMul(totalBytes, size_t{2}, totalBytes)) {
    return false; // overflow — dimensions too large
  }

  // needsTranspose_ (rotation 90/270): pre-rotate framebuffer in software.
  //   90° CW:  dst(x, H-1-y) = src(y, x)   — transpose + Y-flip
  //   270° CW: dst(W-1-x, y) = src(y, x)   — transpose + X-flip
  const uint8_t *sendBuf = rgb565;
  if (needsTranspose_) {
    if (transposeBuf_.size() < totalBytes) {
      transposeBuf_.resize(totalBytes);
    }
    if (cfg_.rotation == 270) {
      for (uint32_t y = 0; y < cfg_.height; ++y) {
        for (uint32_t x = 0; x < cfg_.width; ++x) {
          size_t srcIdx = (static_cast<size_t>(y) * cfg_.width + x) * 2;
          size_t dstIdx =
              (static_cast<size_t>(cfg_.width - 1 - x) * cfg_.height + y) * 2;
          transposeBuf_[dstIdx] = rgb565[srcIdx];
          transposeBuf_[dstIdx + 1] = rgb565[srcIdx + 1];
        }
      }
    } else {
      for (uint32_t y = 0; y < cfg_.height; ++y) {
        uint32_t flippedY = cfg_.height - 1 - y;
        for (uint32_t x = 0; x < cfg_.width; ++x) {
          size_t srcIdx = (static_cast<size_t>(y) * cfg_.width + x) * 2;
          size_t dstIdx = (static_cast<size_t>(x) * cfg_.height + flippedY) * 2;
          transposeBuf_[dstIdx] = rgb565[srcIdx];
          transposeBuf_[dstIdx + 1] = rgb565[srcIdx + 1];
        }
      }
    }
    sendBuf = transposeBuf_.data();
  }

  // chunk at 4096 bytes for SPI reliability
  const size_t chunkSize = 4096;
  for (size_t offset = 0; offset < totalBytes; offset += chunkSize) {
    size_t len = std::min(chunkSize, totalBytes - offset);
    if (!sendData(sendBuf + offset, len))
      return false;
  }
  return true;
}

bool St7735Display::blitRegion(const uint8_t *rgb565, uint32_t srcW,
                               uint32_t srcH, int srcX, int srcY, int dispX,
                               int dispY, int w, int h) {
  if (spiFd_ < 0)
    return false;
  if (w <= 0 || h <= 0)
    return false;
  // Validate source region bounds to prevent out-of-bounds reads.
  if (srcX < 0 || srcY < 0)
    return false;
  // Use int64_t to prevent overflow when srcX+w or srcY+h exceed UINT32_MAX.
  if (static_cast<int64_t>(srcX) + w > static_cast<int64_t>(srcW))
    return false;
  if (static_cast<int64_t>(srcY) + h > static_cast<int64_t>(srcH))
    return false;
  // Validate destination bounds against the physical display dimensions.
  // When needsTranspose_, the transposed region is h wide and w tall.
  if (dispX < 0 || dispY < 0)
    return false;
  if (needsTranspose_) {
    if (static_cast<int64_t>(dispX) + h > cfg_.width)
      return false;
    if (static_cast<int64_t>(dispY) + w > cfg_.height)
      return false;
  } else {
    if (static_cast<int64_t>(dispX) + w > cfg_.width)
      return false;
    if (static_cast<int64_t>(dispY) + h > cfg_.height)
      return false;
  }

  // needsTranspose_ (rotation 90/270): pre-rotate sub-region in software.
  // Transposed region is h×w (h wide, w tall); address window uses (h, w).
  //   90° CW:  dst(col, h-1-row) = src(row, col)
  //   270° CW: dst(w-1-col, row) = src(row, col)
  if (needsTranspose_) {
    int winW = h;
    int winH = w;
    if (!setAddrWindow(dispX, dispY, winW, winH))
      return false;
    if (!sendCommand(RAMWR))
      return false;

    size_t regionBytes = static_cast<size_t>(w) * h * 2;
    if (transposeBuf_.size() < regionBytes) {
      transposeBuf_.resize(regionBytes);
    }
    if (cfg_.rotation == 270) {
      for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
          size_t srcIdx = (static_cast<size_t>(srcY + row) * srcW +
                           static_cast<size_t>(srcX + col)) *
                          2;
          size_t dstIdx = (static_cast<size_t>(w - 1 - col) * h + row) * 2;
          transposeBuf_[dstIdx] = rgb565[srcIdx];
          transposeBuf_[dstIdx + 1] = rgb565[srcIdx + 1];
        }
      }
    } else {
      for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
          size_t srcIdx = (static_cast<size_t>(srcY + row) * srcW +
                           static_cast<size_t>(srcX + col)) *
                          2;
          size_t dstIdx = (static_cast<size_t>(col) * h +
                           static_cast<size_t>(h - 1 - row)) *
                          2;
          transposeBuf_[dstIdx] = rgb565[srcIdx];
          transposeBuf_[dstIdx + 1] = rgb565[srcIdx + 1];
        }
      }
    }
    const size_t chunkSize = 4096;
    for (size_t offset = 0; offset < regionBytes; offset += chunkSize) {
      size_t len = std::min(chunkSize, regionBytes - offset);
      if (!sendData(transposeBuf_.data() + offset, len))
        return false;
    }
    return true;
  }

  // Normal (no rotation): set address window for the sub-region.
  if (!setAddrWindow(dispX, dispY, w, h))
    return false;
  if (!sendCommand(RAMWR))
    return false;

  // Normal (no MV): send pixel data row by row (source rows are
  // contiguous in srcW, but we need to skip the gap between rows
  // in the source framebuffer).
  const size_t chunkSize = 4096;
  for (int row = 0; row < h; ++row) {
    const uint8_t *srcRow =
        rgb565 + (static_cast<size_t>(srcY + row) * srcW + srcX) * 2;
    size_t rowBytes = static_cast<size_t>(w) * 2;

    if (rowBytes <= chunkSize) {
      // If the row fits in one chunk, send directly
      if (!sendData(srcRow, rowBytes))
        return false;
    } else {
      // Split large rows into chunks
      for (size_t offset = 0; offset < rowBytes; offset += chunkSize) {
        size_t len = std::min(chunkSize, rowBytes - offset);
        if (!sendData(srcRow + offset, len))
          return false;
      }
    }
  }
  return true;
}

void St7735Display::flash() {
  size_t totalBytes = 0;
  if (!checkedMul(static_cast<size_t>(cfg_.width), cfg_.height, totalBytes) ||
      !checkedMul(totalBytes, 2, totalBytes)) {
    std::cerr << "Display: flash size overflow\n";
    return;
  }
  try {
    // Lazily initialize the pre-allocated flash buffers on first call.
    // Subsequent flashes reuse the same buffers, avoiding heap churn.
    if (flashWhite_.size() != totalBytes) {
      flashWhite_.assign(totalBytes, 0xFF);
      flashBlack_.assign(totalBytes, 0x00);
    }

    if (!blit(flashWhite_.data())) {
      std::cerr << "Display: flash white blit failed\n";
      return;
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(80)); // 80ms white flash
    if (!blit(flashBlack_.data())) {
      std::cerr << "Display: flash black blit failed\n";
    }
  } catch (const std::bad_alloc &) {
    std::cerr << "Display: flash out of memory\n";
  }
}

void St7735Display::setBacklight(bool on) {
  if (!gpioReq_)
    return;
  auto *req = gpioReq_;
  gpiod_line_request_set_value(req, cfg_.backlightPin,
                               on ? GPIOD_LINE_VALUE_ACTIVE
                                  : GPIOD_LINE_VALUE_INACTIVE);
}

void St7735Display::shutdown() {
  if (spiFd_ >= 0) {
    setBacklight(false);
    (void)sendCommand(DISPOFF); // best-effort during shutdown
    close(spiFd_);
    spiFd_ = -1;
  }
  if (gpioReq_) {
    gpiod_line_request_release(gpioReq_);
    gpioReq_ = nullptr;
  }
}

} // namespace picamera
