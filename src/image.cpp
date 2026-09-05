#include "image.h"
#include "safe_path.h"

#include <thread>
#include <vector>
#include <algorithm>
#include <cstring>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define PICAMERA_HAS_NEON 1
#endif

namespace picamera {

namespace {

inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

// Sample one NV12 pixel at (sx, sy) and convert to RGB565 (RRRRRGGGGGGBBBBB).
// Guards odd-width UV reads: if the V byte is past the luma width, use the
// stride padding if available, else replicate the previous UV pair.
inline uint16_t nv12ToRgb565Pixel(const uint8_t *y, const uint8_t *uv,
                                  uint32_t stride, uint32_t sx, uint32_t sy) {
    int Y = y[static_cast<size_t>(sy) * stride + sx];
    uint32_t uvX = (sx / 2) * 2;
    uint32_t uvY = sy / 2;
    int U;
    int V;
    if (uvX + 1 < stride) {
        size_t uvBase = static_cast<size_t>(uvY) * stride;
        U = uv[uvBase + uvX] - 128;
        V = uv[uvBase + uvX + 1] - 128;
    } else if (uvX >= 2) {
        size_t uvBase = static_cast<size_t>(uvY) * stride;
        U = uv[uvBase + uvX - 2] - 128;
        V = uv[uvBase + uvX - 1] - 128;
    } else {
        U = 0;
        V = 0;
    }
    int C = Y - 16;
    int R = (298 * C + 409 * V + 128) >> 8;
    int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
    int B = (298 * C + 516 * U + 128) >> 8;
    return static_cast<uint16_t>(
        ((clamp8(R) >> 3) << 11) | ((clamp8(G) >> 2) << 5) | (clamp8(B) >> 3));
}

// Store a 16-bit RGB565 value big-endian (high byte first) for SPI displays.
inline void storeRgb565Be(uint8_t *out, uint16_t pixel, size_t idx) {
    out[idx] = static_cast<uint8_t>(pixel >> 8);
    out[idx + 1] = static_cast<uint8_t>(pixel & 0xFF);
}

// ---------------------------------------------------------------------------
// Scalar reference implementation — the tested path on x86 and the fallback
// for odd-sized remainders on NEON builds.
// ---------------------------------------------------------------------------
void nv12ToRgbRowPair_scalar(const uint8_t *yRow0, const uint8_t *yRow1,
                              const uint8_t *uvRow, uint8_t *rgb0, uint8_t *rgb1,
                              uint32_t w, uint32_t stride, bool haveRow1) {
    for (uint32_t x = 0; x < w; x += 2) {
        // For odd widths, the last iteration has x = w-1 and the V byte
        // is at x+1 = w. Guard against the UV row bound (stride), not the
        // luma width, because for odd w with stride > w the V byte at
        // position w is valid (it's in the row padding). Only fall back to
        // replicating the previous UV pair when the V byte is truly absent
        // (stride == w, i.e. no padding). Use neutral chroma (U=V=128)
        // when there is no previous UV pair.
        int U;
        int V;
        if (x + 1 < stride) {
            U = uvRow[x];
            V = uvRow[x + 1];
        } else if (x >= 2) {
            U = uvRow[x - 2];
            V = uvRow[x - 1];
        } else {
            U = 128;
            V = 128;
        }
        int D = U - 128;
        int E = V - 128;
        int Ruv = 409 * E;
        int Guv = -100 * D - 208 * E;
        int Buv = 516 * D;

        for (uint32_t dx = 0; dx < 2 && x + dx < w; ++dx) {
            int C0 = yRow0[x + dx] - 16;
            rgb0[(x + dx) * 3 + 0] = clamp8((298 * C0 + Ruv + 128) >> 8);
            rgb0[(x + dx) * 3 + 1] = clamp8((298 * C0 + Guv + 128) >> 8);
            rgb0[(x + dx) * 3 + 2] = clamp8((298 * C0 + Buv + 128) >> 8);

            if (haveRow1) {
                int C1 = yRow1[x + dx] - 16;
                rgb1[(x + dx) * 3 + 0] = clamp8((298 * C1 + Ruv + 128) >> 8);
                rgb1[(x + dx) * 3 + 1] = clamp8((298 * C1 + Guv + 128) >> 8);
                rgb1[(x + dx) * 3 + 2] = clamp8((298 * C1 + Buv + 128) >> 8);
            }
        }
    }
}

#ifdef PICAMERA_HAS_NEON
// ---------------------------------------------------------------------------
// NEON SIMD path — processes 16 pixels per row-pair iteration on ARM.
//
// BT.601 limited-range NV12->RGB24:
//   C = Y - 16,  D = U - 128,  E = V - 128
//   R = clamp((298*C + 409*E + 128) >> 8)
//   G = clamp((298*C - 100*D - 208*E + 128) >> 8)
//   B = clamp((298*C + 516*D + 128) >> 8)
//
// The products (298*239, 516*127, etc.) overflow int16, so the multiply-
// accumulate uses int32x4_t (vmull_n_s16 / vmlal_n_s16 / vmlsl_n_s16).
// vqshrun_n_s32 does the saturating right-shift-by-8 directly to uint16,
// which clamps to [0, 65535] — but since the math is designed to land in
// [0, 255], the result fits in uint8 after narrowing.
//
// NV12 chroma subsampling: each UV pair covers a 2x2 Y block. For 16 Y
// pixels there are 8 UV pairs (16 bytes). vld2_u8 deinterleaves them into
// 8 U and 8 V values; vzip_u8 duplicates each to cover 2 pixels.
// ---------------------------------------------------------------------------
// Convert 8 Y pixels (one row) to 8 RGB triplets, given 8 U and 8 V values
// (already deinterleaved and duplicated to per-pixel granularity).
static inline void convert8pixels_neon(uint8x8_t y8, uint8x8_t u8, uint8x8_t v8,
                                        uint8_t *out) {
    // Widen to int16 and subtract offsets.
    int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8(y8));
    y16 = vsubq_s16(y16, vdupq_n_s16(16));
    int16x8_t u16 = vreinterpretq_s16_u16(vmovl_u8(u8));
    u16 = vsubq_s16(u16, vdupq_n_s16(128));
    int16x8_t v16 = vreinterpretq_s16_u16(vmovl_u8(v8));
    v16 = vsubq_s16(v16, vdupq_n_s16(128));

