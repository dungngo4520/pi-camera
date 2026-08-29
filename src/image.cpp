#include "image.h"

#include <thread>
#include <vector>
#include <algorithm>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define PICAMERA_HAS_NEON 1
#endif

namespace picamera {

namespace {

inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
}

// ---------------------------------------------------------------------------
// Scalar reference implementation — the tested path on x86 and the fallback
// for odd-sized remainders on NEON builds.
// ---------------------------------------------------------------------------
void nv12ToRgbRowPair_scalar(const uint8_t *yRow0, const uint8_t *yRow1,
                              const uint8_t *uvRow, uint8_t *rgb0, uint8_t *rgb1,
                              uint32_t w, bool haveRow1) {
    for (uint32_t x = 0; x < w; x += 2) {
        int U = uvRow[x];
        int V = uvRow[x + 1];
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
    uint8x8_t r8 = vmovn_u16(vcombine_u16(r16_lo, r16_hi));

    // G = (Y*298 - U*100 - V*208 + 128) >> 8
    int32x4_t g_lo = vmlsl_n_s16(y298_lo, u_lo, 100);
    g_lo = vmlsl_n_s16(g_lo, v_lo, 208);
    g_lo = vaddq_s32(g_lo, vdupq_n_s32(128));
    int32x4_t g_hi = vmlsl_n_s16(y298_hi, u_hi, 100);
    g_hi = vmlsl_n_s16(g_hi, v_hi, 208);
    g_hi = vaddq_s32(g_hi, vdupq_n_s32(128));
    uint16x4_t g16_lo = vqshrun_n_s32(g_lo, 8);
    uint16x4_t g16_hi = vqshrun_n_s32(g_hi, 8);
    uint8x8_t g8 = vmovn_u16(vcombine_u16(g16_lo, g16_hi));

    // B = (Y*298 + U*516 + 128) >> 8
    int32x4_t b_lo = vaddq_s32(vmlal_n_s16(y298_lo, u_lo, 516), vdupq_n_s32(128));
    int32x4_t b_hi = vaddq_s32(vmlal_n_s16(y298_hi, u_hi, 516), vdupq_n_s32(128));
    uint16x4_t b16_lo = vqshrun_n_s32(b_lo, 8);
    uint16x4_t b16_hi = vqshrun_n_s32(b_hi, 8);
    uint8x8_t b8 = vmovn_u16(vcombine_u16(b16_lo, b16_hi));

    // Interleave R, G, B into RGBRGB... and store 24 bytes (8 pixels).
    uint8x8x3_t rgb;
    rgb.val[0] = r8;
    rgb.val[1] = g8;
    rgb.val[2] = b8;
    vst3_u8(out, rgb);
}

static void nv12ToRgbRowPair_neon(const uint8_t *yRow0, const uint8_t *yRow1,
                                   const uint8_t *uvRow, uint8_t *rgb0,
                                   uint8_t *rgb1, uint32_t w, bool haveRow1) {
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

    // Process remaining 8-pixel block if present.
    if (x + 8 <= w) {
        uint8x8x2_t uv_pair = vld2_u8(&uvRow[x]);
        uint8x8_t u = uv_pair.val[0];
        uint8x8_t v = uv_pair.val[1];
        uint8x8x2_t u_dup = vzip_u8(u, u);
        uint8x8x2_t v_dup = vzip_u8(v, v);

        convert8pixels_neon(vld1_u8(&yRow0[x]),
                            u_dup.val[0], v_dup.val[0],
                            &rgb0[x * 3]);
        if (haveRow1) {
            convert8pixels_neon(vld1_u8(&yRow1[x]),
                                u_dup.val[0], v_dup.val[0],
                                &rgb1[x * 3]);
        }
        x += 8;
    }

    // Scalar fallback for the last <8 pixels (odd widths).
    if (x < w) {
        nv12ToRgbRowPair_scalar(yRow0 + x, yRow1 + x, uvRow + x,
                                rgb0 + x * 3, rgb1 + x * 3,
                                w - x, haveRow1);
    }
}
#endif // PICAMERA_HAS_NEON

// Dispatch to NEON if available, else scalar. Processes one 2-row pair.
void nv12ToRgbRowPair(const uint8_t *yRow0, const uint8_t *yRow1,
                       const uint8_t *uvRow, uint8_t *rgb0, uint8_t *rgb1,
                       uint32_t w, bool haveRow1) {
#ifdef PICAMERA_HAS_NEON
    nv12ToRgbRowPair_neon(yRow0, yRow1, uvRow, rgb0, rgb1, w, haveRow1);
#else
    nv12ToRgbRowPair_scalar(yRow0, yRow1, uvRow, rgb0, rgb1, w, haveRow1);
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
                                uint32_t w, uint32_t h, uint32_t stride) {
    size_t rgbSize = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> rgb(rgbSize);

    const unsigned nThreads = std::min<unsigned>(
        std::thread::hardware_concurrency(),
        (h + 1) / 2);  // one thread per 2-row pair, capped

    if (nThreads <= 1 || h < 8) {
        // Single-threaded path.
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
                w, haveRow1);
        }
        return rgb;
    }

