#include "camera_config.h"
#include "cli.h"
#include "test_runner.h"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace picamera;

namespace {

// --- parseOutputFormat / extensionFor helpers ---

TEST(parse_output_format_known) {
  CHECK(parseOutputFormat("ppm") == OutputFormat::PPM);
  CHECK(parseOutputFormat("raw") == OutputFormat::RAW_NV12);
  CHECK(parseOutputFormat("png") == OutputFormat::PNG);
  CHECK(parseOutputFormat("jpeg") == OutputFormat::JPEG);
  CHECK(parseOutputFormat("jpg") == OutputFormat::JPEG); // alias
  CHECK(parseOutputFormat("dng") == OutputFormat::DNG);
}

TEST(parse_output_format_unknown_is_nullopt) {
  CHECK(!parseOutputFormat("tiff"));
  CHECK(!parseOutputFormat(""));
  CHECK(!parseOutputFormat("xyz"));
}

TEST(parse_output_format_case_insensitive) {
  CHECK(parseOutputFormat("JPEG") == OutputFormat::JPEG);
  CHECK(parseOutputFormat("Png") == OutputFormat::PNG);
  CHECK(parseOutputFormat("DNG") == OutputFormat::DNG);
  CHECK(parseOutputFormat("RAW") == OutputFormat::RAW_NV12);
  CHECK(parseOutputFormat("PpM") == OutputFormat::PPM);
}

TEST(extension_for_each_format) {
  CHECK(extensionFor(OutputFormat::PPM) == std::string_view("ppm"));
  CHECK(extensionFor(OutputFormat::RAW_NV12) == std::string_view("raw"));
  CHECK(extensionFor(OutputFormat::PNG) == std::string_view("png"));
  CHECK(extensionFor(OutputFormat::JPEG) == std::string_view("jpg"));
  CHECK(extensionFor(OutputFormat::DNG) == std::string_view("dng"));
}

// Helper: build a (char**) argv from a vector of strings, run parseArgs.
// Returns false if parseArgs rejected it.
bool runParse(const std::vector<std::string> &args, CliOptions &opts,
              CameraConfig &cfg) {
  std::vector<char *> argv;
  argv.reserve(args.size());
  for (const auto &s : args)
    argv.push_back(const_cast<char *>(s.c_str()));
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
  CHECK(
      runParse({"picamera", "--timelapse", "60", "--count", "10"}, opts, cfg));
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
  CHECK(
      runParse({"picamera", "--capture", "x", "--format", "jpeg"}, opts, cfg));
  CHECK(cfg.format == OutputFormat::JPEG);
  // "jpg" alias should also work.
  CHECK(runParse({"picamera", "--capture", "x", "--format", "jpg"}, opts, cfg));
  CHECK(cfg.format == OutputFormat::JPEG);
}

TEST(cli_format_dng_accepted) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--capture", "x.dng", "--format", "dng"}, opts,
                 cfg));
  CHECK(cfg.format == OutputFormat::DNG);
}

TEST(cli_format_invalid_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(
      !runParse({"picamera", "--capture", "x", "--format", "tiff"}, opts, cfg));
}

TEST(cli_png_level_validated) {
  CliOptions opts;
  CameraConfig cfg;
  // Valid levels 0-9.
  CHECK(runParse(
      {"picamera", "--capture", "x", "--format", "png", "--png-level", "1"},
      opts, cfg));
  CHECK_EQ(cfg.pngLevel, 1);
  CHECK(runParse(
      {"picamera", "--capture", "x", "--format", "png", "--png-level", "9"},
      opts, cfg));
  CHECK_EQ(cfg.pngLevel, 9);
  // Out of range.
  CHECK(!runParse(
      {"picamera", "--capture", "x", "--format", "png", "--png-level", "10"},
      opts, cfg));
  CHECK(!runParse(
      {"picamera", "--capture", "x", "--format", "png", "--png-level", "-1"},
      opts, cfg));
}

TEST(cli_jpeg_quality_validated) {
  CliOptions opts;
  CameraConfig cfg;
  // Valid quality 1-100.
  CHECK(runParse({"picamera", "--capture", "x", "--format", "jpeg",
                  "--jpeg-quality", "50"},
                 opts, cfg));
  CHECK_EQ(cfg.jpegQuality, 50);
  CHECK(runParse({"picamera", "--capture", "x", "--format", "jpeg",
                  "--jpeg-quality", "100"},
                 opts, cfg));
  CHECK_EQ(cfg.jpegQuality, 100);
  CHECK(runParse(
      {"picamera", "--capture", "x", "--format", "jpeg", "--jpeg-quality", "1"},
      opts, cfg));
  CHECK_EQ(cfg.jpegQuality, 1);
  // Out of range.
  CHECK(!runParse(
      {"picamera", "--capture", "x", "--format", "jpeg", "--jpeg-quality", "0"},
      opts, cfg));
  CHECK(!runParse({"picamera", "--capture", "x", "--format", "jpeg",
                   "--jpeg-quality", "101"},
                  opts, cfg));
  // Default is 90 when not specified (fresh cfg to avoid stale value).
  {
    CliOptions opts2;
    CameraConfig cfg2;
    CHECK(runParse({"picamera", "--capture", "x", "--format", "jpeg"}, opts2,
                   cfg2));
    CHECK_EQ(cfg2.jpegQuality, 90);
  }
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
  CHECK(runParse(
      {"picamera", "--capture", "x", "--width", "1920", "--height", "1080"},
      opts, cfg));
  CHECK_EQ(cfg.width, 1920u);
  CHECK_EQ(cfg.height, 1080u);
}