    int16x4_t y_lo = vget_low_s16(y16);
    int16x4_t y_hi = vget_high_s16(y16);
    int16x4_t u_lo = vget_low_s16(u16);
    int16x4_t u_hi = vget_high_s16(u16);
    int16x4_t v_lo = vget_low_s16(v16);
    int16x4_t v_hi = vget_high_s16(v16);

    // Y * 298 (widening to int32)
    int32x4_t y298_lo = vmull_n_s16(y_lo, 298);
    int32x4_t y298_hi = vmull_n_s16(y_hi, 298);

    // R = (Y*298 + V*409 + 128) >> 8
    int32x4_t r_lo = vaddq_s32(vmlal_n_s16(y298_lo, v_lo, 409), vdupq_n_s32(128));
    int32x4_t r_hi = vaddq_s32(vmlal_n_s16(y298_hi, v_hi, 409), vdupq_n_s32(128));
    uint16x4_t r16_lo = vqshrun_n_s32(r_lo, 8);
    uint16x4_t r16_hi = vqshrun_n_s32(r_hi, 8);
    uint8x8_t r8 = vqmovn_u16(vcombine_u16(r16_lo, r16_hi));

    // G = (Y*298 - U*100 - V*208 + 128) >> 8
    int32x4_t g_lo = vmlsl_n_s16(y298_lo, u_lo, 100);
    g_lo = vmlsl_n_s16(g_lo, v_lo, 208);
    g_lo = vaddq_s32(g_lo, vdupq_n_s32(128));
    int32x4_t g_hi = vmlsl_n_s16(y298_hi, u_hi, 100);
    g_hi = vmlsl_n_s16(g_hi, v_hi, 208);
    g_hi = vaddq_s32(g_hi, vdupq_n_s32(128));
    uint16x4_t g16_lo = vqshrun_n_s32(g_lo, 8);
    uint16x4_t g16_hi = vqshrun_n_s32(g_hi, 8);
    uint8x8_t g8 = vqmovn_u16(vcombine_u16(g16_lo, g16_hi));

