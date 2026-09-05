#include "camera_mode.h"
#include "test_runner.h"
#include "wifi_server.h"

#include <string>
#include <vector>

using picamera::AeConstraintMode;
using picamera::AeExposureMode;
using picamera::applySettingsJson;
using picamera::AspectRatio;
using picamera::BracketType;
using picamera::CameraSettings;
using picamera::DriveMode;
using picamera::ExposureMode;
using picamera::extractFileName;
using picamera::fileListingHtml;
using picamera::FileNamingMode;
using picamera::GridType;
using picamera::HttpRequest;
using picamera::ImageSize;
using picamera::isSafeFileName;
using picamera::MeteringMode;
using picamera::NoiseReductionMode;
using picamera::OutputFormat;
using picamera::parseHttpRequest;
using picamera::PictureStyle;
using picamera::SensorMode;
using picamera::settingsToJson;
using picamera::statusToJson;
using picamera::urlDecode;
using picamera::VideoCodec;
using picamera::VideoResolution;
using picamera::ZebraMode;

// --- parseHttpRequest ---

TEST(http_parse_get_root) {
  std::string raw = "GET / HTTP/1.1\r\nHost: pi\r\n\r\n";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "GET");
  CHECK(req.path == "/");
  CHECK(req.body.empty());
}

TEST(http_parse_get_file) {
  std::string raw = "GET /file/photo.jpg HTTP/1.1\r\nHost: pi\r\n\r\n";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "GET");
  CHECK(req.path == "/file/photo.jpg");
}

TEST(http_parse_get_status) {
  std::string raw = "GET /status HTTP/1.1\r\nHost: pi\r\n\r\n";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "GET");
  CHECK(req.path == "/status");
}

TEST(http_parse_post_capture) {
  std::string raw =
      "POST /capture HTTP/1.1\r\nHost: pi\r\nContent-Length: 0\r\n\r\n";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "POST");
  CHECK(req.path == "/capture");
}

TEST(http_parse_post_settings_with_body) {
  std::string raw = "POST /settings HTTP/1.1\r\nHost: pi\r\nContent-Length: "
                    "15\r\n\r\n{\"iso\":\"400\"}";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "POST");
  CHECK(req.path == "/settings");
  CHECK(req.body == "{\"iso\":\"400\"}");
}

TEST(http_parse_malformed_returns_false) {
  HttpRequest req;
  CHECK(!parseHttpRequest("", req));
  CHECK(!parseHttpRequest("garbage", req));
  CHECK(!parseHttpRequest("GET\r\n", req));
}

TEST(http_parse_newline_only_headers) {
  // Some clients use \n instead of \r\n
  std::string raw = "GET /settings HTTP/1.1\nHost: pi\n\n";
  HttpRequest req;
  CHECK(parseHttpRequest(raw, req));
  CHECK(req.method == "GET");
  CHECK(req.path == "/settings");
}

// --- settingsToJson ---

TEST(settings_json_contains_key_fields) {
  CameraSettings s;
  s.shutterUs = 1000;
  s.analogueGain = 2.0f;
  s.exposureValue = 1.5f;
  s.awbMode = "daylight";
  s.captureFormat = OutputFormat::DNG;
  s.jpegQuality = 85;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"shutterUs\":1000") != std::string::npos);
  CHECK(json.find("\"analogueGain\":2") != std::string::npos);
  CHECK(json.find("\"exposureValue\":1.5") != std::string::npos);
  CHECK(json.find("\"awbMode\":\"daylight\"") != std::string::npos);
  CHECK(json.find("\"captureFormat\":\"dng\"") != std::string::npos);
  CHECK(json.find("\"jpegQuality\":85") != std::string::npos);
  CHECK(json.find("\"driveMode\":\"single\"") != std::string::npos);
}

TEST(settings_json_starts_and_ends_with_braces) {
  CameraSettings s;
  std::string json = settingsToJson(s);
  CHECK(!json.empty());
  CHECK(json.front() == '{');
  CHECK(json.back() == '}');
}

