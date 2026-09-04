#include "test_runner.h"
#include "safe_path.h"

#include <string>
#include <unistd.h>
#include <fcntl.h>

using namespace picamera;

namespace {

TEST(safe_path_component_rejects_traversal) {
    CHECK(!isSafePathComponent(".."));
    CHECK(!isSafePathComponent("."));
    CHECK(!isSafePathComponent("../etc"));
    CHECK(!isSafePathComponent("foo/../bar"));
    CHECK(!isSafePathComponent("foo/.."));
}

TEST(safe_path_component_rejects_absolute) {
    CHECK(!isSafePathComponent("/etc/passwd"));
    CHECK(!isSafePathComponent("/tmp"));
}

TEST(safe_path_component_rejects_control_chars) {
    // Use octal escapes (\NNN) instead of hex (\xNN) because hex escapes
    // are greedy — "foo\x01bar" would be parsed as \x01ba + r.
    CHECK(!isSafePathComponent("foo\001bar"));
    CHECK(!isSafePathComponent("foo\177"));
    CHECK(!isSafePathComponent("foo\nbar"));
}

TEST(safe_path_component_rejects_leading_dash) {
    CHECK(!isSafePathComponent("-flag"));
    CHECK(!isSafePathComponent("--help"));
}

TEST(safe_path_component_rejects_empty) {
    CHECK(!isSafePathComponent(""));
}

TEST(safe_path_component_accepts_valid) {
    CHECK(isSafePathComponent("capture"));
    CHECK(isSafePathComponent("shot"));
    CHECK(isSafePathComponent("img_001"));
    CHECK(isSafePathComponent("my-photos"));
    CHECK(isSafePathComponent("20240101"));
}

TEST(safe_capture_path_builds_correctly) {
    std::string path = safeCapturePath("/home/pi/captures", "shot",
                                       "20240101-120000", "jpg");
    CHECK_EQ(path, std::string("/home/pi/captures/shot_20240101-120000.jpg"));
}

TEST(safe_capture_path_no_trailing_slash) {
    std::string path = safeCapturePath("/home/pi/captures/", "shot",
                                       "20240101-120000", "jpg");
    CHECK_EQ(path, std::string("/home/pi/captures/shot_20240101-120000.jpg"));
}

TEST(safe_capture_path_rejects_bad_prefix) {
    CHECK(safeCapturePath("/tmp", "..", "20240101", "jpg").empty());
    CHECK(safeCapturePath("/tmp", "/etc", "20240101", "jpg").empty());
    CHECK(safeCapturePath("/tmp", "", "20240101", "jpg").empty());
}

TEST(safe_capture_path_rejects_bad_timestamp) {
    CHECK(safeCapturePath("/tmp", "shot", "../etc", "jpg").empty());
    CHECK(safeCapturePath("/tmp", "shot", "foo/bar", "jpg").empty());
}

TEST(safe_capture_path_rejects_empty_root) {
    CHECK(safeCapturePath("", "shot", "20240101", "jpg").empty());
}

TEST(safe_capture_path_rejects_traversal_in_root) {
    // rootDir with ".." should be rejected (new validation)
    CHECK(safeCapturePath("/home/pi/../etc", "shot", "20240101", "jpg").empty());
    CHECK(safeCapturePath("../captures", "shot", "20240101", "jpg").empty());
    CHECK(safeCapturePath("/tmp/../../etc", "shot", "20240101", "jpg").empty());
}

TEST(safe_capture_path_rejects_control_chars_in_root) {
    // rootDir with control characters should be rejected
    CHECK(safeCapturePath("/tmp/foo\nbar", "shot", "20240101", "jpg").empty());
    CHECK(safeCapturePath("/tmp/foo\001bar", "shot", "20240101", "jpg").empty());
}

TEST(safe_capture_path_accepts_nested_root) {
    // Valid nested directory paths should be accepted
    std::string path = safeCapturePath("/home/pi/captures/sub", "shot",
                                       "20240101-120000", "jpg");
    CHECK_EQ(path, std::string("/home/pi/captures/sub/shot_20240101-120000.jpg"));
}

TEST(safe_device_path_accepts_valid) {
    CHECK(isSafeDevicePath("/dev/spidev0.0"));
    CHECK(isSafeDevicePath("/dev/i2c-1"));
    CHECK(isSafeDevicePath("/dev/gpiochip0"));
}

TEST(safe_device_path_rejects_non_dev) {
    CHECK(!isSafeDevicePath("/etc/passwd"));
    CHECK(!isSafeDevicePath("/tmp/spidev0.0"));
    CHECK(!isSafeDevicePath("spidev0.0"));
}

TEST(safe_device_path_rejects_traversal) {
    CHECK(!isSafeDevicePath("/dev/../etc/passwd"));
    CHECK(!isSafeDevicePath("/dev/../../etc/shadow"));
}

TEST(safe_device_path_rejects_del_char) {
    // 0x7F (DEL) should be rejected, consistent with isSafePathComponent.
    std::string path = "/dev/spidev0.0";
    path.push_back('\x7F');
    CHECK(!isSafeDevicePath(path));
}

TEST(safe_device_path_rejects_control_chars) {
    // Use \x01 followed by a non-hex char to avoid the C++ hex escape
    // consuming following hex digits (e.g. \x01de would be one char 0x1DE).
    std::string path = "/dev/spi\x01""dev0.0";
    CHECK(!isSafeDevicePath(path));
}

TEST(safe_file_path_rejects_double_slash) {
    // Empty path components from "//" sequences should be rejected.
    CHECK(!isSafeFilePath("/home//pi/captures/photo.jpg"));
    CHECK(!isSafeFilePath("captures//photo.jpg"));
    // A trailing slash is rejected — file paths must not end with "/".
    CHECK(!isSafeFilePath("captures/photo.jpg/"));
}

TEST(checked_mul_no_overflow) {
    size_t result = 0;
    CHECK(checkedMul(100, 200, result));
    CHECK_EQ(result, static_cast<size_t>(20000));
}

TEST(checked_mul_zero) {
    size_t result = 999;
    CHECK(checkedMul(0, 12345, result));
    CHECK_EQ(result, static_cast<size_t>(0));
    CHECK(checkedMul(12345, 0, result));
    CHECK_EQ(result, static_cast<size_t>(0));
}

TEST(checked_mul_overflow_detected) {
    size_t result = 0;
    CHECK(!checkedMul(SIZE_MAX, 2, result));
    CHECK(!checkedMul(SIZE_MAX / 2 + 1, 2, result));
}

TEST(checked_add_no_overflow) {
    size_t result = 0;
    CHECK(checkedAdd(100, 200, result));
    CHECK_EQ(result, static_cast<size_t>(300));
}

TEST(checked_add_overflow_detected) {
    size_t result = 0;
    CHECK(!checkedAdd(SIZE_MAX, 1, result));
    CHECK(!checkedAdd(SIZE_MAX - 10, 20, result));
}

TEST(is_path_inside_valid) {
    CHECK(isPathInside("/home/pi/captures/shot.jpg", "/home/pi/captures"));
    CHECK(isPathInside("/home/pi/captures/sub/shot.jpg", "/home/pi/captures"));
}

TEST(is_path_inside_rejects_outside) {
    CHECK(!isPathInside("/etc/passwd", "/home/pi/captures"));
    CHECK(!isPathInside("/home/pi/captures", "/home/pi/captures"));
}

TEST(is_path_inside_rejects_traversal) {
    CHECK(!isPathInside("/home/pi/captures/../etc/passwd", "/home/pi/captures"));
}

TEST(safe_file_path_accepts_simple) {
    CHECK(isSafeFilePath("photo.png"));
    CHECK(isSafeFilePath("capture_0001.jpg"));
    CHECK(isSafeFilePath("subdir/photo.png"));
    CHECK(isSafeFilePath("a/b/c/test.ppm"));
}

TEST(safe_file_path_rejects_traversal) {
    CHECK(!isSafeFilePath("../etc/passwd"));
    CHECK(!isSafeFilePath("foo/../bar"));
    CHECK(!isSafeFilePath("a/../../b"));
    CHECK(!isSafeFilePath(".."));
}

TEST(safe_file_path_rejects_absolute) {
    CHECK(!isSafeFilePath("/etc/passwd"));
    CHECK(!isSafeFilePath("/tmp/x.ppm"));
}

TEST(safe_file_path_rejects_control_chars) {
    CHECK(!isSafeFilePath("foo\001bar.png"));
    CHECK(!isSafeFilePath("foo\nbar.png"));
}

TEST(safe_file_path_rejects_empty) {
    CHECK(!isSafeFilePath(""));
}

TEST(canonicalize_dir_resolves_dotdot) {
    // "../foo" relative to CWD should resolve to a path without ".."
    std::string r = canonicalizeDir("/tmp/../etc");
    CHECK(!r.empty());
    CHECK(r.find("..") == std::string::npos);
    CHECK_EQ(r, std::string("/etc"));
}

TEST(canonicalize_dir_strips_trailing_slash) {
    std::string r = canonicalizeDir("/tmp/");
    CHECK(!r.empty());
    CHECK(r.back() != '/');
}

TEST(canonicalize_dir_rejects_empty) {
    CHECK(canonicalizeDir("").empty());
}

TEST(canonicalize_dir_rejects_control_chars) {
    CHECK(canonicalizeDir("/tmp/foo\nbar").empty());
    CHECK(canonicalizeDir("/tmp/foo\001bar").empty());
}

TEST(canonicalize_dir_accepts_simple_absolute) {
    std::string r = canonicalizeDir("/tmp/captures");
    CHECK(!r.empty());
    CHECK(r.find("..") == std::string::npos);
}

TEST(is_canonical_path_inside_valid) {
    CHECK(isCanonicalPathInside("/home/pi/captures/shot.jpg", "/home/pi/captures"));
    CHECK(isCanonicalPathInside("/home/pi/captures/sub/shot.jpg", "/home/pi/captures"));
}

TEST(is_canonical_path_inside_rejects_outside) {
    CHECK(!isCanonicalPathInside("/etc/passwd", "/home/pi/captures"));
    CHECK(!isCanonicalPathInside("/home/pi/captures", "/home/pi/captures"));
}

TEST(is_canonical_path_inside_rejects_traversal) {
    CHECK(!isCanonicalPathInside("/home/pi/captures/../etc/passwd", "/home/pi/captures"));
}

TEST(is_canonical_path_inside_rejects_non_absolute) {
    CHECK(!isCanonicalPathInside("relative/path", "/home/pi/captures"));
    CHECK(!isCanonicalPathInside("/home/pi/captures/file", "relative"));
}

} // namespace