    // B = (Y*298 + U*516 + 128) >> 8
    int32x4_t b_lo = vaddq_s32(vmlal_n_s16(y298_lo, u_lo, 516), vdupq_n_s32(128));
    int32x4_t b_hi = vaddq_s32(vmlal_n_s16(y298_hi, u_hi, 516), vdupq_n_s32(128));
    uint16x4_t b16_lo = vqshrun_n_s32(b_lo, 8);
    uint16x4_t b16_hi = vqshrun_n_s32(b_hi, 8);
    uint8x8_t b8 = vqmovn_u16(vcombine_u16(b16_lo, b16_hi));

    // Interleave R, G, B into RGBRGB... and store 24 bytes (8 pixels).
    uint8x8x3_t rgb;
    rgb.val[0] = r8;
    rgb.val[1] = g8;
    rgb.val[2] = b8;
    vst3_u8(out, rgb);
}

static void nv12ToRgbRowPair_neon(const uint8_t *yRow0, const uint8_t *yRow1,
                                   const uint8_t *uvRow, uint8_t *rgb0,
                                   uint8_t *rgb1, uint32_t w, uint32_t stride,
                                   bool haveRow1) {
    uint32_t x = 0;
    // Process 16 pixels per iteration (8 UV pairs, two 8-pixel halves).
    while (x + 16 <= w) {
        // Load 8 UV pairs (16 bytes), deinterleave into 8 U + 8 V.
        uint8x8x2_t uv_pair = vld2_u8(&uvRow[x]);
        uint8x8_t u = uv_pair.val[0];
        uint8x8_t v = uv_pair.val[1];
        // Duplicate each U/V to cover 2 pixels: [U0 U0 U1 U1 ... U7 U7].
        uint8x8x2_t u_dup = vzip_u8(u, u);
        uint8x8x2_t v_dup = vzip_u8(v, v);

        // First 8 pixels (low half).
        convert8pixels_neon(vld1_u8(&yRow0[x]),
                            u_dup.val[0], v_dup.val[0],
                            &rgb0[x * 3]);
        if (haveRow1) {
            convert8pixels_neon(vld1_u8(&yRow1[x]),
                                u_dup.val[0], v_dup.val[0],
                                &rgb1[x * 3]);
        }

        // Second 8 pixels (high half).
        convert8pixels_neon(vld1_u8(&yRow0[x + 8]),
                            u_dup.val[1], v_dup.val[1],
                            &rgb0[(x + 8) * 3]);
        if (haveRow1) {
            convert8pixels_neon(vld1_u8(&yRow1[x + 8]),
                                u_dup.val[1], v_dup.val[1],
                                &rgb1[(x + 8) * 3]);
        }
        x += 16;
    }

    // Scalar fallback for the last <16 pixels (handles odd widths and
    // the 8-pixel NEON remainder safely without over-reading UV).
    if (x < w) {
        nv12ToRgbRowPair_scalar(yRow0 + x, yRow1 + x, uvRow + x,
                                rgb0 + x * 3, rgb1 + x * 3,
                                w - x, stride - x, haveRow1);
    }
}
#endif // PICAMERA_HAS_NEON

// Dispatch to NEON if available, else scalar. Processes one 2-row pair.
void nv12ToRgbRowPair(const uint8_t *yRow0, const uint8_t *yRow1,
                       const uint8_t *uvRow, uint8_t *rgb0, uint8_t *rgb1,
                       uint32_t w, uint32_t stride, bool haveRow1) {
#ifdef PICAMERA_HAS_NEON
    nv12ToRgbRowPair_neon(yRow0, yRow1, uvRow, rgb0, rgb1, w, stride, haveRow1);
#else
    nv12ToRgbRowPair_scalar(yRow0, yRow1, uvRow, rgb0, rgb1, w, stride, haveRow1);
#endif
}

} // namespace

