// Diagnostic test: draw a known pattern to the ST7735S display
// to determine the exact pixel mapping for each rotation value.
//
// Build on Pi:
//   g++ -std=c++20 -o display_test tools/display_test.cpp \
//     src/display.cpp src/font.cpp src/camera_mode.cpp \
//     src/image.cpp src/safe_path.cpp \
//     -Isrc -lgpiod
// Run:   ./display_test [rotation]
//          rotation: 0, 90, 180, 270 (default: 90)

#include "display.h"
#include "font.h"
#include "camera_mode.h"
#include "image.h"
#include "safe_path.h"

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

using namespace picamera;

static void setPx(uint8_t *fb, uint32_t w, uint32_t h,
                  int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;
    size_t idx = ((size_t)y * w + x) * 2;
    fb[idx] = color >> 8;
    fb[idx + 1] = color & 0xFF;
}

static void clearScreen(uint8_t *fb, uint32_t w, uint32_t h) {
    std::memset(fb, 0, (size_t)w * h * 2);
}

int main(int argc, char **argv) {
    int rotation = 90;
    if (argc > 1) rotation = std::atoi(argv[1]);

    DisplayConfig cfg;
    cfg.rotation = rotation;

    St7735Display display;
    if (!display.init(cfg)) {
        std::cerr << "Display init failed\n";
        return 1;
    }

    uint32_t w = display.width();
    uint32_t h = display.height();
    std::vector<uint8_t> fb(w * h * 2, 0);

    clearScreen(fb.data(), w, h);

    // Corner markers — each is a 10x10 colored square
    // TL = red, TR = green, BL = yellow, BR = white
    for (int dy = 0; dy < 10; ++dy) {
        for (int dx = 0; dx < 10; ++dx) {
            setPx(fb.data(), w, h, dx, dy, kColorRed);         // TL
            setPx(fb.data(), w, h, w-1-dx, dy, kColorGreen);   // TR
            setPx(fb.data(), w, h, dx, h-1-dy, kColorYellow);  // BL
            setPx(fb.data(), w, h, w-1-dx, h-1-dy, kColorWhite); // BR
        }
    }

    // Corner text labels
    drawText(fb.data(), w, h, 12, 2, "TL", kColorRed, kColorBlack, false);
    drawText(fb.data(), w, h, w - 24, 2, "TR", kColorGreen, kColorBlack, false);
    drawText(fb.data(), w, h, 12, h - 9, "BL", kColorYellow, kColorBlack, false);
    drawText(fb.data(), w, h, w - 24, h - 9, "BR", kColorWhite, kColorBlack, false);

    // Horizontal line across the middle (should be left-to-right if rotation correct)
    for (uint32_t x = 0; x < w; ++x) {
        setPx(fb.data(), w, h, x, h / 2, kColorWhite);
    }

    // Vertical line down the middle (should be top-to-bottom if rotation correct)
    for (uint32_t y = 0; y < h; ++y) {
        setPx(fb.data(), w, h, w / 2, y, kColorWhite);
    }

    // "HORIZ" text along the horizontal line (should read horizontally)
    drawText(fb.data(), w, h, 4, h/2 - 4, "HORIZ", kColorWhite, kColorBlack, false);

    // Rotation label centered
    std::string label = "R" + std::to_string(rotation);
    int lw = static_cast<int>(label.size()) * 6;
    drawText(fb.data(), w, h, (w - lw) / 2, (h - 7) / 2, label,
             kColorWhite, kColorBlack, false);

    // Up arrow (triangle pointing up) above center
    for (int dy = 0; dy < 10; ++dy) {
        int halfW = (10 - dy) / 2;
        for (int dx = -halfW; dx <= halfW; ++dx) {
            setPx(fb.data(), w, h, w/2 + dx, h/2 - 25 + dy, kColorRed);
        }
    }

    display.blit(fb.data());
    std::cout << "Test pattern displayed. Rotation=" << rotation
              << " (" << w << "x" << h << ")\n";

    // Keep displaying for 15 seconds
    std::this_thread::sleep_for(std::chrono::seconds(60));

    display.shutdown();
    return 0;
}
