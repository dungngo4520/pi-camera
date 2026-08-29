#include "display.h"

#include <iostream>
#include <cstring>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>

namespace picamera {

namespace {

// ST7735S command definitions
enum St7735Cmd : uint8_t {
    NOP     = 0x00,
    SWRESET = 0x01,
    SLPOUT  = 0x11,
    INVOFF  = 0x20,
    INVON   = 0x21,
    GAMSET  = 0x26,
    DISPOFF = 0x28,
    DISPON  = 0x29,
    CASET   = 0x2A,
    RASET   = 0x2B,
    RAMWR   = 0x2C,
    COLMOD  = 0x3A,
    MADCTL  = 0x36,
    FRMCTR1 = 0xB1,
    INVCTR  = 0xB4,
    PWCTR1  = 0xC0,
    PWCTR2  = 0xC1,
    PWCTR3  = 0xC2,
    PWCTR4  = 0xC3,
    PWCTR5  = 0xC4,
    VMCTR1  = 0xC5,
    GMCTRP1 = 0xE0,
    GMCTRN1 = 0xE1,
};

} // namespace

bool St7735Display::init(const DisplayConfig &cfg) {
    cfg_ = cfg;

    // --- Open gpiochip and request DC, RST, BL as outputs ---
    struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
    if (!chip) {
        std::cerr << "Display: cannot open gpiochip0: " << strerror(errno) << "\n";
        return false;
    }
    gpioChip_ = chip;

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_drive(settings, GPIOD_LINE_DRIVE_PUSH_PULL);

    struct gpiod_line_config *lineCfg = gpiod_line_config_new();
    unsigned int pins[] = {
        static_cast<unsigned>(cfg_.dcPin),
        static_cast<unsigned>(cfg_.resetPin),
        static_cast<unsigned>(cfg_.backlightPin),
    };
    for (size_t i = 0; i < 3; ++i)
        gpiod_line_config_add_line_settings(lineCfg, &pins[i], 1, settings);

    struct gpiod_request_config *reqCfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(reqCfg, "picamera-display");

    struct gpiod_line_request *req =
        gpiod_chip_request_lines(chip, reqCfg, lineCfg);
    gpiod_line_config_free(lineCfg);
    gpiod_request_config_free(reqCfg);
    gpiod_line_settings_free(settings);

    if (!req) {
        std::cerr << "Display: cannot request GPIO lines: " << strerror(errno) << "\n";
        gpiod_chip_close(chip);
        gpioChip_ = nullptr;
        return false;
    }
    gpioReq_ = req;

    // --- Open spidev ---
    spiFd_ = open(cfg_.spiDevice.c_str(), O_RDWR);
    if (spiFd_ < 0) {
        std::cerr << "Display: cannot open " << cfg_.spiDevice
                  << ": " << strerror(errno) << "\n";
        gpiod_line_request_release(req);
        gpiod_chip_close(chip);
        gpioReq_ = nullptr;
        gpioChip_ = nullptr;
        return false;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = cfg_.spiSpeed;
    if (ioctl(spiFd_, SPI_IOC_RD_MODE, &mode) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spiFd_, SPI_IOC_RD_BITS_PER_WORD, &bits) < 0 ||
        ioctl(spiFd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
        ioctl(spiFd_, SPI_IOC_RD_MAX_SPEED_HZ, &speed) < 0) {
        std::cerr << "Display: SPI config failed: " << strerror(errno) << "\n";
        close(spiFd_);
        spiFd_ = -1;
        gpiod_line_request_release(req);
        gpiod_chip_close(chip);
        gpioReq_ = nullptr;
        gpioChip_ = nullptr;
        return false;
    }

    // --- Hardware reset ---
    reset();

    // --- ST7735S initialization sequence (minimal, proven working) ---
    sendCommand(SWRESET);
    usleep(150000); // 150ms
    sendCommand(SLPOUT);
    usleep(120000); // 120ms

    // Pixel format: 16-bit RGB565
    sendCommand(COLMOD);
    uint8_t colmod = 0x05;
    sendData(&colmod, 1);

    // MADCTL: rotation + color order
    sendCommand(MADCTL);
    uint8_t madctl = 0;
    if (cfg_.bgr) madctl |= 0x08;
    switch (cfg_.rotation) {
        case 0:   madctl |= 0x00; break; // normal
        case 90:  madctl |= 0x60; break; // MV | MX
        case 180: madctl |= 0xC0; break; // MX | MY
        case 270: madctl |= 0xA0; break; // MV | MY
        default:  madctl |= 0x00; break; // unknown -> normal
    }
    sendData(&madctl, 1);

    // Set address window
    setAddrWindow();

    // Display on
    sendCommand(DISPON);
    usleep(100000); // 100ms

    // Backlight on
    setBacklight(true);

    std::cout << "Display: ST7735S " << cfg_.width << "x" << cfg_.height
              << " initialized (SPI " << cfg_.spiSpeed / 1000000 << "MHz"
              << ", rotation " << cfg_.rotation << ")\n";
    return true;
}

void St7735Display::reset() {
    auto *req = gpioReq_;
    // RST low → high → wait
    gpiod_line_request_set_value(req, cfg_.resetPin, GPIOD_LINE_VALUE_INACTIVE);
    usleep(10000); // 10ms
    gpiod_line_request_set_value(req, cfg_.resetPin, GPIOD_LINE_VALUE_ACTIVE);
    usleep(120000); // 120ms
}

void St7735Display::sendCommand(uint8_t cmd) {
    auto *req = gpioReq_;
    // DC=0 for command
    gpiod_line_request_set_value(req, cfg_.dcPin, GPIOD_LINE_VALUE_INACTIVE);

    struct spi_ioc_transfer tr = {};
    tr.tx_buf = reinterpret_cast<uintptr_t>(&cmd);
    tr.len = 1;
    tr.speed_hz = cfg_.spiSpeed;
    tr.bits_per_word = 8;

    ioctl(spiFd_, SPI_IOC_MESSAGE(1), &tr);
}

void St7735Display::sendData(const uint8_t *data, size_t len) {
    auto *req = gpioReq_;
    // DC=1 for data
    gpiod_line_request_set_value(req, cfg_.dcPin, GPIOD_LINE_VALUE_ACTIVE);

    struct spi_ioc_transfer tr = {};
    tr.tx_buf = reinterpret_cast<uintptr_t>(data);
    tr.len = len;
    tr.speed_hz = cfg_.spiSpeed;
    tr.bits_per_word = 8;

    ioctl(spiFd_, SPI_IOC_MESSAGE(1), &tr);
}

void St7735Display::setAddrWindow() {
    // Column address set (CASET): start_col, end_col
    uint8_t colData[] = {
        0x00,
        static_cast<uint8_t>(cfg_.colOffset),
        0x00,
        static_cast<uint8_t>(cfg_.colOffset + cfg_.width - 1),
    };
    sendCommand(CASET);
    sendData(colData, 4);

    // Row address set (RASET): start_row, end_row
    uint8_t rowData[] = {
        0x00,
        static_cast<uint8_t>(cfg_.rowOffset),
        0x00,
        static_cast<uint8_t>(cfg_.rowOffset + cfg_.height - 1),
    };
    sendCommand(RASET);
    sendData(rowData, 4);
}

bool St7735Display::blit(const uint8_t *rgb565) {
    if (spiFd_ < 0) return false;

    // Set address window + start memory write
    setAddrWindow();
    sendCommand(RAMWR);

    // Send pixel data in chunks (large single transfers can fail on spidev)
    size_t totalBytes = static_cast<size_t>(cfg_.width) * cfg_.height * 2;
    const size_t chunkSize = 256;
    for (size_t offset = 0; offset < totalBytes; offset += chunkSize) {
        size_t len = std::min(chunkSize, totalBytes - offset);
        sendData(rgb565 + offset, len);
    }
    return true;
}

void St7735Display::flash() {
    size_t totalBytes = static_cast<size_t>(cfg_.width) * cfg_.height * 2;
    std::vector<uint8_t> white(totalBytes, 0xFF);
    std::vector<uint8_t> black(totalBytes, 0x00);

    blit(white.data());
    usleep(80000); // 80ms white flash
    blit(black.data());
}

void St7735Display::setBacklight(bool on) {
    auto *req = gpioReq_;
    gpiod_line_request_set_value(req, cfg_.backlightPin,
        on ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
}

void St7735Display::shutdown() {
    if (spiFd_ >= 0) {
        setBacklight(false);
        sendCommand(DISPOFF);
        close(spiFd_);
        spiFd_ = -1;
    }
    if (gpioReq_) {
        gpiod_line_request_release(gpioReq_);
        gpioReq_ = nullptr;
    }
    if (gpioChip_) {
        gpiod_chip_close(gpioChip_);
        gpioChip_ = nullptr;
    }
}

} // namespace picamera