// ---------------------------------------------------------------------------
// Public API: convert a full NV12 frame to RGB24.
//
// Multi-threaded: splits the image into horizontal strips (one per thread,
// up to hardware_concurrency) and converts them in parallel. Each strip is
// a multiple of 2 rows (NV12 chroma subsampling). For small images or single-
// core systems, falls back to single-threaded.
// ---------------------------------------------------------------------------
std::vector<uint8_t> nv12ToRgb(const uint8_t *y, const uint8_t *uv,
                                uint32_t w, uint32_t h, uint32_t stride,
                                size_t ySize, size_t uvSize) {
    if (!y || !uv || w == 0 || h == 0 || stride < w) return {};

    size_t needY = 0;
    size_t needUv = 0;
    if (!checkedMul(static_cast<size_t>(stride), h, needY)) return {};
    if (!checkedMul(static_cast<size_t>(stride), (h + 1) / 2, needUv)) return {};
    if (ySize < needY || uvSize < needUv) return {};

    size_t rgbSize = 0;
    if (!checkedMul(static_cast<size_t>(w), h, rgbSize)) return {};
    if (!checkedMul(rgbSize, 3, rgbSize)) return {};
    std::vector<uint8_t> rgb(rgbSize);

    const unsigned nThreads = std::min<unsigned>(
        std::thread::hardware_concurrency(),
        (h + 1) / 2);  // one thread per 2-row pair, capped

    if (nThreads <= 1 || h < 8) {
        for (uint32_t yRow = 0; yRow < h; yRow += 2) {
            bool haveRow1 = (yRow + 1 < h);
            size_t yOff0 = static_cast<size_t>(yRow) * stride;
            size_t yOff1 = static_cast<size_t>(yRow + 1) * stride;
            size_t uvOff = static_cast<size_t>(yRow / 2) * stride;
            size_t rgbOff0 = static_cast<size_t>(yRow) * w * 3;
            size_t rgbOff1 = static_cast<size_t>(yRow + 1) * w * 3;
            nv12ToRgbRowPair(
                y + yOff0,
                haveRow1 ? y + yOff1 : y + yOff0,
                uv + uvOff,
                rgb.data() + rgbOff0,
                haveRow1 ? rgb.data() + rgbOff1 : rgb.data() + rgbOff0,
                w, stride, haveRow1);
        }
        return rgb;
    }

    // Multi-threaded: divide row pairs among threads.
    const uint32_t rowPairs = (h + 1) / 2;
    const uint32_t pairsPerThread = (rowPairs + nThreads - 1) / nThreads;

    std::vector<std::thread> threads;
    threads.reserve(nThreads);

    // RAII joiner: if emplace_back throws, already-started threads
    // are joined before the vector destructor would call std::terminate.
    class ThreadJoiner {
    public:
        explicit ThreadJoiner(std::vector<std::thread> &ts) : ts_(ts) {}
        ~ThreadJoiner() { for (auto &t : ts_) if (t.joinable()) t.join(); }
    private:
        std::vector<std::thread> &ts_;
    } joiner{threads};

    for (unsigned t = 0; t < nThreads; ++t) {
        uint32_t startPair = t * pairsPerThread;
        uint32_t endPair = std::min(startPair + pairsPerThread, rowPairs);
        if (startPair >= endPair) break;

        threads.emplace_back([&, startPair, endPair] {
            for (uint32_t p = startPair; p < endPair; ++p) {
                uint32_t yRow = p * 2;
                bool haveRow1 = (yRow + 1 < h);
                size_t yOff0 = static_cast<size_t>(yRow) * stride;
                size_t yOff1 = static_cast<size_t>(yRow + 1) * stride;
                size_t uvOff = static_cast<size_t>(p) * stride;
                size_t rgbOff0 = static_cast<size_t>(yRow) * w * 3;
                size_t rgbOff1 = static_cast<size_t>(yRow + 1) * w * 3;
                nv12ToRgbRowPair(
                    y + yOff0,
                    haveRow1 ? y + yOff1 : y + yOff0,
                    uv + uvOff,
                    rgb.data() + rgbOff0,
                    haveRow1 ? rgb.data() + rgbOff1 : rgb.data() + rgbOff0,
                    w, stride, haveRow1);
            }
        });
    }
    for (auto &th : threads) th.join();
    return rgb;
}

