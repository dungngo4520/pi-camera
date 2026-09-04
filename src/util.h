#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <system_error>

namespace picamera {

constexpr float kIsoScaleFactor = 100.0f;
constexpr uint64_t kMicrosPerMillis = 1000;
constexpr uint64_t kMicrosPerSec = 1000000;
constexpr uint64_t kMicrosPerMin = 60000000;
constexpr uint8_t kJpegSoi0 = 0xFF;
constexpr uint8_t kJpegSoi1 = 0xD8;
constexpr uint8_t kJpegEoi0 = 0xFF;
constexpr uint8_t kJpegEoi1 = 0xD9;

inline std::string errnoString(int err) {
    return std::system_category().message(err);
}

inline uint32_t analogueGainToIso(float gain) {
    float g = std::isfinite(gain) ? std::max(0.0f, gain) : 0.0f;
    float isoF = std::min(g * kIsoScaleFactor, static_cast<float>(UINT32_MAX - 1));
    long long iso = std::llround(isoF);
    return static_cast<uint32_t>(std::min<long long>(iso, UINT32_MAX - 1));
}

// RAII wrapper around std::malloc/std::free for setjmp-safe memory management.
// Used in the *caller* of setjmp/longjmp core functions (e.g. PNG/JPEG decode
// wrappers) where normal scope exit runs the destructor. Must NOT be in scope
// when longjmp could fire — longjmp skips destructors (C++ [support.runtime]/3).
class MallocGuard {
public:
    explicit MallocGuard(uint8_t *p) : p_(p) {}
    ~MallocGuard() { if (p_) std::free(p_); }
    MallocGuard(const MallocGuard &) = delete;
    MallocGuard &operator=(const MallocGuard &) = delete;
    uint8_t *get() const { return p_; }
    uint8_t *release() { uint8_t *t = p_; p_ = nullptr; return t; }
private:
    uint8_t *p_;
};

inline bool checkedMul(size_t a, size_t b, size_t &result) {
    if (a == 0 || b == 0) { result = 0; return true; }
    if (a > SIZE_MAX / b) return false;
    result = a * b;
    return true;
}

inline bool checkedAdd(size_t a, size_t b, size_t &result) {
    if (a > SIZE_MAX - b) return false;
    result = a + b;
    return true;
}

#if SIZE_MAX != UINT64_MAX
inline bool checkedMul(uint64_t a, uint64_t b, uint64_t &result) {
    if (a == 0 || b == 0) { result = 0; return true; }
    if (a > UINT64_MAX / b) return false;
    result = a * b;
    return true;
}

inline bool checkedAdd(uint64_t a, uint64_t b, uint64_t &result) {
    if (a > UINT64_MAX - b) return false;
    result = a + b;
    return true;
}
#endif

}