TEST(settings_json_escapes_special_chars) {
  CameraSettings s;
  s.awbMode = "auto\"test";
  std::string json = settingsToJson(s);
  CHECK(json.find("\\\"") != std::string::npos);
}

// --- statusToJson ---

TEST(status_json_contains_required_fields) {
  CameraSettings s;
  s.shutterUs = 5000;
  s.analogueGain = 4.0f;
  s.exposureValue = -1.0f;
  s.awbMode = "cloudy";
  s.driveMode = DriveMode::Continuous;
  s.captureFormat = OutputFormat::PNG;
  std::string json = statusToJson(75, s, 42);
  CHECK(json.find("\"batteryPercent\":75") != std::string::npos);
  CHECK(json.find("\"captureCount\":42") != std::string::npos);
  CHECK(json.find("\"iso\":400") != std::string::npos);
  CHECK(json.find("\"shutterUs\":5000") != std::string::npos);
  CHECK(json.find("\"ev\":-1") != std::string::npos);
  CHECK(json.find("\"wb\":\"cloudy\"") != std::string::npos);
  CHECK(json.find("\"driveMode\":\"continuous\"") != std::string::npos);
  CHECK(json.find("\"format\":\"png\"") != std::string::npos);
}

TEST(status_json_iso_zero_when_auto) {
  CameraSettings s;
  s.analogueGain = 0; // auto
  std::string json = statusToJson(50, s, 0);
  CHECK(json.find("\"iso\":0") != std::string::npos);
}

// --- fileListingHtml ---

TEST(file_listing_html_empty) {
  std::vector<std::string> files;
  std::string html = fileListingHtml(files);
  CHECK(html.find("No captures yet") != std::string::npos);
  CHECK(html.find("<html>") != std::string::npos);
}

TEST(file_listing_html_with_files) {
  std::vector<std::string> files = {"/home/pi/captures/photo1.jpg",
                                    "/home/pi/captures/photo2.dng"};
  std::string html = fileListingHtml(files);
  CHECK(html.find("photo1.jpg") != std::string::npos);
  CHECK(html.find("photo2.dng") != std::string::npos);
  CHECK(html.find("/file/photo1.jpg") != std::string::npos);
  CHECK(html.find("/file/photo2.dng") != std::string::npos);
}

TEST(file_listing_html_escapes_no_crash_on_empty_name) {
  std::vector<std::string> files = {""};
  std::string html = fileListingHtml(files);
  CHECK(!html.empty());
}

// --- applySettingsJson ---

TEST(apply_settings_json_updates_shutter) {
  CameraSettings s;
  s.shutterUs = 0;
  applySettingsJson("{\"shutterUs\":5000}", s);
  CHECK(s.shutterUs == 5000);
}

TEST(apply_settings_json_updates_iso_gain) {
  CameraSettings s;
  s.analogueGain = 0;
  applySettingsJson("{\"analogueGain\":4.0}", s);
  CHECK(s.analogueGain == 4.0f);
}

TEST(apply_settings_json_updates_ev) {
  CameraSettings s;
  s.exposureValue = 0;
  applySettingsJson("{\"exposureValue\":1.5}", s);
  CHECK(s.exposureValue == 1.5f);
}

TEST(apply_settings_json_updates_awb_mode) {
  CameraSettings s;
  s.awbMode = "auto";
  applySettingsJson("{\"awbMode\":\"daylight\"}", s);
  CHECK(s.awbMode == "daylight");
}

TEST(apply_settings_json_updates_format) {
  CameraSettings s;
  s.captureFormat = OutputFormat::JPEG;
  applySettingsJson("{\"captureFormat\":\"dng\"}", s);
  CHECK(s.captureFormat == OutputFormat::DNG);
}

TEST(apply_settings_json_updates_drive_mode) {
  CameraSettings s;
  s.driveMode = DriveMode::Single;
  applySettingsJson("{\"driveMode\":\"bracket\"}", s);
  CHECK(s.driveMode == DriveMode::Bracket);
}