    // Multi-threaded: divide row pairs among threads.
    const uint32_t rowPairs = (h + 1) / 2;
    const uint32_t pairsPerThread = (rowPairs + nThreads - 1) / nThreads;

    std::vector<std::thread> threads;
    threads.reserve(nThreads);

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
                    w, haveRow1);
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
void nv12ToRgb565Scaled(const uint8_t *y, const uint8_t *uv,
                        uint32_t srcW, uint32_t srcH, uint32_t stride,
                        uint8_t *out, uint32_t dispW, uint32_t dispH) {
    // Compute center-crop region to match display aspect ratio
    float srcAspect = static_cast<float>(srcW) / srcH;
    float dispAspect = static_cast<float>(dispW) / dispH;

    uint32_t cropW;
    uint32_t cropH;
    uint32_t cropX;
    uint32_t cropY;
    if (srcAspect > dispAspect) {
        // Source is wider — crop horizontally
        cropH = srcH;
        cropW = static_cast<uint32_t>(srcH * dispAspect);
        cropX = (srcW - cropW) / 2;
        cropY = 0;
    } else {
        // Source is taller — crop vertically
        cropW = srcW;
        cropH = static_cast<uint32_t>(srcW / dispAspect);
        cropX = 0;
        cropY = (srcH - cropH) / 2;
    }
    cropH = std::min(cropH, srcH);
    cropW = std::min(cropW, srcW);

    for (uint32_t dy = 0; dy < dispH; ++dy) {
        // Map display row to source row (nearest-neighbor)
        uint32_t sy = cropY + (dy * cropH) / dispH;
        if (sy >= srcH) sy = srcH - 1;

        for (uint32_t dx = 0; dx < dispW; ++dx) {
            // Map display column to source column
            uint32_t sx = cropX + (dx * cropW) / dispW;
            if (sx >= srcW) sx = srcW - 1;

            // Get Y sample
            int Y = y[sy * stride + sx];

            // Get UV sample (NV12: UV is interleaved, subsampled 2x2)
            uint32_t uvX = (sx / 2) * 2;
            uint32_t uvY = sy / 2;
            int U = uv[uvY * stride + uvX] - 128;
            int V = uv[uvY * stride + uvX + 1] - 128;

            // BT.601 limited-range YUV -> RGB
            int C = Y - 16;
            int R = (298 * C + 409 * V + 128) >> 8;
            int G = (298 * C - 100 * U - 208 * V + 128) >> 8;
            int B = (298 * C + 516 * U + 128) >> 8;

            // Clamp to [0, 255]
            R = R < 0 ? 0 : R > 255 ? 255 : R;
            G = G < 0 ? 0 : G > 255 ? 255 : G;
            B = B < 0 ? 0 : B > 255 ? 255 : B;

            // Pack to RGB565: RRRRRGGG GGGBBBBB
            uint16_t pixel = static_cast<uint16_t>(
                ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3));

            // Store big-endian (high byte first for SPI)
            size_t outIdx = (static_cast<size_t>(dy) * dispW + dx) * 2;
            out[outIdx] = static_cast<uint8_t>(pixel >> 8);
            out[outIdx + 1] = static_cast<uint8_t>(pixel & 0xFF);
        }
    }
}

} // namespace picamera
