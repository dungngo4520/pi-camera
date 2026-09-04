#include "test_runner.h"
#include "preview_helpers.h"
#include "camera_config.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace picamera;

namespace {

// --- makeCaptureFilename tests ---

TEST(make_capture_filename_has_prefix) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::JPEG);
    // The filename should contain the prefix followed by an underscore.
    auto pos = fn.find("shot_");
    CHECK(pos != std::string::npos);
}

TEST(make_capture_filename_has_extension) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::PNG);
    CHECK(fn.size() >= 4);
    CHECK(fn.substr(fn.size() - 4) == ".png");
}

TEST(make_capture_filename_jpeg_extension) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::JPEG);
    CHECK(fn.size() >= 4);
    CHECK(fn.substr(fn.size() - 4) == ".jpg");
}

TEST(make_capture_filename_dng_extension) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::DNG);
    CHECK(fn.size() >= 4);
    CHECK(fn.substr(fn.size() - 4) == ".dng");
}

TEST(make_capture_filename_ppm_extension) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::PPM);
    CHECK(fn.size() >= 4);
    CHECK(fn.substr(fn.size() - 4) == ".ppm");
}

TEST(make_capture_filename_raw_extension) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::RAW_NV12);
    CHECK(fn.size() >= 4);
    CHECK(fn.substr(fn.size() - 4) == ".raw");
}

TEST(make_capture_filename_has_timestamp) {
    std::string fn = makeCaptureFilename("/tmp", "shot", OutputFormat::JPEG);
    // Timestamp format: YYYYMMDD-HHMMSS-mmm (at least 8+1+6+1+3 = 19 chars)
    // The prefix "shot_" is 5 chars, plus timestamp, plus ".jpg" = 4.
    // So minimum length is 5 + 19 + 4 = 28.
    CHECK(fn.size() >= 28);
    // Verify the timestamp portion looks like a date: starts with 4 digits.
    auto underscore = fn.find("shot_");
    CHECK(underscore != std::string::npos);
    std::string ts = fn.substr(underscore + 5, fn.size() - underscore - 5 - 4);
    CHECK(ts.size() >= 19);
    // First 4 chars should be digits (year).
    for (int i = 0; i < 4; ++i) {
        CHECK(ts[i] >= '0' && ts[i] <= '9');
    }
}

TEST(make_capture_filename_includes_dir) {
    std::string fn = makeCaptureFilename("/home/pi/captures", "img", OutputFormat::JPEG);
    CHECK(fn.find("/home/pi/captures/") == 0);
}

TEST(make_capture_filename_rejects_bad_prefix) {
    // safeCapturePath rejects unsafe prefixes — makeCaptureFilename delegates to it.
    CHECK(makeCaptureFilename("/tmp", "..", OutputFormat::JPEG).empty());
    CHECK(makeCaptureFilename("/tmp", "/etc", OutputFormat::JPEG).empty());
    CHECK(makeCaptureFilename("/tmp", "", OutputFormat::JPEG).empty());
}

TEST(make_capture_filename_rejects_bad_dir) {
    CHECK(makeCaptureFilename("", "shot", OutputFormat::JPEG).empty());
    CHECK(makeCaptureFilename("../etc", "shot", OutputFormat::JPEG).empty());
}

TEST(make_capture_filename_two_calls_differ) {
    // Two calls should produce different filenames (millisecond precision).
    std::string fn1 = makeCaptureFilename("/tmp", "shot", OutputFormat::JPEG);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::string fn2 = makeCaptureFilename("/tmp", "shot", OutputFormat::JPEG);
    CHECK(fn1 != fn2);
}

// --- hasDiskSpace tests -----

TEST(has_disk_space_true_for_tmp) {
    // /tmp should have free space on any dev machine.
    CHECK(hasDiskSpace("/tmp"));
}

TEST(has_disk_space_false_for_nonexistent_dir) {
    // A nonexistent directory should fail closed (statvfs returns error).
    CHECK(!hasDiskSpace("/nonexistent/path/that/does/not/exist/12345"));
}

TEST(has_disk_space_false_for_empty_path) {
    CHECK(!hasDiskSpace(""));
}

// --- listCaptures tests ---

// Helper: create a unique temp directory for testing listCaptures.
std::string makeTempDir() {
    static unsigned counter = 0;
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("picamera_test_list_" + std::to_string(getpid()) +
                       "_" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir.string();
}

TEST(list_captures_empty_dir) {
    std::string dir = makeTempDir();
    auto files = listCaptures(dir);
    CHECK(files.empty());
    std::filesystem::remove_all(dir);
}

TEST(list_captures_nonexistent_dir_returns_empty) {
    auto files = listCaptures("/nonexistent/dir/12345");
    CHECK(files.empty());
}

TEST(list_captures_filters_by_extension) {
    std::string dir = makeTempDir();
    // Create files with various extensions.
    std::ofstream(dir + "/img.jpg") << "x";
    std::ofstream(dir + "/img.png") << "x";
    std::ofstream(dir + "/img.dng") << "x";
    std::ofstream(dir + "/img.ppm") << "x";
    std::ofstream(dir + "/img.jpeg") << "x";
    // Non-image extensions should be excluded.
    std::ofstream(dir + "/notes.txt") << "x";
    std::ofstream(dir + "/data.bin") << "x";

    auto files = listCaptures(dir);
    CHECK_EQ(files.size(), static_cast<size_t>(5));
    std::filesystem::remove_all(dir);
}

TEST(list_captures_excludes_non_regular_files) {
    std::string dir = makeTempDir();
    std::ofstream(dir + "/img.jpg") << "x";
    // Create a subdirectory named like an image — should be excluded.
    std::filesystem::create_directory(dir + "/sub.jpg");

    auto files = listCaptures(dir);
    CHECK_EQ(files.size(), static_cast<size_t>(1));
    std::filesystem::remove_all(dir);
}

TEST(list_captures_sorted_newest_first) {
    std::string dir = makeTempDir();
    // Create files with slight time gaps so modification times differ.
    std::ofstream(dir + "/old.jpg") << "x";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::ofstream(dir + "/new.jpg") << "x";

    auto files = listCaptures(dir);
    REQUIRE(files.size() == 2);
    // Newest first — "new.jpg" should be before "old.jpg".
    CHECK(files[0].find("new.jpg") != std::string::npos);
    CHECK(files[1].find("old.jpg") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(list_captures_returns_full_paths) {
    std::string dir = makeTempDir();
    std::ofstream(dir + "/img.jpg") << "x";

    auto files = listCaptures(dir);
    REQUIRE(files.size() == 1);
    // The returned path should contain the directory prefix.
    CHECK(files[0].find(dir) != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST(list_captures_case_insensitive_extension) {
    std::string dir = makeTempDir();
    std::ofstream(dir + "/img.JPG") << "x";
    std::ofstream(dir + "/img.Png") << "x";

    auto files = listCaptures(dir);
    CHECK_EQ(files.size(), static_cast<size_t>(2));
    std::filesystem::remove_all(dir);
}

} // namespace