TEST(apply_settings_json_updates_bracket_type_iso) {
  CameraSettings s;
  s.bracketType = BracketType::AE;
  applySettingsJson("{\"bracketType\":\"iso\"}", s);
  CHECK(s.bracketType == BracketType::ISO);
}

TEST(settings_to_json_bracket_type_iso) {
  CameraSettings s;
  s.bracketType = BracketType::ISO;
  std::string json = settingsToJson(s);
  // The JSON should contain "bracketType":"iso"
  CHECK(json.find("\"bracketType\":\"iso\"") != std::string::npos);
}

TEST(apply_settings_json_updates_jpeg_quality) {
  CameraSettings s;
  s.jpegQuality = 90;
  applySettingsJson("{\"jpegQuality\":75}", s);
  CHECK(s.jpegQuality == 75);
}

TEST(apply_settings_json_updates_bool_fields) {
  CameraSettings s;
  s.aeEnable = true;
  s.awbEnable = true;
  applySettingsJson("{\"aeEnable\":false,\"awbEnable\":false}", s);
  CHECK(s.aeEnable == false);
  CHECK(s.awbEnable == false);
}

TEST(apply_settings_json_updates_timer) {
  CameraSettings s;
  s.timerDuration = 0;
  applySettingsJson("{\"timerDuration\":10}", s);
  CHECK(s.timerDuration == 10);
}

TEST(apply_settings_json_ignores_unknown_keys) {
  CameraSettings s;
  s.jpegQuality = 90;
  applySettingsJson("{\"unknownField\":42,\"jpegQuality\":50}", s);
  CHECK(s.jpegQuality == 50);
}

TEST(apply_settings_json_partial_update_preserves_others) {
  CameraSettings s;
  s.shutterUs = 1000;
  s.jpegQuality = 85;
  applySettingsJson("{\"jpegQuality\":50}", s);
  CHECK(s.shutterUs == 1000); // unchanged
  CHECK(s.jpegQuality == 50); // updated
}

TEST(apply_settings_json_updates_multiple_fields) {
  CameraSettings s;
  applySettingsJson("{\"shutterUs\":2000,\"analogueGain\":2.0,"
                    "\"exposureValue\":-1.0,\"awbMode\":\"cloudy\"}",
                    s);
  CHECK(s.shutterUs == 2000);
  CHECK(s.analogueGain == 2.0f);
  CHECK(s.exposureValue == -1.0f);
  CHECK(s.awbMode == "cloudy");
}

TEST(apply_settings_json_empty_body_no_changes) {
  CameraSettings s;
  s.jpegQuality = 90;
  applySettingsJson("{}", s);
  CHECK(s.jpegQuality == 90);
}

TEST(apply_settings_json_updates_exposure_mode) {
  CameraSettings s;
  s.exposureMode = ExposureMode::Program;
  applySettingsJson("{\"exposureMode\":\"manual\"}", s);
  CHECK(s.exposureMode == ExposureMode::Manual);
}

TEST(apply_settings_json_updates_picture_style) {
  CameraSettings s;
  s.pictureStyle = PictureStyle::Standard;
  applySettingsJson("{\"pictureStyle\":\"vivid\"}", s);
  CHECK(s.pictureStyle == PictureStyle::Vivid);
}

TEST(apply_settings_json_updates_image_size) {
  CameraSettings s;
  s.imageSize = ImageSize::Large;
  applySettingsJson("{\"imageSize\":\"medium\"}", s);
  CHECK(s.imageSize == ImageSize::Medium);
}

TEST(apply_settings_json_updates_aspect_ratio) {
  CameraSettings s;
  s.aspectRatio = AspectRatio::Native;
  applySettingsJson("{\"aspectRatio\":\"16:9\"}", s);
  CHECK(s.aspectRatio == AspectRatio::Ratio169);
}

// --- urlDecode ---

TEST(url_decode_percent_encoded) {
  CHECK(urlDecode("hello%20world") == "hello world");
  CHECK(urlDecode("file%2Ftest") == "file/test");
}