// ---------------------------------------------------------------------------
// NV12 -> RGB565 with center-crop + nearest-neighbor scaling.
//
// Output is big-endian RGB565 (high byte first) for SPI displays like the
// ST7735S. The source is center-cropped to match the display aspect ratio,
// then scaled to dispW x dispH.
// ---------------------------------------------------------------------------
bool nv12ToRgb565Scaled(const uint8_t *y, const uint8_t *uv,
                        uint32_t srcW, uint32_t srcH, uint32_t stride,
                        size_t ySize, size_t uvSize,
                        uint8_t *out, uint32_t dispW, uint32_t dispH,
                        size_t outSize) {
    if (!y || !uv || !out || srcW == 0 || srcH == 0 || dispW == 0 || dispH == 0 ||
        stride < srcW)
        return false;
    size_t needY = 0;
    size_t needUv = 0;
    if (!checkedMul(static_cast<size_t>(stride), srcH, needY)) return false;
    if (!checkedMul(static_cast<size_t>(stride), (srcH + 1) / 2, needUv)) return false;
    if (ySize < needY || uvSize < needUv) return false;
    size_t requiredOut = 0;
    if (!checkedMul(static_cast<size_t>(dispW), dispH, requiredOut) ||
        !checkedMul(requiredOut, 2, requiredOut)) return false;
    if (outSize < requiredOut) return false;

    float srcAspect = static_cast<float>(srcW) / srcH;
    float dispAspect = static_cast<float>(dispW) / dispH;

    uint32_t cropW;
    uint32_t cropH;
    if (srcAspect > dispAspect) {
        cropH = srcH;
        cropW = static_cast<uint32_t>(srcH * dispAspect);
    } else {
        cropW = srcW;
        cropH = static_cast<uint32_t>(srcW / dispAspect);
    }
    // Clamp before computing offsets to prevent unsigned underflow.
    cropW = std::min(cropW, srcW);
    cropH = std::min(cropH, srcH);
    cropW = std::max(cropW, 1u);
    cropH = std::max(cropH, 1u);
    uint32_t cropX;
    uint32_t cropY;
    if (srcAspect > dispAspect) {
        cropX = (srcW - cropW) / 2;
        cropY = 0;
    } else {
        cropX = 0;
        cropY = (srcH - cropH) / 2;
    }

    for (uint32_t dy = 0; dy < dispH; ++dy) {
        // Map display row to source row (nearest-neighbor). uint64_t for the
        // product: dy*cropH can exceed 2^32 for large source resolutions.
        uint32_t sy = cropY + static_cast<uint32_t>(
            (static_cast<uint64_t>(dy) * cropH) / dispH);
        if (sy >= srcH) sy = srcH - 1;

        for (uint32_t dx = 0; dx < dispW; ++dx) {
            uint32_t sx = cropX + static_cast<uint32_t>(
                (static_cast<uint64_t>(dx) * cropW) / dispW);
            if (sx >= srcW) sx = srcW - 1;
            uint16_t pixel = nv12ToRgb565Pixel(y, uv, stride, sx, sy);
            storeRgb565Be(out, pixel, (static_cast<size_t>(dy) * dispW + dx) * 2);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// NV12 -> RGB565 with explicit crop region + nearest-neighbor scaling.
// Used by the focus magnifier: crop the center of the viewfinder and scale
// to the display. cropX/cropY must be even (NV12 chroma 2x2 subsampling).
// ---------------------------------------------------------------------------
bool nv12ToRgb565CroppedScaled(const uint8_t *y, const uint8_t *uv,
                               uint32_t srcW, uint32_t srcH, uint32_t stride,
                               size_t ySize, size_t uvSize,
                               uint32_t cropX, uint32_t cropY,
                               uint32_t cropW, uint32_t cropH,
                               uint8_t *out, uint32_t dispW, uint32_t dispH,
                               size_t outSize) {
    if (!y || !uv || !out || srcW == 0 || srcH == 0 || dispW == 0 || dispH == 0 ||
        stride < srcW || cropW == 0 || cropH == 0)
        return false;
    // cropX/cropY must be even for NV12 chroma alignment.
    if ((cropX & 1) || (cropY & 1)) return false;
    if (cropX + cropW > srcW || cropY + cropH > srcH) return false;
    size_t needY = 0;
    size_t needUv = 0;
    if (!checkedMul(static_cast<size_t>(stride), srcH, needY)) return false;
    if (!checkedMul(static_cast<size_t>(stride), (srcH + 1) / 2, needUv)) return false;
    if (ySize < needY || uvSize < needUv) return false;
    size_t requiredOut = 0;
    if (!checkedMul(static_cast<size_t>(dispW), dispH, requiredOut) ||
        !checkedMul(requiredOut, 2, requiredOut)) return false;
    if (outSize < requiredOut) return false;

    for (uint32_t dy = 0; dy < dispH; ++dy) {
        uint32_t sy = cropY + static_cast<uint32_t>(
            (static_cast<uint64_t>(dy) * cropH) / dispH);
        if (sy >= srcH) sy = srcH - 1;
        for (uint32_t dx = 0; dx < dispW; ++dx) {
            uint32_t sx = cropX + static_cast<uint32_t>(
                (static_cast<uint64_t>(dx) * cropW) / dispW);
            if (sx >= srcW) sx = srcW - 1;
            uint16_t pixel = nv12ToRgb565Pixel(y, uv, stride, sx, sy);
            storeRgb565Be(out, pixel, (static_cast<size_t>(dy) * dispW + dx) * 2);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Downscale NV12 by an integer factor (2 or 4) using block averaging.
// Y plane: average factor x factor blocks. UV plane: average factor x factor
// blocks (each UV sample covers a 2x2 Y block, so the UV block is
// factor/2 x factor/2 in UV samples — but we just average the raw UV bytes
// the same way as Y, treating the interleaved U,V pairs).
// Returns packed NV12 (Y then UV, stride == outW).
// ---------------------------------------------------------------------------
std::vector<uint8_t> downscaleNv12(const uint8_t *y, const uint8_t *uv,
                                   uint32_t srcW, uint32_t srcH, uint32_t stride,
                                   size_t ySize, size_t uvSize,
                                   int factor) {
    if (!y || !uv || srcW == 0 || srcH == 0 || stride < srcW) return {};
    if (factor != 2 && factor != 4) return {};
    size_t needY = 0;
    size_t needUv = 0;
    if (!checkedMul(static_cast<size_t>(stride), srcH, needY)) return {};
    if (!checkedMul(static_cast<size_t>(stride), (srcH + 1) / 2, needUv)) return {};
    if (ySize < needY || uvSize < needUv) return {};

    // Round down to even for NV12 chroma alignment.
    uint32_t outW = (srcW / factor) & ~1u;
    uint32_t outH = (srcH / factor) & ~1u;
    if (outW == 0 || outH == 0) return {};

    size_t outYSize = 0;
    size_t outUvSize = 0;
    if (!checkedMul(static_cast<size_t>(outW), outH, outYSize)) return {};
    if (!checkedMul(static_cast<size_t>(outW), outH / 2, outUvSize)) return {};
    std::vector<uint8_t> result(outYSize + outUvSize);
    uint8_t *outY = result.data();
    uint8_t *outUv = result.data() + outYSize;

    // Downscale Y plane: average factor x factor blocks.
    for (uint32_t oy = 0; oy < outH; ++oy) {
        for (uint32_t ox = 0; ox < outW; ++ox) {
            unsigned sum = 0;
            for (int fy = 0; fy < factor; ++fy) {
                uint32_t sy = oy * factor + fy;
                if (sy >= srcH) break;
                for (int fx = 0; fx < factor; ++fx) {
                    uint32_t sx = ox * factor + fx;
                    if (sx >= srcW) break;
                    sum += y[static_cast<size_t>(sy) * stride + sx];
                }
            }
            outY[static_cast<size_t>(oy) * outW + ox] =
                static_cast<uint8_t>((sum + factor * factor / 2) / (factor * factor));
        }
    }

    // UV plane: NV12 UV is interleaved with stride == srcW bytes per row.
    // Average factor x factor blocks of UV bytes (same spatial block as Y).
    for (uint32_t oy = 0; oy < outH / 2; ++oy) {
        for (uint32_t ox = 0; ox < outW; ++ox) {
            unsigned sum = 0;
            for (int fy = 0; fy < factor; ++fy) {
                uint32_t sy = oy * factor + fy;
                if (sy >= srcH / 2) break;
                for (int fx = 0; fx < factor; ++fx) {
                    uint32_t sx = ox * factor + fx;
                    if (sx >= srcW) break;
                    sum += uv[static_cast<size_t>(sy) * stride + sx];
                }
            }
            outUv[static_cast<size_t>(oy) * outW + ox] =
                static_cast<uint8_t>((sum + factor * factor / 2) / (factor * factor));
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Crop an NV12 frame to (cropX, cropY, cropW, cropH).
// cropX/cropY must be even. Returns packed NV12 (Y then UV, stride == cropW).
// ---------------------------------------------------------------------------
std::vector<uint8_t> cropNv12(const uint8_t *y, const uint8_t *uv,
                              uint32_t srcW, uint32_t srcH, uint32_t stride,
                              size_t ySize, size_t uvSize,
                              uint32_t cropX, uint32_t cropY,
                              uint32_t cropW, uint32_t cropH) {
    if (!y || !uv || srcW == 0 || srcH == 0 || stride < srcW) return {};
    if (cropW == 0 || cropH == 0) return {};
    if ((cropX & 1) || (cropY & 1)) return {};
    if ((cropW & 1) || (cropH & 1)) return {};
    if (cropX + cropW > srcW || cropY + cropH > srcH) return {};
    size_t needY = 0;
    size_t needUv = 0;
    if (!checkedMul(static_cast<size_t>(stride), srcH, needY)) return {};
    if (!checkedMul(static_cast<size_t>(stride), (srcH + 1) / 2, needUv)) return {};
    if (ySize < needY || uvSize < needUv) return {};

    size_t outYSize = 0;
    size_t outUvSize = 0;
    if (!checkedMul(static_cast<size_t>(cropW), cropH, outYSize)) return {};
    if (!checkedMul(static_cast<size_t>(cropW), cropH / 2, outUvSize)) return {};
    std::vector<uint8_t> result(outYSize + outUvSize);
    uint8_t *outY = result.data();
    uint8_t *outUv = result.data() + outYSize;

    for (uint32_t r = 0; r < cropH; ++r) {
        const uint8_t *srcRow = y + static_cast<size_t>(cropY + r) * stride + cropX;
        std::memcpy(outY + static_cast<size_t>(r) * cropW, srcRow, cropW);
    }
    // UV rows = cropH/2, each row = cropW bytes.
    for (uint32_t r = 0; r < cropH / 2; ++r) {
        const uint8_t *srcRow = uv + static_cast<size_t>(cropY / 2 + r) * stride + cropX;
        std::memcpy(outUv + static_cast<size_t>(r) * cropW, srcRow, cropW);
    }

    return result;
}

} // namespace picamera
