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

#include <exception>
#include <iostream>
#include <string>
#include <vector>

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
inline std::string toStr(bool v)            { return v ? "true" : "false"; }
} // namespace picamera::test