TEST(url_decode_plus_to_space) {
  CHECK(urlDecode("hello+world") == "hello world");
}

TEST(url_decode_no_encoding) { CHECK(urlDecode("photo.jpg") == "photo.jpg"); }

TEST(url_decode_empty) { CHECK(urlDecode("").empty()); }

TEST(url_decode_mixed) {
  CHECK(urlDecode("my%20file+name.jpg") == "my file name.jpg");
}

TEST(url_decode_invalid_percent_kept) {
  CHECK(urlDecode("test%2") == "test%2");
  CHECK(urlDecode("test%GG") == "test%GG");
}

// --- extractFileName ---

TEST(extract_file_name_basic) {
  CHECK(extractFileName("/file/photo.jpg") == "photo.jpg");
}

TEST(extract_file_name_with_path) {
  CHECK(extractFileName("/file/subdir/photo.jpg") == "subdir/photo.jpg");
}

TEST(extract_file_name_with_query_string) {
  CHECK(extractFileName("/file/photo.jpg?download=1") == "photo.jpg");
}

TEST(extract_file_name_url_encoded) {
  CHECK(extractFileName("/file/my%20photo.jpg") == "my photo.jpg");
}

TEST(extract_file_name_not_file_path) {
  CHECK(extractFileName("/").empty());
  CHECK(extractFileName("/status").empty());
  CHECK(extractFileName("/settings").empty());
}

TEST(extract_file_name_empty_after_prefix) {
  CHECK(extractFileName("/file/").empty());
}

// --- isSafeFileName (path-traversal rejection) ---

TEST(is_safe_file_name_plain_name) {
  CHECK(isSafeFileName("photo.jpg"));
  CHECK(isSafeFileName("IMG_0001.dng"));
}

TEST(is_safe_file_name_rejects_path_separator) {
  CHECK(!isSafeFileName("subdir/photo.jpg"));
  CHECK(!isSafeFileName("/etc/passwd"));
  CHECK(!isSafeFileName("a/b/c.jpg"));
}

TEST(is_safe_file_name_rejects_parent_traversal) {
  CHECK(!isSafeFileName("../etc/passwd"));
  CHECK(!isSafeFileName(".."));
  // The guard rejects any occurrence of "..", matching the original
  // filename.find("..") check in handleConnection().
  CHECK(!isSafeFileName("photo..jpg"));
  CHECK(!isSafeFileName("..hidden.jpg"));
  CHECK(!isSafeFileName("photo.jpg.."));
}

TEST(is_safe_file_name_empty_is_safe) {
  // An empty string has no '/' or ".." — it is "safe" by this check.
  // The empty case is rejected earlier by the filename.empty() guard in
  // handleConnection(), so isSafeFileName itself returns true here.
  CHECK(isSafeFileName(""));
}

// --- enum range validation in applySettingsJson ---

TEST(apply_settings_json_valid_video_resolution) {
  CameraSettings s;
  applySettingsJson("{\"videoResolution\":2}", s);
  CHECK(s.videoResolution == VideoResolution::Res1280x720);
}

TEST(apply_settings_json_invalid_video_resolution_ignored) {
  CameraSettings s;
  s.videoResolution = VideoResolution::Res640x480;
  applySettingsJson("{\"videoResolution\":99}", s);
  CHECK(s.videoResolution == VideoResolution::Res640x480);
}

TEST(apply_settings_json_negative_video_resolution_ignored) {
  CameraSettings s;
  s.videoResolution = VideoResolution::Res640x480;
  applySettingsJson("{\"videoResolution\":-1}", s);
  CHECK(s.videoResolution == VideoResolution::Res640x480);
}

TEST(apply_settings_json_valid_video_codec) {
  CameraSettings s;
  applySettingsJson("{\"videoCodec\":2}", s);
  CHECK(s.videoCodec == VideoCodec::YUV);
}

TEST(apply_settings_json_invalid_video_codec_ignored) {
  CameraSettings s;
  s.videoCodec = VideoCodec::MJPEG;
  applySettingsJson("{\"videoCodec\":99}", s);
  CHECK(s.videoCodec == VideoCodec::MJPEG);
}

