#include "test_runner.h"
#include "cli.h"

#include <cstring>
#include <string>
#include <vector>

using namespace picamera;

namespace {

// Helper: build a (char**) argv from a vector of strings, run parseArgs.
// Returns false if parseArgs rejected it.
bool runParse(const std::vector<std::string> &args, CliOptions &opts,
              CameraConfig &cfg) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (auto &s : args) argv.push_back(const_cast<char *>(s.c_str()));
    return parseArgs(static_cast<int>(argv.size()), argv.data(), opts, cfg);
}

TEST(cli_capture_mode) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "out.png"}, opts, cfg));
    CHECK_EQ(opts.mode, std::string("capture"));
    CHECK_EQ(opts.captureFile, std::string("out.png"));
}

TEST(cli_list_controls_mode) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--list-controls"}, opts, cfg));
    CHECK_EQ(opts.mode, std::string("list-controls"));
}

TEST(cli_timelapse_mode) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--timelapse", "60", "--count", "10"}, opts, cfg));
    CHECK_EQ(opts.mode, std::string("timelapse"));
    CHECK_EQ(opts.timelapseInterval, 60);
    CHECK_EQ(opts.timelapseCount, 10);
}

TEST(cli_format_png) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--format", "png"}, opts, cfg));
    CHECK(cfg.format == OutputFormat::PNG);
}

TEST(cli_format_raw) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--format", "raw"}, opts, cfg));
    CHECK(cfg.format == OutputFormat::RAW_NV12);
}

TEST(cli_format_jpeg_accepted) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--format", "jpeg"}, opts, cfg));
    CHECK(cfg.format == OutputFormat::JPEG);
    // "jpg" alias should also work.
    CHECK(runParse({"picamera", "--capture", "x", "--format", "jpg"}, opts, cfg));
    CHECK(cfg.format == OutputFormat::JPEG);
}

TEST(cli_format_invalid_rejected) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(!runParse({"picamera", "--capture", "x", "--format", "tiff"}, opts, cfg));
}

TEST(cli_png_level_validated) {
    CliOptions opts;
    CameraConfig cfg;
    // Valid levels 0-9.
    CHECK(runParse({"picamera", "--capture", "x", "--format", "png", "--png-level", "1"}, opts, cfg));
    CHECK_EQ(cfg.pngLevel, 1);
    CHECK(runParse({"picamera", "--capture", "x", "--format", "png", "--png-level", "9"}, opts, cfg));
    CHECK_EQ(cfg.pngLevel, 9);
    // Out of range.
    CHECK(!runParse({"picamera", "--capture", "x", "--format", "png", "--png-level", "10"}, opts, cfg));
    CHECK(!runParse({"picamera", "--capture", "x", "--format", "png", "--png-level", "-1"}, opts, cfg));
}

TEST(cli_unknown_flag_rejected) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(!runParse({"picamera", "--capture", "x", "--bogus"}, opts, cfg));
}

TEST(cli_no_mode_rejected) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(!runParse({"picamera", "--width", "1920"}, opts, cfg));
}

TEST(cli_width_height_parsed) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--width", "1920", "--height", "1080"},
                   opts, cfg));
    CHECK_EQ(cfg.width, 1920u);
    CHECK_EQ(cfg.height, 1080u);
}

TEST(cli_shutter_and_iso_parsed) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--shutter", "30000", "--iso", "2.0"},
                   opts, cfg));
    CHECK_EQ(cfg.exposureTime, 30000ull);
    // 2.0f exactly representable
    CHECK(cfg.analogueGain > 1.99f && cfg.analogueGain < 2.01f);
}

TEST(cli_digital_gain_parsed) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--digital-gain", "1.5"}, opts, cfg));
    CHECK(cfg.digitalGain > 1.49f && cfg.digitalGain < 1.51f);
}

TEST(cli_ae_disable_flag) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--ae-disable"}, opts, cfg));
    CHECK(!cfg.aeEnable);
}

TEST(cli_awb_disable_flag) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--awb-disable"}, opts, cfg));
    CHECK(!cfg.awbEnable);
}

TEST(cli_awb_mode_parsed) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--awb", "daylight"}, opts, cfg));
    CHECK_EQ(cfg.awbMode, std::string("daylight"));
}

TEST(cli_warmup_parsed) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--warmup", "15"}, opts, cfg));
    CHECK_EQ(cfg.warmupFrames, 15u);
}

TEST(cli_bad_integer_rejected) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(!runParse({"picamera", "--capture", "x", "--width", "notanumber"}, opts, cfg));
}

TEST(cli_missing_value_rejected) {
    CliOptions opts;
    CameraConfig cfg;
    // --width with no following value
    CHECK(!runParse({"picamera", "--capture", "x", "--width"}, opts, cfg));
}

} // namespace
