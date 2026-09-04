// Asymmetric test: draws a horizontal line near the top (y=10)
// and a vertical line near the left (x=10).
// If rotation is correct: horizontal line at top, vertical line at left.
// If X/Y is swapped: horizontal line appears on left, vertical line at top.

#include "display.h"
#include "font.h"
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

    // Horizontal RED line at y=10 (near top), full width
    for (uint32_t x = 0; x < w; ++x) {
        setPx(fb.data(), w, h, x, 10, 0xF800); // red
        setPx(fb.data(), w, h, x, 11, 0xF800);
    }

    // Vertical GREEN line at x=10 (near left), full height
    for (uint32_t y = 0; y < h; ++y) {
        setPx(fb.data(), w, h, 10, y, 0x07E0); // green
        setPx(fb.data(), w, h, 11, y, 0x07E0);
    }

    // Draw "TOP" text at (30, 3) — should be near top if correct
    drawText(fb.data(), w, h, 30, 3, "TOP", 0xFFFF, 0x0000, false);

    // Draw "LEFT" text at (15, 50) rotated 90° would put it elsewhere
    drawText(fb.data(), w, h, 15, 50, "LEFT", 0xFFFF, 0x0000, false);

    // Big "T" character at center (scale 4) — the top bar should be horizontal
    drawCharScaled(fb.data(), w, h, w/2 - 10, h/2 - 14, 'T', 4,
                   0xFFFF, 0x0000, false);

    display.blit(fb.data());
    std::cout << "Asymmetric test. Rotation=" << rotation << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(60));
    display.shutdown();
    return 0;
}