TEST(apply_settings_json_valid_sensor_mode) {
  CameraSettings s;
  applySettingsJson("{\"sensorMode\":3}", s);
  CHECK(s.sensorMode == SensorMode::Mode2028x1520);
}

TEST(apply_settings_json_invalid_sensor_mode_ignored) {
  CameraSettings s;
  s.sensorMode = SensorMode::Mode2028x1080;
  applySettingsJson("{\"sensorMode\":99}", s);
  CHECK(s.sensorMode == SensorMode::Mode2028x1080);
}

TEST(apply_settings_json_negative_sensor_mode_ignored) {
  CameraSettings s;
  s.sensorMode = SensorMode::Mode2028x1080;
  applySettingsJson("{\"sensorMode\":-1}", s);
  CHECK(s.sensorMode == SensorMode::Mode2028x1080);
}

// --- new JSON keys: aeExposureMode, aeConstraintMode, noiseReduction,
//     gridType, showHistogram, zebraMode, focusPeaking, meteringMode,
//     bracketEv ---

TEST(settings_json_contains_ae_exposure_mode) {
  CameraSettings s;
  s.aeExposureMode = AeExposureMode::Long;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"aeExposureMode\":2") != std::string::npos);
}

TEST(apply_settings_json_valid_ae_exposure_mode) {
  CameraSettings s;
  applySettingsJson("{\"aeExposureMode\":1}", s);
  CHECK(s.aeExposureMode == AeExposureMode::Short);
}

TEST(apply_settings_json_invalid_ae_exposure_mode_ignored) {
  CameraSettings s;
  s.aeExposureMode = AeExposureMode::Normal;
  applySettingsJson("{\"aeExposureMode\":99}", s);
  CHECK(s.aeExposureMode == AeExposureMode::Normal);
}

TEST(apply_settings_json_negative_ae_exposure_mode_ignored) {
  CameraSettings s;
  s.aeExposureMode = AeExposureMode::Long;
  applySettingsJson("{\"aeExposureMode\":-1}", s);
  CHECK(s.aeExposureMode == AeExposureMode::Long);
}

TEST(settings_json_contains_ae_constraint_mode) {
  CameraSettings s;
  s.aeConstraintMode = AeConstraintMode::Highlight;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"aeConstraintMode\":1") != std::string::npos);
}

TEST(apply_settings_json_valid_ae_constraint_mode) {
  CameraSettings s;
  applySettingsJson("{\"aeConstraintMode\":2}", s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Shadows);
}

TEST(apply_settings_json_invalid_ae_constraint_mode_ignored) {
  CameraSettings s;
  s.aeConstraintMode = AeConstraintMode::Normal;
  applySettingsJson("{\"aeConstraintMode\":99}", s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Normal);
}

TEST(apply_settings_json_negative_ae_constraint_mode_ignored) {
  CameraSettings s;
  s.aeConstraintMode = AeConstraintMode::Shadows;
  applySettingsJson("{\"aeConstraintMode\":-1}", s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Shadows);
}

TEST(settings_json_contains_noise_reduction) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::HighQuality;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"noiseReduction\":2") != std::string::npos);
}

TEST(apply_settings_json_valid_noise_reduction) {
  CameraSettings s;
  applySettingsJson("{\"noiseReduction\":3}", s);
  CHECK(s.noiseReduction == NoiseReductionMode::Minimal);
}

TEST(apply_settings_json_invalid_noise_reduction_ignored) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::Fast;
  applySettingsJson("{\"noiseReduction\":99}", s);
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
}

TEST(apply_settings_json_negative_noise_reduction_ignored) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::Fast;
  applySettingsJson("{\"noiseReduction\":-1}", s);
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
}

TEST(settings_json_contains_grid_type) {
  CameraSettings s;
  s.gridType = GridType::GoldenRatio;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"gridType\":4") != std::string::npos);
}

