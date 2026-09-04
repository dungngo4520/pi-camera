// Simple color bar test — no text, just colored quadrants
// to determine exact pixel mapping.
//
// Build on Pi:
//   g++ -std=c++20 -o color_test tools/color_test.cpp \
//     src/display.cpp src/safe_path.cpp -Isrc -lgpiod
// Run:   ./color_test [rotation]

#include "display.h"
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

    // Fill quadrants with distinct colors:
    //   Top-Left = Red (0xF800)
    //   Top-Right = Green (0x07E0)
    //   Bottom-Left = Blue (0x001F)
    //   Bottom-Right = White (0xFFFF)
    auto setPx = [&](int x, int y, uint16_t c) {
        if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;
        size_t idx = ((size_t)y * w + x) * 2;
        fb[idx] = c >> 8;
        fb[idx + 1] = c & 0xFF;
    };

    uint16_t red   = 0xF800;
    uint16_t green = 0x07E0;
    uint16_t blue  = 0x001F;
    uint16_t white = 0xFFFF;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint16_t c;
            if (y < h/2) {
                if (x < w/2) c = red;      // top-left
                else c = green;             // top-right
            } else {
                if (x < w/2) c = blue;     // bottom-left
                else c = white;             // bottom-right
            }
            setPx(x, y, c);
        }
    }

    // Draw a white cross (lines) at the boundaries for clarity
    for (uint32_t x = 0; x < w; ++x) {
        setPx(x, h/2, 0xFFFF);
        setPx(x, h/2 - 1, 0xFFFF);
    }
    for (uint32_t y = 0; y < h; ++y) {
        setPx(w/2, y, 0xFFFF);
        setPx(w/2 - 1, y, 0xFFFF);
    }

    display.blit(fb.data());
    std::cout << "Color quadrants displayed. Rotation=" << rotation << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(60));
    display.shutdown();
    return 0;
}