TEST(cli_shutter_and_iso_parsed) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse(
      {"picamera", "--capture", "x", "--shutter", "30000", "--iso", "2.0"},
      opts, cfg));
  CHECK_EQ(cfg.exposureTime, 30000ull);
  // 2.0f exactly representable
  CHECK(cfg.analogueGain > 1.99f && cfg.analogueGain < 2.01f);
}

TEST(cli_digital_gain_parsed) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--capture", "x", "--digital-gain", "1.5"}, opts,
                 cfg));
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
  CHECK(
      runParse({"picamera", "--capture", "x", "--awb", "daylight"}, opts, cfg));
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
  CHECK(!runParse({"picamera", "--capture", "x", "--width", "notanumber"}, opts,
                  cfg));
}

TEST(cli_missing_value_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  // --width with no following value
  CHECK(!runParse({"picamera", "--capture", "x", "--width"}, opts, cfg));
}

TEST(cli_bracket_parsed) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--capture", "x.png", "--bracket", "3,-2,0,+2"},
                 opts, cfg));
  REQUIRE(cfg.bracketEv.size() == 3);
  // Use a tolerance for float comparison
  CHECK(std::abs(cfg.bracketEv[0] - (-2.0f)) < 0.01f);
  CHECK(std::abs(cfg.bracketEv[1] - 0.0f) < 0.01f);
  CHECK(std::abs(cfg.bracketEv[2] - 2.0f) < 0.01f);
}

TEST(cli_bracket_count_mismatch_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  // Says 3 but only gives 2 EV values
  CHECK(!runParse({"picamera", "--capture", "x", "--bracket", "3,-2,0"}, opts,
                  cfg));
}

TEST(cli_bracket_no_comma_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--bracket", "3"}, opts, cfg));
}

TEST(cli_bracket_count_out_of_range_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--bracket", "10,0"}, opts,
                  cfg));
  CHECK(!runParse({"picamera", "--capture", "x", "--bracket", "0"}, opts, cfg));
}

TEST(cli_preview_mode_parsed) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview"}, opts, cfg));
  CHECK_EQ(opts.mode, std::string("preview"));
}

TEST(cli_preview_options_parsed) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(
      runParse({"picamera", "--preview", "--preview-w", "240", "--preview-h",
                "240", "--preview-fps", "10", "--spi-device", "/dev/spidev0.1",
                "--display-rotate", "90", "--capture-format", "png",
                "--capture-dir", "/tmp/captures", "--capture-prefix", "shot"},
               opts, cfg));
  CHECK_EQ(opts.mode, std::string("preview"));
  CHECK_EQ(opts.previewWidth, 240u);
  CHECK_EQ(opts.previewHeight, 240u);
  CHECK_EQ(opts.previewFps, 10u);
  CHECK_EQ(opts.spiDevice, std::string("/dev/spidev0.1"));
  CHECK_EQ(opts.displayRotation, 90);
  CHECK_EQ(opts.captureFormat, std::string("png"));
  CHECK_EQ(opts.captureDir, std::string("/tmp/captures"));
  CHECK_EQ(opts.capturePrefix, std::string("shot"));
}

TEST(cli_preview_fps_zero_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--preview", "--preview-fps", "0"}, opts, cfg));
}

TEST(cli_preview_fps_over_max_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(
      !runParse({"picamera", "--preview", "--preview-fps", "121"}, opts, cfg));
}

TEST(cli_width_wraparound_rejected) {
  // 4294967297 = 2^32 + 1, which would wrap to 1 via static_cast<uint32_t>
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x.ppm", "--width", "4294967297"},
                  opts, cfg));
}

TEST(cli_battery_addr_wraparound_rejected) {
  // 0x101 would wrap to 0x01 via static_cast<uint8_t>, bypassing range check
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse(
      {"picamera", "--preview", "--battery", "--battery-addr", "0x101"}, opts,
      cfg));
}