TEST(apply_settings_json_valid_grid_type) {
  CameraSettings s;
  applySettingsJson("{\"gridType\":2}", s);
  CHECK(s.gridType == GridType::Square);
}

TEST(apply_settings_json_invalid_grid_type_ignored) {
  CameraSettings s;
  s.gridType = GridType::Thirds;
  applySettingsJson("{\"gridType\":99}", s);
  CHECK(s.gridType == GridType::Thirds);
}

TEST(apply_settings_json_negative_grid_type_ignored) {
  CameraSettings s;
  s.gridType = GridType::Thirds;
  applySettingsJson("{\"gridType\":-1}", s);
  CHECK(s.gridType == GridType::Thirds);
}

TEST(settings_json_contains_show_histogram) {
  CameraSettings s;
  s.showHistogram = true;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"showHistogram\":true") != std::string::npos);
}

TEST(apply_settings_json_valid_show_histogram) {
  CameraSettings s;
  s.showHistogram = false;
  applySettingsJson("{\"showHistogram\":true}", s);
  CHECK(s.showHistogram == true);
}

TEST(apply_settings_json_invalid_show_histogram_ignored) {
  CameraSettings s;
  s.showHistogram = false;
  applySettingsJson("{\"showHistogram\":\"yes\"}", s);
  CHECK(s.showHistogram == false);
}

TEST(settings_json_contains_zebra_mode) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Threshold80;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"zebraMode\":2") != std::string::npos);
}

TEST(apply_settings_json_valid_zebra_mode) {
  CameraSettings s;
  applySettingsJson("{\"zebraMode\":3}", s);
  CHECK(s.zebraMode == ZebraMode::Threshold100);
}

TEST(apply_settings_json_invalid_zebra_mode_ignored) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Off;
  applySettingsJson("{\"zebraMode\":99}", s);
  CHECK(s.zebraMode == ZebraMode::Off);
}

TEST(apply_settings_json_negative_zebra_mode_ignored) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Off;
  applySettingsJson("{\"zebraMode\":-1}", s);
  CHECK(s.zebraMode == ZebraMode::Off);
}

TEST(settings_json_contains_focus_peaking) {
  CameraSettings s;
  s.focusPeaking = true;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"focusPeaking\":true") != std::string::npos);
}

TEST(apply_settings_json_valid_focus_peaking) {
  CameraSettings s;
  s.focusPeaking = false;
  applySettingsJson("{\"focusPeaking\":true}", s);
  CHECK(s.focusPeaking == true);
}

TEST(apply_settings_json_invalid_focus_peaking_ignored) {
  CameraSettings s;
  s.focusPeaking = false;
  applySettingsJson("{\"focusPeaking\":\"on\"}", s);
  CHECK(s.focusPeaking == false);
}

TEST(settings_json_contains_metering_mode) {
  CameraSettings s;
  s.meteringMode = MeteringMode::Spot;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"meteringMode\":\"spot\"") != std::string::npos);
}

TEST(apply_settings_json_valid_metering_mode_string) {
  CameraSettings s;
  applySettingsJson("{\"meteringMode\":\"centre\"}", s);
  CHECK(s.meteringMode == MeteringMode::Centre);
}

TEST(apply_settings_json_valid_metering_mode_integer) {
  CameraSettings s;
  applySettingsJson("{\"meteringMode\":2}", s);
  CHECK(s.meteringMode == MeteringMode::Matrix);
}

TEST(apply_settings_json_invalid_metering_mode_ignored) {
  CameraSettings s;
  s.meteringMode = MeteringMode::Matrix;
  applySettingsJson("{\"meteringMode\":\"bogus\"}", s);
  CHECK(s.meteringMode == MeteringMode::Matrix);
}

TEST(apply_settings_json_invalid_metering_mode_int_ignored) {
  CameraSettings s;
  s.meteringMode = MeteringMode::Matrix;
  applySettingsJson("{\"meteringMode\":99}", s);
  CHECK(s.meteringMode == MeteringMode::Matrix);
}

