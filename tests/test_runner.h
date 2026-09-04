// Minimal test framework — no external dependencies.
//
// Usage:
//   TEST("name") { CHECK(...); CHECK_EQ(a, b); REQUIRE(...); }
//
// TEST() creates a function registered at static-init time; test_main.cpp
// runs them all. CHECK records a failure and continues; REQUIRE aborts the
// current test. Failed REQUIREs use a local exception to unwind back to the
// runner, so destructors run normally (unlike assert()).
#pragma once

#include <atomic>
#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace picamera::test {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestCase {
    const char *name;
    void (*fn)();
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> r;
    return r;
}

inline int &failCount() {
    static int n = 0;
    return n;
}

inline int checkFailures() { return failCount(); }

struct Registrar {
    Registrar(const char *name, void (*fn)()) {
        registry().push_back({name, fn});
    }
};

inline void reportCheck(const char *expr, const char *file, int line) {
    std::cerr << "  CHECK failed: " << expr << "  (" << file << ":" << line << ")\n";
    ++failCount();
}

inline void reportEq(const char *aexpr, const char *bexpr, const std::string &a,
                     const std::string &b, const char *file, int line) {
    std::cerr << "  CHECK_EQ failed: " << aexpr << " == " << bexpr << "\n"
              << "    lhs = " << a << "\n    rhs = " << b
              << "  (" << file << ":" << line << ")\n";
    ++failCount();
}

} // namespace picamera::test

#define TEST(name)                                                            \
    static void test_##name();                                                \
    static ::picamera::test::Registrar reg_##name(#name, &test_##name);      \
    static void test_##name()

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond))                                                          \
            ::picamera::test::reportCheck(#cond, __FILE__, __LINE__);         \
    } while (0)

#define REQUIRE(cond)                                                         \
    do {                                                                      \
        if (!(cond)) {                                                        \
            ::picamera::test::reportCheck(#cond, __FILE__, __LINE__);         \
            throw ::picamera::test::Failure(#cond);                          \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        auto _a = (a);                                                        \
        auto _b = (b);                                                        \
        if (!(_a == _b))                                                      \
            ::picamera::test::reportEq(#a, #b, ::picamera::test::toStr(_a),  \
                                       ::picamera::test::toStr(_b),          \
                                       __FILE__, __LINE__);                   \
    } while (0)

// toStr overloads for readable CHECK_EQ diagnostics.
namespace picamera::test {
inline std::string toStr(int v)             { return std::to_string(v); }
inline std::string toStr(long v)            { return std::to_string(v); }
inline std::string toStr(long long v)       { return std::to_string(v); }
inline std::string toStr(unsigned v)        { return std::to_string(v); }
inline std::string toStr(unsigned long v)   { return std::to_string(v); }
inline std::string toStr(unsigned long long v) { return std::to_string(v); }
inline std::string toStr(const char *v)     { return std::string(v ? v : "<null>"); }
inline std::string toStr(const std::string &v) { return "\"" + v + "\""; }
inline std::string toStr(std::string_view v) { return "\"" + std::string(v) + "\""; }
inline std::string toStr(bool v)            { return v ? "true" : "false"; }

// Create a unique temporary file path with the given suffix.
// The file is NOT created — only the path is returned. The caller owns
// cleanup (typically via unlink after use).
inline std::string tmpPath(const char *suffix) {
    char tmpl[] = "/tmp/picamera_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        // Fallback: use PID + a static counter for uniqueness if mkstemp fails.
        static std::atomic<unsigned> counter{0};
        return std::string("/tmp/picamera_test_") +
               std::to_string(getpid()) + "_" +
               std::to_string(counter.fetch_add(1)) + suffix;
    }
    close(fd);
    unlink(tmpl);  // we want the path, not the file
    return std::string(tmpl) + suffix;
}
} // namespace picamera::test