TEST(cli_battery_addr_accepts_0x_prefix) {
  // 0x48 should parse the same as 48 (hex 0x48 = decimal 72)
  CliOptions opts;
  CameraConfig cfg;
  CHECK(
      runParse({"picamera", "--preview", "--battery", "--battery-addr", "0x48"},
               opts, cfg));
  CHECK_EQ(opts.batteryI2cAddress, static_cast<uint8_t>(0x48));
}

TEST(cli_battery_addr_accepts_no_prefix) {
  // Without 0x prefix, still works (hex 48 = decimal 72)
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview", "--battery", "--battery-addr", "48"},
                 opts, cfg));
  CHECK_EQ(opts.batteryI2cAddress, static_cast<uint8_t>(0x48));
}

TEST(cli_wifi_flag_sets_enabled) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview", "--wifi"}, opts, cfg));
  CHECK(opts.wifiEnabled == true);
}

TEST(cli_wifi_flag_default_false) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview"}, opts, cfg));
  CHECK(opts.wifiEnabled == false);
}

TEST(cli_bt_flag_sets_enabled) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview", "--bt"}, opts, cfg));
  CHECK(opts.btEnabled == true);
}

TEST(cli_bt_flag_default_false) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--preview"}, opts, cfg));
  CHECK(opts.btEnabled == false);
}

TEST(cli_capture_traversal_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "../etc/passwd"}, opts, cfg));
}

TEST(cli_capture_absolute_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "/tmp/x.ppm"}, opts, cfg));
}

TEST(cli_output_traversal_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse(
      {"picamera", "--timelapse", "60", "--output", "../evil_%04d.ppm"}, opts,
      cfg));
}

TEST(cli_capture_dir_traversal_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--preview", "--capture-dir", "../evil"}, opts,
                  cfg));
}

TEST(cli_numeric_trailing_garbage_rejected) {
  // std::stoull/stoi/stof silently stop at the first invalid char,
  // accepting "4056abc" as 4056. Verify our parsers reject this.
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--width", "4056abc"}, opts, cfg));
  CHECK(!runParse({"picamera", "--height", "3040xyz"}, opts, cfg));
  CHECK(!runParse({"picamera", "--iso", "100foo"}, opts, cfg));
  CHECK(!runParse({"picamera", "--shutter", "30000xyz"}, opts, cfg));
  CHECK(!runParse({"picamera", "--timelapse", "60abc"}, opts, cfg));
  CHECK(!runParse({"picamera", "--count", "10xyz"}, opts, cfg));
  CHECK(!runParse({"picamera", "--preview-fps", "30abc"}, opts, cfg));
}

TEST(cli_numeric_valid_still_accepted) {
  // Ensure normal valid values still parse correctly after the
  // trailing-garbage check was added.
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse(
      {"picamera", "--preview", "--preview-w", "320", "--preview-h", "240"},
      opts, cfg));
  CHECK_EQ(opts.previewWidth, 320u);
  CHECK_EQ(opts.previewHeight, 240u);
}

TEST(cli_awb_unknown_mode_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--awb", "bogus"}, opts, cfg));
}

TEST(cli_awb_all_modes_accepted) {
  // All documented AWB modes should be accepted.
  for (const char *mode : {"auto", "daylight", "cloudy", "incandescent",
                           "tungsten", "fluorescent", "indoor"}) {
    CliOptions opts;
    CameraConfig cfg;
    CHECK(runParse({"picamera", "--capture", "x", "--awb", mode}, opts, cfg));
    CHECK_EQ(cfg.awbMode, std::string(mode));
  }
}

TEST(cli_iso_inf_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--iso", "inf"}, opts, cfg));
}

TEST(cli_iso_nan_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--iso", "nan"}, opts, cfg));
}

TEST(cli_digital_gain_inf_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--digital-gain", "infinity"},
                  opts, cfg));
}

TEST(cli_help_long_flag_rejected) {
  // --help prints usage and returns false (no mode to run).
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--help"}, opts, cfg));
}

TEST(cli_help_short_flag_rejected) {
  // -h prints usage and returns false (no mode to run).
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "-h"}, opts, cfg));
}

TEST(cli_output_in_capture_mode_rejected) {
  // --output is a timelapse-only flag; accepting it in --capture mode
  // would silently ignore the user-supplied pattern.
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--capture", "x", "--output", "photo_%04d.jpg"},
                  opts, cfg));
}

TEST(cli_output_in_preview_mode_rejected) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(!runParse({"picamera", "--preview", "--output", "photo_%04d.jpg"}, opts,
                  cfg));
}

TEST(cli_output_in_timelapse_accepted) {
  CliOptions opts;
  CameraConfig cfg;
  CHECK(runParse({"picamera", "--timelapse", "60", "--count", "5", "--output",
                  "shot_%04d.png"},
                 opts, cfg));
  CHECK_EQ(opts.outputPattern, std::string("shot_%04d.png"));
}

} // namespace
