// Font test: display every character in the font on the LCD.
//
// Build on Pi:
//   g++ -std=c++20 -o font_test tools/font_test.cpp \
//     src/display.cpp src/font.cpp src/camera_mode.cpp \
//     src/image.cpp src/safe_path.cpp -Isrc -lgpiod
// Run:   ./font_test [rotation]

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
    std::memset(fb.data(), 0, fb.size());

    // All characters in the font, in rows of 16 chars each.
    // 6px per char, 16 chars = 96px wide (fits in 128).
    // 7px per row, 5 rows = 35px tall (fits in 128).
    const char *rows[] = {
        " ABCDEFGHIJKLMNO",
        "PQRSTUVWXYZ01234",
        "56789%.V-+",
    };

    int yStart = 16;
    for (int r = 0; r < 3; ++r) {
        drawText(fb.data(), w, h, 4, yStart + r * 10, rows[r],
                 kColorWhite, kColorBlack, false);
    }

    // Draw a border to verify orientation
    for (uint32_t x = 0; x < w; ++x) {
        size_t idx = (x) * 2;
        fb[idx] = 0xFF; fb[idx+1] = 0xFF; // top edge
        idx = ((h-1) * w + x) * 2;
        fb[idx] = 0xFF; fb[idx+1] = 0xFF; // bottom edge
    }
    for (uint32_t y = 0; y < h; ++y) {
        size_t idx = (y * w) * 2;
        fb[idx] = 0xFF; fb[idx+1] = 0xFF; // left edge
        idx = (y * w + w-1) * 2;
        fb[idx] = 0xFF; fb[idx+1] = 0xFF; // right edge
    }

    display.blit(fb.data());
    std::cout << "Font test displayed. Rotation=" << rotation << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(120));
    display.shutdown();
    return 0;
}