TEST(settings_json_contains_bracket_ev) {
  CameraSettings s;
  s.bracketEv = {-1.0f, 0.0f, 1.0f};
  std::string json = settingsToJson(s);
  CHECK(json.find("\"bracketEv\":\"-1,0,1\"") != std::string::npos);
}

TEST(settings_json_bracket_ev_empty) {
  CameraSettings s;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"bracketEv\":\"\"") != std::string::npos);
}

TEST(apply_settings_json_valid_bracket_ev) {
  CameraSettings s;
  applySettingsJson("{\"bracketEv\":\"-0.5,0,0.5\"}", s);
  CHECK(s.bracketEv.size() == 3);
  CHECK(s.bracketEv[0] == -0.5f);
  CHECK(s.bracketEv[1] == 0.0f);
  CHECK(s.bracketEv[2] == 0.5f);
}

TEST(apply_settings_json_bracket_ev_empty_string) {
  CameraSettings s;
  s.bracketEv = {-1.0f, 1.0f};
  applySettingsJson("{\"bracketEv\":\"\"}", s);
  CHECK(s.bracketEv.empty());
}

TEST(apply_settings_json_invalid_bracket_ev_ignored) {
  CameraSettings s;
  s.bracketEv = {-1.0f, 1.0f};
  applySettingsJson("{\"bracketEv\":\"abc\"}", s);
  CHECK(s.bracketEv.size() == 2);
  CHECK(s.bracketEv[0] == -1.0f);
  CHECK(s.bracketEv[1] == 1.0f);
}

// --- fileNamingMode and useDateSubfolders ---

TEST(settings_json_contains_file_naming_mode) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Sequential;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"fileNamingMode\":1") != std::string::npos);
}

TEST(settings_json_contains_file_naming_mode_timestamp) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Timestamp;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"fileNamingMode\":0") != std::string::npos);
}

TEST(apply_settings_json_valid_file_naming_mode_sequential) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Timestamp;
  applySettingsJson("{\"fileNamingMode\":1}", s);
  CHECK(s.fileNamingMode == FileNamingMode::Sequential);
}

TEST(apply_settings_json_valid_file_naming_mode_timestamp) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Sequential;
  applySettingsJson("{\"fileNamingMode\":0}", s);
  CHECK(s.fileNamingMode == FileNamingMode::Timestamp);
}

TEST(apply_settings_json_invalid_file_naming_mode_ignored) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Timestamp;
  applySettingsJson("{\"fileNamingMode\":99}", s);
  CHECK(s.fileNamingMode == FileNamingMode::Timestamp);
}

TEST(apply_settings_json_negative_file_naming_mode_ignored) {
  CameraSettings s;
  s.fileNamingMode = FileNamingMode::Sequential;
  applySettingsJson("{\"fileNamingMode\":-1}", s);
  CHECK(s.fileNamingMode == FileNamingMode::Sequential);
}

TEST(settings_json_contains_use_date_subfolders) {
  CameraSettings s;
  s.useDateSubfolders = true;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"useDateSubfolders\":true") != std::string::npos);
}

TEST(settings_json_contains_use_date_subfolders_false) {
  CameraSettings s;
  s.useDateSubfolders = false;
  std::string json = settingsToJson(s);
  CHECK(json.find("\"useDateSubfolders\":false") != std::string::npos);
}

TEST(apply_settings_json_valid_use_date_subfolders_true) {
  CameraSettings s;
  s.useDateSubfolders = false;
  applySettingsJson("{\"useDateSubfolders\":true}", s);
  CHECK(s.useDateSubfolders == true);
}

TEST(apply_settings_json_valid_use_date_subfolders_false) {
  CameraSettings s;
  s.useDateSubfolders = true;
  applySettingsJson("{\"useDateSubfolders\":false}", s);
  CHECK(s.useDateSubfolders == false);
}

TEST(apply_settings_json_invalid_use_date_subfolders_ignored) {
  CameraSettings s;
  s.useDateSubfolders = true;
  applySettingsJson("{\"useDateSubfolders\":\"yes\"}", s);
  CHECK(s.useDateSubfolders == true);
}
