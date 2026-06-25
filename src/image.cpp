#include "image.h"

namespace picamera {

std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv,
                                uint32_t w, uint32_t h, uint32_t stride) {
    auto clamp = [](int v) {
        return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
    };
    size_t rgbSize = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> rgb(rgbSize);

    for (uint32_t yRow = 0; yRow < h; yRow += 2) {
        for (uint32_t x = 0; x < w; x += 2) {
            int U = uv[(yRow / 2) * stride + (x / 2) * 2];
            int V = uv[(yRow / 2) * stride + (x / 2) * 2 + 1];
            int D = U - 128;
            int E = V - 128;
            int Ruv = 409 * E;
            int Guv = -100 * D - 208 * E;
            int Buv = 516 * D;

            for (uint32_t dy = 0; dy < 2 && yRow + dy < h; ++dy) {
                for (uint32_t dx = 0; dx < 2 && x + dx < w; ++dx) {
                    int C = y[(yRow + dy) * stride + (x + dx)] - 16;
                    int R = (298 * C + Ruv + 128) >> 8;
                    int G = (298 * C + Guv + 128) >> 8;
                    int B = (298 * C + Buv + 128) >> 8;
                    size_t off = (static_cast<size_t>(yRow + dy) * w + (x + dx)) * 3;
                    rgb[off + 0] = clamp(R);
                    rgb[off + 1] = clamp(G);
                    rgb[off + 2] = clamp(B);
                }
            }
        }
    }
    return rgb;
}

}
