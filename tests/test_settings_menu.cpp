#include "camera_mode.h"
#include "settings_menu.h"
#include "test_runner.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <unistd.h>

using picamera::AeConstraintMode;
using picamera::AeExposureMode;
using picamera::AspectRatio;
using picamera::aspectRatioCrop;
using picamera::basicMenuItemAdjustLeft;
using picamera::basicMenuItemAdjustRight;
using picamera::basicMenuItemCount;
using picamera::basicMenuItemIsAdvancedToggle;
using picamera::basicMenuItemIsFormatCard;
using picamera::basicMenuItemLabel;
using picamera::basicMenuItemValue;
using picamera::BracketType;
using picamera::CameraConfig;
using picamera::CameraSettings;
using picamera::CropRegion;
using picamera::defaultSettingsPath;
using picamera::DriveMode;
using picamera::ExposureMode;
using picamera::FileNamingMode;
using picamera::GridType;
using picamera::ImageSize;
using picamera::loadSettings;
using picamera::MenuMode;
using picamera::MeteringMode;
using picamera::NoiseReductionMode;
using picamera::OutputFormat;
using picamera::PictureStyle;
using picamera::pictureStyleParams;
using picamera::saveSettings;
using picamera::sensorModeDims;
using picamera::SensorMode;
using picamera::settingsItemAdjustLeft;
using picamera::settingsItemAdjustRight;
using picamera::settingsItemLabel;
using picamera::settingsItemValue;
using picamera::settingsNeedsReconfigure;
using picamera::SettingsTab;
using picamera::settingsTabItemCount;
using picamera::settingsToCameraConfig;
using picamera::VideoCodec;
using picamera::VideoDimensions;
using picamera::videoResolutionDims;
using picamera::VideoResolution;
using picamera::ZebraMode;

// --- Item count tests ---

TEST(settings_tab_capture_has_13_items) {
  CHECK(settingsTabItemCount(SettingsTab::Capture) == 13);
}

TEST(settings_tab_exposure_has_14_items) {
  CHECK(settingsTabItemCount(SettingsTab::Exposure) == 14);
}

TEST(settings_tab_color_has_14_items) {
  CHECK(settingsTabItemCount(SettingsTab::Color) == 14);
}

TEST(settings_tab_focusdisp_has_8_items) {
  CHECK(settingsTabItemCount(SettingsTab::FocusDisp) == 8);
}

TEST(settings_tab_video_has_5_items) {
  CHECK(settingsTabItemCount(SettingsTab::Video) == 5);
}

TEST(settings_tab_system_has_12_items) {
  CHECK(settingsTabItemCount(SettingsTab::System) == 12);
}

// --- Label tests ---

TEST(settings_label_shooting_drive) {
  CHECK(settingsItemLabel(SettingsTab::Capture, 6) == "DRIVE");
}

TEST(settings_label_shooting_shutter) {
  CHECK(settingsItemLabel(SettingsTab::Exposure, 1) == "SHUTTER");
}

TEST(settings_label_image_format) {
  CHECK(settingsItemLabel(SettingsTab::Capture, 0) == "FORMAT");
}

TEST(settings_label_display_grid) {
  CHECK(settingsItemLabel(SettingsTab::FocusDisp, 2) == "GRID");
}

TEST(settings_label_system_exit) {
  CHECK(settingsItemLabel(SettingsTab::System, 11) == "EXIT");
}

TEST(settings_label_out_of_range_returns_question) {
  CHECK(settingsItemLabel(SettingsTab::Capture, 99) == "??");
}

// --- Value tests ---

TEST(settings_value_drive_default_single) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 6, s) == "SINGLE");
}

TEST(settings_value_shutter_default_auto) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 1, s) == "AUTO");
}

TEST(settings_value_iso_default_auto) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 2, s) == "AUTO");
}

TEST(settings_value_ev_default_zero) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 6, s) == "0");
}

TEST(settings_value_format_default_jpeg) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 0, s) == "JPEG");
}

TEST(settings_value_grid_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 2, s) == "OFF");
}

TEST(settings_value_battery_default_on) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::System, 0, s) == "ON");
}

// --- Adjust tests ---

TEST(settings_adjust_drive_mode_cycles) {
  CameraSettings s;
  // Single -> SelfTimer
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::SelfTimer);
  // SelfTimer -> Bracket
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Bracket);
  // Bracket -> Timelapse
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Timelapse);
  // Timelapse -> Continuous
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Continuous);
  // Continuous -> Bulb
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Bulb);
  // Bulb -> Video
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Video);
  // Video -> Single (wrap)
  settingsItemAdjustRight(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Single);
}

TEST(settings_adjust_drive_mode_left_cycles) {
  CameraSettings s;
  // Single -> Video (wrap backward)
  settingsItemAdjustLeft(SettingsTab::Capture, 6, s);
  CHECK(s.driveMode == DriveMode::Video);
}

TEST(settings_value_drive_mode_bulb_label) {
  CameraSettings s;
  s.driveMode = DriveMode::Bulb;
  CHECK(settingsItemValue(SettingsTab::Capture, 6, s) == "BULB");
}

TEST(settings_value_drive_mode_video_label) {
  CameraSettings s;
  s.driveMode = DriveMode::Video;
  CHECK(settingsItemValue(SettingsTab::Capture, 6, s) == "VIDEO");
}

TEST(settings_adjust_shutter_increases) {
  CameraSettings s;
  CHECK(s.shutterUs == 0);
  settingsItemAdjustRight(SettingsTab::Exposure, 1, s);
  CHECK(s.shutterUs > 0);
  CHECK(s.aeEnable == false); // manual shutter disables AE
}

TEST(settings_adjust_shutter_back_to_auto_reenables_ae) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Exposure, 1, s);
  CHECK(s.aeEnable == false);
  // Cycle back to AUTO (shutterUs == 0)
  while (s.shutterUs != 0) {
    settingsItemAdjustLeft(SettingsTab::Exposure, 1, s);
  }
  CHECK(s.shutterUs == 0);
  CHECK(s.aeEnable == true); // AE re-enabled
}

TEST(settings_adjust_iso_back_to_auto_reenables_ae) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Exposure, 2, s);
  CHECK(s.analogueGain > 0);
  CHECK(s.aeEnable == false);
  // Cycle back to AUTO (gain == 0)
  while (s.analogueGain != 0.0f) {
    settingsItemAdjustLeft(SettingsTab::Exposure, 2, s);
  }
  CHECK(s.analogueGain == 0.0f);
  CHECK(s.aeEnable == true);
}

TEST(settings_adjust_shutter_decreases) {
  CameraSettings s;
  s.shutterUs = 8000; // 1/125
  settingsItemAdjustLeft(SettingsTab::Exposure, 1, s);
  CHECK(s.shutterUs < 8000);
}

TEST(settings_adjust_iso_increases) {
  CameraSettings s;
  CHECK(s.analogueGain == 0);
  settingsItemAdjustRight(SettingsTab::Exposure, 2, s);
  CHECK(s.analogueGain > 0);
}

TEST(settings_adjust_ev_increases) {
  CameraSettings s;
  CHECK(s.exposureValue == 0);
  settingsItemAdjustRight(SettingsTab::Exposure, 6, s);
  CHECK(s.exposureValue > 0);
}

TEST(settings_adjust_ev_decreases) {
  CameraSettings s;
  s.exposureValue = 1.0f;
  settingsItemAdjustLeft(SettingsTab::Exposure, 6, s);
  CHECK(s.exposureValue < 1.0f);
}

TEST(settings_adjust_metering_cycles) {
  CameraSettings s;
  CHECK(s.meteringMode == MeteringMode::Matrix);
  settingsItemAdjustRight(SettingsTab::Exposure, 7, s);
  CHECK(s.meteringMode == MeteringMode::Centre);
  settingsItemAdjustRight(SettingsTab::Exposure, 7, s);
  CHECK(s.meteringMode == MeteringMode::Spot);
  settingsItemAdjustRight(SettingsTab::Exposure, 7, s);
  CHECK(s.meteringMode == MeteringMode::Matrix);
}

TEST(settings_adjust_format_cycles_right) {
  CameraSettings s;
  CHECK(s.captureFormat == OutputFormat::JPEG);
  settingsItemAdjustRight(SettingsTab::Capture, 0, s);
  CHECK(s.captureFormat == OutputFormat::DNG);
  settingsItemAdjustRight(SettingsTab::Capture, 0, s);
  CHECK(s.captureFormat == OutputFormat::DngJpeg);
  settingsItemAdjustRight(SettingsTab::Capture, 0, s);
  CHECK(s.captureFormat == OutputFormat::RawJpeg);
  settingsItemAdjustRight(SettingsTab::Capture, 0, s);
  CHECK(s.captureFormat == OutputFormat::PNG);
}

TEST(settings_adjust_format_cycles_left) {
  CameraSettings s;
  CHECK(s.captureFormat == OutputFormat::JPEG);
  settingsItemAdjustLeft(SettingsTab::Capture, 0, s);
  CHECK(s.captureFormat == OutputFormat::RAW_NV12);
}

TEST(settings_adjust_quality_increases) {
  CameraSettings s;
  CHECK(s.jpegQuality == 90);
  settingsItemAdjustRight(SettingsTab::Capture, 1, s);
  CHECK(s.jpegQuality == 91);
}

TEST(settings_adjust_quality_decreases) {
  CameraSettings s;
  s.jpegQuality = 50;
  settingsItemAdjustLeft(SettingsTab::Capture, 1, s);
  CHECK(s.jpegQuality == 49);
}

TEST(settings_adjust_quality_min_10) {
  CameraSettings s;
  s.jpegQuality = 10;
  settingsItemAdjustLeft(SettingsTab::Capture, 1, s);
  CHECK(s.jpegQuality == 10); // clamped
}

TEST(settings_adjust_quality_max_100) {
  CameraSettings s;
  s.jpegQuality = 100;
  settingsItemAdjustRight(SettingsTab::Capture, 1, s);
  CHECK(s.jpegQuality == 100); // clamped
}

TEST(settings_adjust_grid_cycles) {
  CameraSettings s;
  CHECK(s.gridType == GridType::Off);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 2, s);
  CHECK(s.gridType == GridType::Thirds);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 2, s);
  CHECK(s.gridType == GridType::Square);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 2, s);
  CHECK(s.gridType == GridType::Diagonal);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 2, s);
  CHECK(s.gridType == GridType::GoldenRatio);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 2, s);
  CHECK(s.gridType == GridType::Off);
}

TEST(settings_adjust_brightness_increases) {
  CameraSettings s;
  CHECK(s.displayBrightness == 100);
  // Already at max — should stay
  settingsItemAdjustRight(SettingsTab::FocusDisp, 6, s);
  CHECK(s.displayBrightness == 100);
}

TEST(settings_adjust_brightness_decreases) {
  CameraSettings s;
  s.displayBrightness = 50;
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 6, s);
  CHECK(s.displayBrightness == 40);
}

TEST(settings_adjust_brightness_min_10) {
  CameraSettings s;
  s.displayBrightness = 10;
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 6, s);
  CHECK(s.displayBrightness == 10); // clamped
}

TEST(settings_adjust_awb_cycles_through_modes) {
  CameraSettings s;
  CHECK(s.awbEnable == true);
  CHECK(s.awbMode == "auto");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "daylight");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "cloudy");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "incandescent");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "tungsten");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "fluorescent");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "indoor");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "shade");
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "flash");
  // Next step turns AWB off
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbEnable == false);
  // Next step turns it back on with "auto"
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbEnable == true);
  CHECK(s.awbMode == "auto");
}

TEST(settings_adjust_bracket_increases_spread) {
  CameraSettings s;
  CHECK(s.bracketEv.empty());
  settingsItemAdjustRight(SettingsTab::Capture, 8, s);
  CHECK(s.bracketEv.size() == 3);
  CHECK(s.bracketEv[0] < 0);
  CHECK(s.bracketEv[1] == 0);
  CHECK(s.bracketEv[2] > 0);
}

TEST(settings_adjust_bracket_decreases_to_off) {
  CameraSettings s;
  s.bracketEv = {-0.5f, 0.0f, 0.5f};
  settingsItemAdjustLeft(SettingsTab::Capture, 8, s);
  CHECK(s.bracketEv.empty());
}

TEST(settings_adjust_flicker_cycles) {
  CameraSettings s;
  CHECK(s.antiFlicker == false);
  settingsItemAdjustRight(SettingsTab::Exposure, 10, s);
  CHECK(s.antiFlicker == true);
  CHECK(s.flickerHz == 50);
  settingsItemAdjustRight(SettingsTab::Exposure, 10, s);
  CHECK(s.flickerHz == 60);
  settingsItemAdjustLeft(SettingsTab::Exposure, 10, s);
  CHECK(s.flickerHz == 50);
  settingsItemAdjustLeft(SettingsTab::Exposure, 10, s);
  CHECK(s.antiFlicker == false);
}

TEST(settings_adjust_timer_cycles_through_presets) {
  CameraSettings s;
  CHECK(s.timerDuration == 0);
  // 0 -> 2
  settingsItemAdjustRight(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 2);
  // 2 -> 5
  settingsItemAdjustRight(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 5);
  // 5 -> 10
  settingsItemAdjustRight(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 10);
  // 10 -> 0 (wrap)
  settingsItemAdjustRight(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 0);
}

TEST(settings_adjust_timer_left_cycles_backward) {
  CameraSettings s;
  CHECK(s.timerDuration == 0);
  // 0 -> 10 (wrap backward)
  settingsItemAdjustLeft(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 10);
  // 10 -> 5
  settingsItemAdjustLeft(SettingsTab::Capture, 7, s);
  CHECK(s.timerDuration == 5);
}

// --- Reconfigure detection ---

TEST(settings_needs_reconfigure_format_change) {
  CameraSettings a;
  CameraSettings b;
  b.captureFormat = OutputFormat::DNG;
  CHECK(settingsNeedsReconfigure(a, b));
}

TEST(settings_needs_reconfigure_aspect_change) {
  CameraSettings a;
  CameraSettings b;
  b.aspectRatio = AspectRatio::Ratio169;
  CHECK(settingsNeedsReconfigure(a, b));
}

TEST(settings_no_reconfigure_on_exposure_only) {
  CameraSettings a;
  CameraSettings b;
  b.exposureValue = 1.0f;
  CHECK(!settingsNeedsReconfigure(a, b));
}

TEST(settings_no_reconfigure_on_same_values) {
  CameraSettings a;
  CameraSettings b;
  CHECK(!settingsNeedsReconfigure(a, b));
}

// --- settingsToCameraConfig mapping ---

TEST(settings_to_config_maps_basic_fields) {
  CameraSettings s;
  s.shutterUs = 1000;
  s.analogueGain = 2.0f;
  s.awbMode = "daylight";
  s.captureFormat = OutputFormat::PNG;
  s.jpegQuality = 85;

  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.exposureTime == 1000);
  CHECK(cfg.analogueGain == 2.0f);
  CHECK(cfg.awbMode == "daylight");
  CHECK(cfg.format == OutputFormat::PNG);
  CHECK(cfg.jpegQuality == 85);
  CHECK(cfg.width == 4056);
  CHECK(cfg.height == 3040);
}

TEST(settings_to_config_maps_extended_fields) {
  CameraSettings s;
  s.exposureValue = 1.5f;
  s.meteringMode = MeteringMode::Spot;
  s.brightness = 0.5f;
  s.contrast = 1.2f;
  s.saturation = 0.8f;
  s.sharpness = 2.0f;
  s.antiFlicker = true;
  s.flickerHz = 60;
  s.awbEnable = false;
  s.wbRedGain = 1.5f;
  s.wbBlueGain = 0.9f;

  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.exposureValue == 1.5f);
  // libcamera: Centre=0, Spot=1, Matrix=2
  CHECK(cfg.meteringMode == MeteringMode::Spot);
  CHECK(cfg.brightness == 0.5f);
  CHECK(cfg.contrast == 1.2f);
  CHECK(cfg.saturation == 0.8f);
  CHECK(cfg.sharpness == 2.0f);
  CHECK(cfg.antiFlicker == true);
  CHECK(cfg.flickerPeriodUs == 8333); // 60Hz → 120Hz light flicker
  CHECK(cfg.awbEnable == false);
  CHECK(cfg.wbRedGain == 1.5f);
  CHECK(cfg.wbBlueGain == 0.9f);
}

TEST(settings_to_config_metering_matrix_maps_to_2) {
  CameraSettings s;
  s.meteringMode = MeteringMode::Matrix;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.meteringMode == MeteringMode::Matrix);
}

TEST(settings_to_config_metering_centre_maps_to_0) {
  CameraSettings s;
  s.meteringMode = MeteringMode::Centre;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.meteringMode == MeteringMode::Centre);
}

TEST(settings_to_config_flicker_50hz) {
  CameraSettings s;
  s.antiFlicker = true;
  s.flickerHz = 50;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.flickerPeriodUs == 10000); // 50Hz → 100Hz light flicker
}

TEST(settings_to_config_flicker_off) {
  CameraSettings s;
  s.antiFlicker = false;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.flickerPeriodUs == 0);
}

TEST(settings_to_config_bracket_ev) {
  CameraSettings s;
  s.bracketEv = {-2.0f, 0.0f, 2.0f};
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.bracketEv.size() == 3);
  CHECK(cfg.bracketEv[0] == -2.0f);
  CHECK(cfg.bracketEv[2] == 2.0f);
}

TEST(settings_to_config_maps_iso_range) {
  CameraSettings s;
  s.isoMin = 200;
  s.isoMax = 1600;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.isoMin == 200);
  CHECK(cfg.isoMax == 1600);
}

TEST(settings_to_config_iso_range_defaults) {
  CameraSettings s;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.isoMin == 100);
  CHECK(cfg.isoMax == 3200);
}

// --- clampGainToIsoRange (manual gain clamping to ISO range) ---

using picamera::clampGainToIsoRange;

TEST(clamp_gain_auto_passthrough) {
  // Non-positive gain means "auto/unset" — returned unchanged.
  CHECK(clampGainToIsoRange(0.0f, 100, 3200) == 0.0f);
  CHECK(clampGainToIsoRange(-1.0f, 100, 3200) == -1.0f);
}

TEST(clamp_gain_within_range_unchanged) {
  // ISO 100..3200 → gain 1.0..32.0; 4.0x (ISO 400) is inside.
  CHECK(clampGainToIsoRange(4.0f, 100, 3200) == 4.0f);
  CHECK(clampGainToIsoRange(1.0f, 100, 3200) == 1.0f);
  CHECK(clampGainToIsoRange(32.0f, 100, 3200) == 32.0f);
}

TEST(clamp_gain_above_max_clamped) {
  // 64.0x = ISO 6400, above max 3200 (32.0x) → clamped to 32.0.
  CHECK(clampGainToIsoRange(64.0f, 100, 3200) == 32.0f);
}

TEST(clamp_gain_below_min_clamped) {
  // 0.5x = ISO 50, below min 100 (1.0x) → clamped to 1.0.
  CHECK(clampGainToIsoRange(0.5f, 100, 3200) == 1.0f);
}

TEST(clamp_gain_narrow_range) {
  // ISO 200..800 → gain 2.0..8.0.
  CHECK(clampGainToIsoRange(1.0f, 200, 800) == 2.0f);
  CHECK(clampGainToIsoRange(10.0f, 200, 800) == 8.0f);
  CHECK(clampGainToIsoRange(5.0f, 200, 800) == 5.0f);
}

TEST(clamp_gain_inverted_range_defensive) {
  // If isoMax < isoMin (misconfiguration), use isoMin as the bound.
  CHECK(clampGainToIsoRange(10.0f, 800, 200) == 8.0f);
  CHECK(clampGainToIsoRange(1.0f, 800, 200) == 8.0f);
}

// --- formatShutter edge cases (tested via settingsItemValue) ---

TEST(settings_value_shutter_near_1s_shows_1s) {
  CameraSettings s;
  s.shutterUs = 999999; // just under 1s — denom would be 1
  CHECK(settingsItemValue(SettingsTab::Exposure, 1, s) == "1\"");
}

TEST(settings_value_shutter_half_second) {
  CameraSettings s;
  s.shutterUs = 500000; // 1/2
  CHECK(settingsItemValue(SettingsTab::Exposure, 1, s) == "1/2");
}

TEST(settings_value_shutter_one_minute) {
  CameraSettings s;
  s.shutterUs = 60000000; // 60s = 1'
  CHECK(settingsItemValue(SettingsTab::Exposure, 1, s) == "1'");
}

// --- Float slider clamp regression tests ---

TEST(settings_adjust_wb_red_clamps_min) {
  CameraSettings s;
  s.wbRedGain = 0.05f; // below min
  settingsItemAdjustLeft(SettingsTab::Color, 2, s);
  CHECK(s.wbRedGain == 0.1f); // clamped to min
}

TEST(settings_adjust_sharpness_clamps_max) {
  CameraSettings s;
  s.sharpness = 15.8f;
  settingsItemAdjustRight(SettingsTab::Color, 9, s);
  CHECK(s.sharpness == 16.0f); // clamped to max
}

TEST(settings_adjust_brightness_clamps_min) {
  CameraSettings s;
  s.brightness = -0.95f;
  settingsItemAdjustLeft(SettingsTab::Color, 6, s);
  CHECK(s.brightness == -1.0f); // clamped to min
}

// --- AWB unknown mode fallback regression test ---

TEST(settings_adjust_awb_unknown_mode_falls_back_to_auto) {
  CameraSettings s;
  s.awbMode = "bogus"; // not a valid mode
  settingsItemAdjustLeft(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "auto"); // fallback
  s.awbMode = "bogus";
  settingsItemAdjustRight(SettingsTab::Color, 0, s);
  CHECK(s.awbMode == "auto"); // fallback
}

// --- Flicker right-cycle wrap to OFF regression test ---

TEST(settings_adjust_flicker_right_wraps_to_off) {
  CameraSettings s;
  s.antiFlicker = true;
  s.flickerHz = 60;
  settingsItemAdjustRight(SettingsTab::Exposure, 10, s);
  CHECK(s.antiFlicker == false); // 60Hz -> OFF
}

// --- New feature tests ---

TEST(settings_adjust_image_size_cycles) {
  CameraSettings s;
  CHECK(s.imageSize == ImageSize::Large);
  settingsItemAdjustRight(SettingsTab::Capture, 3, s);
  CHECK(s.imageSize == ImageSize::Medium);
  settingsItemAdjustRight(SettingsTab::Capture, 3, s);
  CHECK(s.imageSize == ImageSize::Small);
  settingsItemAdjustRight(SettingsTab::Capture, 3, s);
  CHECK(s.imageSize == ImageSize::Large);
}

TEST(settings_adjust_file_naming_cycles) {
  CameraSettings s;
  CHECK(s.fileNamingMode == FileNamingMode::Timestamp);
  settingsItemAdjustRight(SettingsTab::System, 6, s);
  CHECK(s.fileNamingMode == FileNamingMode::Sequential);
  settingsItemAdjustRight(SettingsTab::System, 6, s);
  CHECK(s.fileNamingMode == FileNamingMode::Timestamp);
}

TEST(settings_adjust_date_subfolders_toggles) {
  CameraSettings s;
  CHECK(s.useDateSubfolders == false);
  settingsItemAdjustRight(SettingsTab::System, 7, s);
  CHECK(s.useDateSubfolders == true);
  settingsItemAdjustLeft(SettingsTab::System, 7, s);
  CHECK(s.useDateSubfolders == false);
}

TEST(settings_label_png_is_capture_idx_2) {
  CHECK(settingsItemLabel(SettingsTab::Capture, 2) == "PNG");
}

TEST(settings_value_png_default_6) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 2, s) == "6");
}

TEST(settings_adjust_png_increases) {
  CameraSettings s;
  CHECK(s.pngLevel == 6);
  settingsItemAdjustRight(SettingsTab::Capture, 2, s);
  CHECK(s.pngLevel == 7);
  settingsItemAdjustRight(SettingsTab::Capture, 2, s);
  CHECK(s.pngLevel == 8);
}

TEST(settings_adjust_png_clamps_at_9) {
  CameraSettings s;
  s.pngLevel = 9;
  settingsItemAdjustRight(SettingsTab::Capture, 2, s);
  CHECK(s.pngLevel == 9);
}

TEST(settings_adjust_png_clamps_at_0) {
  CameraSettings s;
  s.pngLevel = 0;
  settingsItemAdjustLeft(SettingsTab::Capture, 2, s);
  CHECK(s.pngLevel == 0);
}

TEST(settings_label_dgain_is_exposure_idx_5) {
  CHECK(settingsItemLabel(SettingsTab::Exposure, 5) == "DGAIN");
}

TEST(settings_value_dgain_default_is_1_0) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 5, s) == "1.0");
}

TEST(settings_adjust_dgain_increases_from_default) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Exposure, 5, s);
  CHECK(s.digitalGain > 1.49f && s.digitalGain < 1.51f);
}

TEST(settings_adjust_dgain_clamps_at_16) {
  CameraSettings s;
  s.digitalGain = 16.0f;
  settingsItemAdjustRight(SettingsTab::Exposure, 5, s);
  CHECK(s.digitalGain == 16.0f);
}

TEST(settings_adjust_dgain_clamps_at_1) {
  CameraSettings s;
  s.digitalGain = 1.0f;
  settingsItemAdjustLeft(SettingsTab::Exposure, 5, s);
  CHECK(s.digitalGain == 1.0f);
}

TEST(settings_adjust_dgain_from_legacy_zero) {
  CameraSettings s;
  s.digitalGain = 0;
  settingsItemAdjustLeft(SettingsTab::Exposure, 5, s);
  CHECK(s.digitalGain == 1.0f);
}

TEST(settings_to_config_maps_wb_kelvin) {
  CameraSettings s;
  s.wbKelvin = 5500;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.wbKelvin == 5500);
}

TEST(settings_to_config_maps_image_size) {
  CameraSettings s;
  s.imageSize = ImageSize::Medium;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.imageSize == ImageSize::Medium);
}

TEST(settings_to_config_maps_aspect_ratio) {
  CameraSettings s;
  s.aspectRatio = AspectRatio::Ratio169;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.aspectRatio == AspectRatio::Ratio169);
}

TEST(settings_to_config_shutter_priority_keeps_ae_on) {
  // Shutter priority: manual shutter, auto gain — AE must stay on.
  CameraSettings s;
  s.exposureMode = ExposureMode::Shutter;
  s.shutterUs = 1000;
  s.aeEnable = true;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.aeEnable == true);
  CHECK(cfg.exposureTime == 1000);
  CHECK(cfg.analogueGain == 0); // auto gain
}

TEST(settings_to_config_manual_mode_disables_ae) {
  CameraSettings s;
  s.exposureMode = ExposureMode::Manual;
  s.shutterUs = 1000;
  s.analogueGain = 2.0f;
  CameraConfig cfg = settingsToCameraConfig(s, 4056, 3040);
  CHECK(cfg.aeEnable == false);
  CHECK(cfg.exposureTime == 1000);
  CHECK(cfg.analogueGain == 2.0f);
}

TEST(settings_save_load_roundtrip) {
  CameraSettings s;
  s.driveMode = DriveMode::Continuous;
  s.shutterUs = 1000;
  s.analogueGain = 2.5f;
  s.exposureValue = 1.5f;
  s.captureFormat = OutputFormat::RawJpeg;
  s.jpegQuality = 85;
  s.imageSize = ImageSize::Small;
  s.awbMode = "shade";
  s.wbKelvin = 6500;
  s.gridType = GridType::GoldenRatio;
  s.fileNamingMode = FileNamingMode::Sequential;
  s.useDateSubfolders = true;
  s.focusMagnify = 2;
  s.bracketEv = {-1.0f, 0.0f, 1.0f};

  std::string path = picamera::test::tmpPath(".conf");
  CHECK(saveSettings(s, path));

  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.driveMode == DriveMode::Continuous);
  CHECK(loaded.shutterUs == 1000);
  CHECK(loaded.analogueGain == 2.5f);
  CHECK(loaded.exposureValue == 1.5f);
  CHECK(loaded.captureFormat == OutputFormat::RawJpeg);
  CHECK(loaded.jpegQuality == 85);
  CHECK(loaded.imageSize == ImageSize::Small);
  CHECK(loaded.awbMode == "shade");
  CHECK(loaded.wbKelvin == 6500);
  CHECK(loaded.gridType == GridType::GoldenRatio);
  CHECK(loaded.fileNamingMode == FileNamingMode::Sequential);
  CHECK(loaded.useDateSubfolders == true);
  CHECK(loaded.focusMagnify == 2);
  CHECK(loaded.bracketEv.size() == 3);
  CHECK(loaded.bracketEv[0] == -1.0f);
  CHECK(loaded.bracketEv[2] == 1.0f);
  std::remove(path.c_str());
}

TEST(settings_load_nonexistent_returns_false) {
  CameraSettings s;
  CHECK(!loadSettings(s, "/nonexistent/path/that/does/not/exist.conf"));
}

TEST(settings_default_path_contains_config) {
  std::string p = defaultSettingsPath();
  CHECK(p.find(".config/picamera") != std::string::npos);
  CHECK(p.find("settings.conf") != std::string::npos);
}

// --- Exposure mode tests ---

TEST(settings_adjust_exposure_mode_cycles) {
  CameraSettings s;
  CHECK(s.exposureMode == ExposureMode::Program);
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Shutter);
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Manual);
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Auto);
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Program);
}

TEST(settings_exposure_mode_shutter_sets_manual_shutter) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Shutter);
  CHECK(s.shutterUs > 0);
  CHECK(s.aeEnable == true);
}

TEST(settings_exposure_mode_manual_disables_ae) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s);
  CHECK(s.exposureMode == ExposureMode::Manual);
  CHECK(s.aeEnable == false);
  CHECK(s.shutterUs > 0);
  CHECK(s.analogueGain > 0);
}

TEST(settings_exposure_mode_program_reenables_ae) {
  CameraSettings s;
  s.exposureMode = ExposureMode::Manual;
  s.aeEnable = false;
  s.shutterUs = 1000;
  s.analogueGain = 2.0f;
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s); // Auto
  settingsItemAdjustRight(SettingsTab::Exposure, 0, s); // Program
  CHECK(s.exposureMode == ExposureMode::Program);
  CHECK(s.aeEnable == true);
  CHECK(s.shutterUs == 0);
  CHECK(s.analogueGain == 0);
}

TEST(settings_value_exposure_mode_default) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 0, s) == "P");
}

// --- Bracket type tests ---

TEST(settings_adjust_bracket_type_cycles) {
  CameraSettings s;
  CHECK(s.bracketType == BracketType::AE);
  settingsItemAdjustRight(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::WB);
  settingsItemAdjustRight(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::ISO);
  settingsItemAdjustRight(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::AE);
}

TEST(settings_value_bracket_type_default_ae) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 9, s) == "AE");
}

TEST(settings_value_bracket_type_iso) {
  CameraSettings s;
  s.bracketType = BracketType::ISO;
  CHECK(settingsItemValue(SettingsTab::Capture, 9, s) == "ISO");
}

TEST(settings_adjust_bracket_type_reverse_cycles) {
  CameraSettings s;
  s.bracketType = BracketType::AE;
  settingsItemAdjustLeft(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::ISO);
  settingsItemAdjustLeft(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::WB);
  settingsItemAdjustLeft(SettingsTab::Capture, 9, s);
  CHECK(s.bracketType == BracketType::AE);
}

// --- Picture style tests ---

TEST(settings_adjust_picture_style_cycles) {
  CameraSettings s;
  CHECK(s.pictureStyle == PictureStyle::Standard);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Vivid);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Natural);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Monochrome);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Portrait);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Landscape);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Sepia);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Cool);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Warm);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Film);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::HDR);
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Standard);
}

TEST(settings_picture_style_vivid_sets_params) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Color, 11, s);
  CHECK(s.pictureStyle == PictureStyle::Vivid);
  CHECK(s.contrast > 1.0f);
  CHECK(s.saturation > 1.0f);
  CHECK(s.sharpness > 1.0f);
}

TEST(settings_picture_style_monochrome_zero_saturation) {
  CameraSettings s;
  s.pictureStyle = PictureStyle::Monochrome;
  auto p = pictureStyleParams(PictureStyle::Monochrome);
  CHECK(p.saturation == 0.0f);
}

TEST(settings_picture_style_standard_neutral) {
  auto p = pictureStyleParams(PictureStyle::Standard);
  CHECK(p.brightness == 0.0f);
  CHECK(p.contrast == 1.0f);
  CHECK(p.saturation == 1.0f);
  CHECK(p.sharpness == 1.0f);
}

TEST(settings_value_picture_style_default) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Color, 11, s) == "STD");
}

// --- Focus magnify tests ---

TEST(settings_adjust_focus_magnify_cycles) {
  CameraSettings s;
  CHECK(s.focusMagnify == 0);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 1, s);
  CHECK(s.focusMagnify == 2);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 1, s);
  CHECK(s.focusMagnify == 4);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 1, s);
  CHECK(s.focusMagnify == 0);
}

TEST(settings_value_focus_magnify_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 1, s) == "OFF");
}

TEST(settings_value_focus_magnify_2x) {
  CameraSettings s;
  s.focusMagnify = 2;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 1, s) == "2X");
}

// --- Aspect ratio crop tests ---

TEST(aspect_ratio_crop_native_no_crop) {
  auto r = aspectRatioCrop(4056, 3040, AspectRatio::Native);
  CHECK(r.w == 4056);
  CHECK(r.h == 3040);
  CHECK(r.x == 0);
  CHECK(r.y == 0);
}

TEST(aspect_ratio_crop_169_crops_height) {
  auto r = aspectRatioCrop(4056, 3040, AspectRatio::Ratio169);
  CHECK(r.w == 4056);
  // 4056 * 9/16 = 2281.5 -> 2280 (even)
  CHECK(r.h == 2280);
  CHECK(r.x == 0);
  CHECK((r.y & 1) == 0); // even
}

TEST(aspect_ratio_crop_11_crops_width) {
  auto r = aspectRatioCrop(4056, 3040, AspectRatio::Ratio11);
  CHECK(r.h == 3040);
  CHECK(r.w == 3040);
  CHECK((r.x & 1) == 0); // even
  CHECK(r.y == 0);
}

TEST(aspect_ratio_crop_43_near_native) {
  auto r = aspectRatioCrop(4056, 3040, AspectRatio::Ratio43);
  // 4056/3040 = 1.334, very close to 4:3 (1.333)
  // Should crop width slightly: 3040 * 4/3 = 4053.33 -> 4052
  CHECK(r.h == 3040);
  CHECK(r.w <= 4056);
}

// --- Settings save/load with new fields ---

TEST(settings_save_load_new_fields_roundtrip) {
  CameraSettings s;
  s.exposureMode = ExposureMode::Manual;
  s.bracketType = BracketType::WB;
  s.pictureStyle = PictureStyle::Vivid;

  std::string path = picamera::test::tmpPath(".conf");
  CHECK(saveSettings(s, path));

  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.exposureMode == ExposureMode::Manual);
  CHECK(loaded.bracketType == BracketType::WB);
  CHECK(loaded.pictureStyle == PictureStyle::Vivid);
  std::remove(path.c_str());
}

// --- Picture style load re-applies B/C/S/Sharp ---

TEST(settings_load_picture_style_reapplies_bcsharp) {
  // Save with Vivid style but manually tweaked B/C/S/Sharp values.
  // On load, the picture style defaults should override the tweaks.
  CameraSettings s;
  s.pictureStyle = PictureStyle::Vivid;
  s.brightness = 0.5f; // tweaked away from Vivid default (0.0)
  s.contrast = 0.5f;   // tweaked away from Vivid default (1.2)
  s.saturation = 0.5f; // tweaked away from Vivid default (1.3)
  s.sharpness = 0.5f;  // tweaked away from Vivid default (1.2)

  std::string path = picamera::test::tmpPath(".conf");
  CHECK(saveSettings(s, path));

  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.pictureStyle == PictureStyle::Vivid);
  // Load re-applies the style preset, discarding the tweaks.
  auto p = pictureStyleParams(PictureStyle::Vivid);
  CHECK(loaded.brightness == p.brightness);
  CHECK(loaded.contrast == p.contrast);
  CHECK(loaded.saturation == p.saturation);
  CHECK(loaded.sharpness == p.sharpness);
  std::remove(path.c_str());
}

TEST(settings_load_picture_style_standard_resets_bcsharp) {
  // Even if saved values are non-default, Standard style resets to neutral.
  CameraSettings s;
  s.pictureStyle = PictureStyle::Standard;
  s.brightness = 0.3f;
  s.contrast = 1.5f;
  s.saturation = 1.8f;
  s.sharpness = 3.0f;

  std::string path = picamera::test::tmpPath(".conf");
  CHECK(saveSettings(s, path));

  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.pictureStyle == PictureStyle::Standard);
  CHECK(loaded.brightness == 0.0f);
  CHECK(loaded.contrast == 1.0f);
  CHECK(loaded.saturation == 1.0f);
  CHECK(loaded.sharpness == 1.0f);
  std::remove(path.c_str());
}

// --- WB bracket formula: R and B shift in opposite directions ---

TEST(wb_bracket_formula_opposite_rb_shift) {
  // The WB bracket formula in preview.cpp shifts R and B in opposite
  // directions to vary color temperature (not intensity).
  // Formula: wbRed = 1.0 + ev * 0.2, wbBlue = 1.0 - ev * 0.2
  // Positive EV -> warmer (more red, less blue).
  // Negative EV -> cooler (less red, more blue).
  // Zero EV -> neutral (1.0, 1.0).
  float ev = 0.5f;
  float wbRed = 1.0f + ev * 0.2f;
  float wbBlue = 1.0f - ev * 0.2f;
  CHECK(wbRed > 1.0f);    // positive EV -> more red
  CHECK(wbBlue < 1.0f);   // positive EV -> less blue
  CHECK(wbRed != wbBlue); // opposite directions

  ev = -0.5f;
  wbRed = 1.0f + ev * 0.2f;
  wbBlue = 1.0f - ev * 0.2f;
  CHECK(wbRed < 1.0f);    // negative EV -> less red
  CHECK(wbBlue > 1.0f);   // negative EV -> more blue
  CHECK(wbRed != wbBlue); // opposite directions

  ev = 0.0f;
  wbRed = 1.0f + ev * 0.2f;
  wbBlue = 1.0f - ev * 0.2f;
  CHECK(wbRed == 1.0f); // neutral
  CHECK(wbBlue == 1.0f);
}

// --- WBSET (one-touch custom white balance) tests ---

TEST(settings_label_image_wbset) {
  CHECK(settingsItemLabel(SettingsTab::Color, 5) == "WBSET");
}

TEST(settings_value_wbset_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Color, 5, s) == "OFF");
}

TEST(settings_adjust_wbset_arms_measure) {
  CameraSettings s;
  CHECK(s.wbMeasurePending == false);
  settingsItemAdjustRight(SettingsTab::Color, 5, s);
  CHECK(s.wbMeasurePending == true);
  CHECK(s.awbEnable == false); // switches to manual gain mode
}

TEST(settings_value_wbset_shows_set_when_pending) {
  CameraSettings s;
  s.wbMeasurePending = true;
  CHECK(settingsItemValue(SettingsTab::Color, 5, s) == "SET");
}

TEST(settings_adjust_wbset_left_also_arms) {
  // Either direction arms the measure (it's a one-shot action).
  CameraSettings s;
  settingsItemAdjustLeft(SettingsTab::Color, 5, s);
  CHECK(s.wbMeasurePending == true);
}

// --- ISO MIN/MAX (auto-ISO range) tests ---

TEST(settings_value_iso_min_default_100) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 3, s) == "100");
}

TEST(settings_value_iso_max_default_3200) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 4, s) == "3200");
}

TEST(settings_adjust_iso_min_increases) {
  CameraSettings s;
  CHECK(s.isoMin == 100);
  settingsItemAdjustRight(SettingsTab::Exposure, 3, s);
  CHECK(s.isoMin == 200);
}

TEST(settings_adjust_iso_min_decreases) {
  CameraSettings s;
  s.isoMin = 800;
  settingsItemAdjustLeft(SettingsTab::Exposure, 3, s);
  CHECK(s.isoMin == 400);
}

TEST(settings_adjust_iso_max_increases) {
  CameraSettings s;
  s.isoMax = 1600;
  settingsItemAdjustRight(SettingsTab::Exposure, 4, s);
  CHECK(s.isoMax == 3200);
}

TEST(settings_adjust_iso_min_clamps_to_max) {
  // If ISO MIN is raised above ISO MAX, MAX should follow.
  CameraSettings s;
  s.isoMin = 100;
  s.isoMax = 200;
  settingsItemAdjustRight(SettingsTab::Exposure, 3, s); // 100 -> 200
  settingsItemAdjustRight(SettingsTab::Exposure, 3, s); // 200 -> 400
  CHECK(s.isoMin == 400);
  CHECK(s.isoMax == 400);
}

TEST(settings_adjust_iso_max_clamps_to_min) {
  // If ISO MAX is lowered below ISO MIN, MIN should follow.
  CameraSettings s;
  s.isoMin = 800;
  s.isoMax = 1600;
  settingsItemAdjustLeft(SettingsTab::Exposure, 4, s); // 1600 -> 800
  settingsItemAdjustLeft(SettingsTab::Exposure, 4, s); // 800 -> 400
  CHECK(s.isoMax == 400);
  CHECK(s.isoMin == 400);
}

// --- DngJpeg format test ---

TEST(settings_format_dngjpeg_label) {
  CameraSettings s;
  s.captureFormat = OutputFormat::DngJpeg;
  CHECK(settingsItemValue(SettingsTab::Capture, 0, s) == "DNG+JPG");
}

// --- AEMODE (AeExposureMode cycling: Normal/Short/Long) ---

TEST(settings_adjust_aemode_cycles_right) {
  CameraSettings s;
  CHECK(s.aeExposureMode == AeExposureMode::Normal);
  settingsItemAdjustRight(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Short);
  settingsItemAdjustRight(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Long);
  // wrap back to Normal
  settingsItemAdjustRight(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Normal);
}

TEST(settings_adjust_aemode_cycles_left) {
  CameraSettings s;
  CHECK(s.aeExposureMode == AeExposureMode::Normal);
  // Normal -> Long (wrap backward)
  settingsItemAdjustLeft(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Long);
  // Long -> Short
  settingsItemAdjustLeft(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Short);
  // Short -> Normal
  settingsItemAdjustLeft(SettingsTab::Exposure, 8, s);
  CHECK(s.aeExposureMode == AeExposureMode::Normal);
}

TEST(settings_value_aemode_default_normal) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 8, s) == "NORMAL");
}

TEST(settings_value_aemode_short) {
  CameraSettings s;
  s.aeExposureMode = AeExposureMode::Short;
  CHECK(settingsItemValue(SettingsTab::Exposure, 8, s) == "SHORT");
}

TEST(settings_value_aemode_long) {
  CameraSettings s;
  s.aeExposureMode = AeExposureMode::Long;
  CHECK(settingsItemValue(SettingsTab::Exposure, 8, s) == "LONG");
}

// --- AECONST (AeConstraintMode cycling: Normal/Highlight/Shadows) ---

TEST(settings_adjust_aeconst_cycles_right) {
  CameraSettings s;
  CHECK(s.aeConstraintMode == AeConstraintMode::Normal);
  settingsItemAdjustRight(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Highlight);
  settingsItemAdjustRight(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Shadows);
  // wrap back to Normal
  settingsItemAdjustRight(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Normal);
}

TEST(settings_adjust_aeconst_cycles_left) {
  CameraSettings s;
  CHECK(s.aeConstraintMode == AeConstraintMode::Normal);
  // Normal -> Shadows (wrap backward)
  settingsItemAdjustLeft(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Shadows);
  // Shadows -> Highlight
  settingsItemAdjustLeft(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Highlight);
  // Highlight -> Normal
  settingsItemAdjustLeft(SettingsTab::Exposure, 9, s);
  CHECK(s.aeConstraintMode == AeConstraintMode::Normal);
}

TEST(settings_value_aeconst_default_normal) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Exposure, 9, s) == "NORMAL");
}

TEST(settings_value_aeconst_highlight) {
  CameraSettings s;
  s.aeConstraintMode = AeConstraintMode::Highlight;
  CHECK(settingsItemValue(SettingsTab::Exposure, 9, s) == "HIGHLIGHT");
}

TEST(settings_value_aeconst_shadows) {
  CameraSettings s;
  s.aeConstraintMode = AeConstraintMode::Shadows;
  CHECK(settingsItemValue(SettingsTab::Exposure, 9, s) == "SHADOWS");
}

// --- INTERVAL (timelapseInterval adjustment: 1-3600s) ---

TEST(settings_adjust_interval_increases) {
  CameraSettings s;
  CHECK(s.timelapseInterval == 5);
  settingsItemAdjustRight(SettingsTab::Capture, 11, s);
  CHECK(s.timelapseInterval == 6);
}

TEST(settings_adjust_interval_decreases) {
  CameraSettings s;
  CHECK(s.timelapseInterval == 5);
  settingsItemAdjustLeft(SettingsTab::Capture, 11, s);
  CHECK(s.timelapseInterval == 4);
}

TEST(settings_adjust_interval_clamps_min_1) {
  CameraSettings s;
  s.timelapseInterval = 1;
  settingsItemAdjustLeft(SettingsTab::Capture, 11, s);
  CHECK(s.timelapseInterval == 1); // clamped
}

TEST(settings_adjust_interval_clamps_max_3600) {
  CameraSettings s;
  s.timelapseInterval = 3600;
  settingsItemAdjustRight(SettingsTab::Capture, 11, s);
  CHECK(s.timelapseInterval == 3600); // clamped
}

TEST(settings_value_interval_default) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 11, s) == "5S");
}

TEST(settings_value_interval_custom) {
  CameraSettings s;
  s.timelapseInterval = 60;
  CHECK(settingsItemValue(SettingsTab::Capture, 11, s) == "60S");
}

// --- COUNT (timelapseCount adjustment: 0-999, 0=infinite) ---

TEST(settings_adjust_count_increases) {
  CameraSettings s;
  CHECK(s.timelapseCount == 10);
  settingsItemAdjustRight(SettingsTab::Capture, 12, s);
  CHECK(s.timelapseCount == 11);
}

TEST(settings_adjust_count_decreases) {
  CameraSettings s;
  CHECK(s.timelapseCount == 10);
  settingsItemAdjustLeft(SettingsTab::Capture, 12, s);
  CHECK(s.timelapseCount == 9);
}

TEST(settings_adjust_count_clamps_min_0) {
  CameraSettings s;
  s.timelapseCount = 0;
  settingsItemAdjustLeft(SettingsTab::Capture, 12, s);
  CHECK(s.timelapseCount == 0); // clamped
}

TEST(settings_adjust_count_clamps_max_999) {
  CameraSettings s;
  s.timelapseCount = 999;
  settingsItemAdjustRight(SettingsTab::Capture, 12, s);
  CHECK(s.timelapseCount == 999); // clamped
}

TEST(settings_value_count_default) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Capture, 12, s) == "10");
}

TEST(settings_value_count_zero_shows_inf) {
  CameraSettings s;
  s.timelapseCount = 0;
  CHECK(settingsItemValue(SettingsTab::Capture, 12, s) == "INF");
}

// --- NR (NoiseReductionMode cycling: Off/Fast/HQ/Minimal) ---

TEST(settings_adjust_nr_cycles_right) {
  CameraSettings s;
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
  settingsItemAdjustRight(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::HighQuality);
  settingsItemAdjustRight(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Minimal);
  settingsItemAdjustRight(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Off);
  // wrap back to Fast
  settingsItemAdjustRight(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
}

TEST(settings_adjust_nr_cycles_left) {
  CameraSettings s;
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
  // Fast -> Off (wrap backward)
  settingsItemAdjustLeft(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Off);
  // Off -> Minimal
  settingsItemAdjustLeft(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Minimal);
  // Minimal -> HighQuality
  settingsItemAdjustLeft(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::HighQuality);
  // HighQuality -> Fast
  settingsItemAdjustLeft(SettingsTab::Color, 10, s);
  CHECK(s.noiseReduction == NoiseReductionMode::Fast);
}

TEST(settings_value_nr_default_fast) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Color, 10, s) == "FAST");
}

TEST(settings_value_nr_off) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::Off;
  CHECK(settingsItemValue(SettingsTab::Color, 10, s) == "OFF");
}

TEST(settings_value_nr_hq) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::HighQuality;
  CHECK(settingsItemValue(SettingsTab::Color, 10, s) == "HQ");
}

TEST(settings_value_nr_minimal) {
  CameraSettings s;
  s.noiseReduction = NoiseReductionMode::Minimal;
  CHECK(settingsItemValue(SettingsTab::Color, 10, s) == "MIN");
}

// --- PWRSAVE (powerSaveTimeout cycling: 0/30/60/300s) ---

TEST(settings_adjust_pwrsave_cycles_right) {
  CameraSettings s;
  CHECK(s.powerSaveTimeout == 30);
  // 30 -> 60
  settingsItemAdjustRight(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 60);
  // 60 -> 300
  settingsItemAdjustRight(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 300);
  // 300 -> 0 (off)
  settingsItemAdjustRight(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 0);
  // 0 -> 30 (wrap)
  settingsItemAdjustRight(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 30);
}

TEST(settings_adjust_pwrsave_cycles_left) {
  CameraSettings s;
  CHECK(s.powerSaveTimeout == 30);
  // 30 -> 0 (off)
  settingsItemAdjustLeft(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 0);
  // 0 -> 300
  settingsItemAdjustLeft(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 300);
  // 300 -> 60
  settingsItemAdjustLeft(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 60);
  // 60 -> 30 (wrap)
  settingsItemAdjustLeft(SettingsTab::System, 1, s);
  CHECK(s.powerSaveTimeout == 30);
}

TEST(settings_value_pwrsave_default_30s) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::System, 1, s) == "30S");
}

TEST(settings_value_pwrsave_zero_shows_off) {
  CameraSettings s;
  s.powerSaveTimeout = 0;
  CHECK(settingsItemValue(SettingsTab::System, 1, s) == "OFF");
}

TEST(settings_value_pwrsave_300s) {
  CameraSettings s;
  s.powerSaveTimeout = 300;
  CHECK(settingsItemValue(SettingsTab::System, 1, s) == "300S");
}

// --- HIST (showHistogram toggle: on/off) ---

TEST(settings_adjust_hist_toggles) {
  CameraSettings s;
  CHECK(s.showHistogram == false);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 3, s);
  CHECK(s.showHistogram == true);
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 3, s);
  CHECK(s.showHistogram == false);
}

TEST(settings_value_hist_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 3, s) == "OFF");
}

TEST(settings_value_hist_on) {
  CameraSettings s;
  s.showHistogram = true;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 3, s) == "ON");
}

// --- ZEBRA (ZebraMode cycling: Off/70%/80%/100%) ---

TEST(settings_adjust_zebra_cycles_right) {
  CameraSettings s;
  CHECK(s.zebraMode == ZebraMode::Off);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold70);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold80);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold100);
  // wrap back to Off
  settingsItemAdjustRight(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Off);
}

TEST(settings_adjust_zebra_cycles_left) {
  CameraSettings s;
  CHECK(s.zebraMode == ZebraMode::Off);
  // Off -> 100% (wrap backward)
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold100);
  // 100% -> 80%
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold80);
  // 80% -> 70%
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Threshold70);
  // 70% -> Off
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 4, s);
  CHECK(s.zebraMode == ZebraMode::Off);
}

TEST(settings_value_zebra_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 4, s) == "OFF");
}

TEST(settings_value_zebra_70) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Threshold70;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 4, s) == "70%");
}

TEST(settings_value_zebra_80) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Threshold80;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 4, s) == "80%");
}

TEST(settings_value_zebra_100) {
  CameraSettings s;
  s.zebraMode = ZebraMode::Threshold100;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 4, s) == "100%");
}

// --- PEAK (focusPeaking toggle: on/off) ---

TEST(settings_adjust_peak_toggles) {
  CameraSettings s;
  CHECK(s.focusPeaking == false);
  settingsItemAdjustRight(SettingsTab::FocusDisp, 0, s);
  CHECK(s.focusPeaking == true);
  settingsItemAdjustLeft(SettingsTab::FocusDisp, 0, s);
  CHECK(s.focusPeaking == false);
}

TEST(settings_value_peak_default_off) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 0, s) == "OFF");
}

TEST(settings_value_peak_on) {
  CameraSettings s;
  s.focusPeaking = true;
  CHECK(settingsItemValue(SettingsTab::FocusDisp, 0, s) == "ON");
}

// --- Sensor mode mapping tests ---

TEST(sensor_mode_dims_auto_returns_zero) {
  auto d = sensorModeDims(SensorMode::Auto);
  CHECK(d.width == 0);
  CHECK(d.height == 0);
}

TEST(sensor_mode_dims_1332x990) {
  auto d = sensorModeDims(SensorMode::Mode1332x990);
  CHECK(d.width == 1332);
  CHECK(d.height == 990);
}

TEST(sensor_mode_dims_2028x1080) {
  auto d = sensorModeDims(SensorMode::Mode2028x1080);
  CHECK(d.width == 2028);
  CHECK(d.height == 1080);
}

TEST(sensor_mode_dims_2028x1520) {
  auto d = sensorModeDims(SensorMode::Mode2028x1520);
  CHECK(d.width == 2028);
  CHECK(d.height == 1520);
}

TEST(sensor_mode_dims_4056x3040) {
  auto d = sensorModeDims(SensorMode::Mode4056x3040);
  CHECK(d.width == 4056);
  CHECK(d.height == 3040);
}

TEST(sensor_mode_label_shows_resolution) {
  CHECK(settingsItemValue(SettingsTab::Capture, 5, CameraSettings{}) ==
        "AUTO");
  CameraSettings s;
  s.sensorMode = SensorMode::Mode2028x1080;
  CHECK(settingsItemValue(SettingsTab::Capture, 5, s) == "2028x1080");
}

TEST(sensor_mode_adjust_cycles) {
  CameraSettings s;
  CHECK(s.sensorMode == SensorMode::Auto);
  settingsItemAdjustRight(SettingsTab::Capture, 5, s);
  CHECK(s.sensorMode == SensorMode::Mode1332x990);
  settingsItemAdjustRight(SettingsTab::Capture, 5, s);
  CHECK(s.sensorMode == SensorMode::Mode2028x1080);
  settingsItemAdjustRight(SettingsTab::Capture, 5, s);
  CHECK(s.sensorMode == SensorMode::Mode2028x1520);
  settingsItemAdjustRight(SettingsTab::Capture, 5, s);
  CHECK(s.sensorMode == SensorMode::Mode4056x3040);
  settingsItemAdjustRight(SettingsTab::Capture, 5, s);
  CHECK(s.sensorMode == SensorMode::Auto); // wraps around
}

TEST(settings_needs_reconfigure_on_sensor_mode_change) {
  CameraSettings before, after;
  after.sensorMode = SensorMode::Mode2028x1080;
  CHECK(settingsNeedsReconfigure(before, after));
}

// --- Video resolution mapping tests ---

TEST(video_resolution_dims_320x240) {
  auto d = videoResolutionDims(VideoResolution::Res320x240);
  CHECK(d.width == 320);
  CHECK(d.height == 240);
}

TEST(video_resolution_dims_640x480) {
  auto d = videoResolutionDims(VideoResolution::Res640x480);
  CHECK(d.width == 640);
  CHECK(d.height == 480);
}

TEST(video_resolution_dims_1280x720) {
  auto d = videoResolutionDims(VideoResolution::Res1280x720);
  CHECK(d.width == 1280);
  CHECK(d.height == 720);
}

TEST(video_resolution_dims_1920x1080) {
  auto d = videoResolutionDims(VideoResolution::Res1920x1080);
  CHECK(d.width == 1920);
  CHECK(d.height == 1080);
}

TEST(video_resolution_label) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Video, 1, s) == "320x240");
  s.videoResolution = VideoResolution::Res1280x720;
  CHECK(settingsItemValue(SettingsTab::Video, 1, s) == "720P");
}

TEST(video_resolution_adjust_cycles) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Video, 1, s);
  CHECK(s.videoResolution == VideoResolution::Res640x480);
  settingsItemAdjustRight(SettingsTab::Video, 1, s);
  CHECK(s.videoResolution == VideoResolution::Res1280x720);
  settingsItemAdjustRight(SettingsTab::Video, 1, s);
  CHECK(s.videoResolution == VideoResolution::Res1920x1080);
  settingsItemAdjustRight(SettingsTab::Video, 1, s);
  CHECK(s.videoResolution == VideoResolution::Res320x240); // wraps
}

// --- Video FPS tests ---

TEST(video_fps_label) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Video, 2, s) == "30FPS");
}

TEST(video_fps_adjust_cycles) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Video, 2, s);
  CHECK(s.videoFps == 50);
  settingsItemAdjustRight(SettingsTab::Video, 2, s);
  CHECK(s.videoFps == 60);
  settingsItemAdjustRight(SettingsTab::Video, 2, s);
  CHECK(s.videoFps == 10); // wraps
  settingsItemAdjustRight(SettingsTab::Video, 2, s);
  CHECK(s.videoFps == 24);
}

// --- Video codec tests ---

TEST(video_codec_label) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Video, 3, s) == "MJPEG");
  s.videoCodec = VideoCodec::H264;
  // H264 falls back to MJPEG encoding (no HW H264 encoder on Pi Zero 2 W);
  // the menu shows "MJPEG*" to indicate the fallback.
  CHECK(settingsItemValue(SettingsTab::Video, 3, s) == "MJPEG*");
  s.videoCodec = VideoCodec::YUV;
  CHECK(settingsItemValue(SettingsTab::Video, 3, s) == "YUV");
}

TEST(video_codec_adjust_cycles) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Video, 3, s);
  CHECK(s.videoCodec == VideoCodec::H264);
  settingsItemAdjustRight(SettingsTab::Video, 3, s);
  CHECK(s.videoCodec == VideoCodec::YUV);
  settingsItemAdjustRight(SettingsTab::Video, 3, s);
  CHECK(s.videoCodec == VideoCodec::MJPEG); // wraps
}

// --- Video bitrate tests ---

TEST(video_bitrate_label) {
  CameraSettings s;
  CHECK(settingsItemValue(SettingsTab::Video, 4, s) == "5MB");
}

TEST(video_bitrate_adjust_cycles) {
  CameraSettings s;
  settingsItemAdjustRight(SettingsTab::Video, 4, s);
  CHECK(s.videoBitrate == 10);
  settingsItemAdjustRight(SettingsTab::Video, 4, s);
  CHECK(s.videoBitrate == 20);
  settingsItemAdjustRight(SettingsTab::Video, 4, s);
  CHECK(s.videoBitrate == 1); // wraps
  settingsItemAdjustRight(SettingsTab::Video, 4, s);
  CHECK(s.videoBitrate == 5);
}

// --- Settings persistence tests for new fields ---

TEST(settings_persistence_video_and_sensor_roundtrip) {
  CameraSettings s;
  s.videoResolution = VideoResolution::Res1280x720;
  s.videoFps = 50;
  s.videoCodec = VideoCodec::H264;
  s.videoBitrate = 20;
  s.sensorMode = SensorMode::Mode2028x1520;
  std::string path = "/tmp/test_settings_vs_" +
                     std::to_string(getpid()) + ".conf";
  CHECK(saveSettings(s, path));
  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.videoResolution == VideoResolution::Res1280x720);
  CHECK(loaded.videoFps == 50);
  CHECK(loaded.videoCodec == VideoCodec::H264);
  CHECK(loaded.videoBitrate == 20);
  CHECK(loaded.sensorMode == SensorMode::Mode2028x1520);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// --- Basic menu tests ---

TEST(basic_menu_item_count_is_14) {
  CHECK(basicMenuItemCount() == 14);
}

TEST(basic_menu_label_expmode) {
  CHECK(basicMenuItemLabel(0) == "EXPMODE");
}

TEST(basic_menu_label_iso) {
  CHECK(basicMenuItemLabel(1) == "ISO");
}

TEST(basic_menu_label_shutter) {
  CHECK(basicMenuItemLabel(2) == "SHUTTER");
}

TEST(basic_menu_label_ev) {
  CHECK(basicMenuItemLabel(3) == "EV");
}

TEST(basic_menu_label_awb) {
  CHECK(basicMenuItemLabel(4) == "AWB");
}

TEST(basic_menu_label_drive) {
  CHECK(basicMenuItemLabel(5) == "DRIVE");
}

TEST(basic_menu_label_timer) {
  CHECK(basicMenuItemLabel(6) == "TIMER");
}

TEST(basic_menu_label_format) {
  CHECK(basicMenuItemLabel(7) == "FORMAT");
}

TEST(basic_menu_label_size) {
  CHECK(basicMenuItemLabel(8) == "SIZE");
}

TEST(basic_menu_label_aspect) {
  CHECK(basicMenuItemLabel(9) == "ASPECT");
}

TEST(basic_menu_label_pstyle) {
  CHECK(basicMenuItemLabel(10) == "PSTYLE");
}

TEST(basic_menu_label_battery) {
  CHECK(basicMenuItemLabel(11) == "BATTERY");
}

TEST(basic_menu_label_format_card) {
  CHECK(basicMenuItemLabel(12) == "FMT CARD");
}

TEST(basic_menu_label_advanced) {
  CHECK(basicMenuItemLabel(13) == "ADVANCED");
}

TEST(basic_menu_value_expmode_default) {
  CameraSettings s;
  CHECK(basicMenuItemValue(0, s) == "P");
}

TEST(basic_menu_value_iso_default_auto) {
  CameraSettings s;
  CHECK(basicMenuItemValue(1, s) == "AUTO");
}

TEST(basic_menu_value_format_default_jpeg) {
  CameraSettings s;
  CHECK(basicMenuItemValue(7, s) == "JPEG");
}

TEST(basic_menu_value_format_card_empty) {
  CameraSettings s;
  CHECK(basicMenuItemValue(12, s) == "");
}

TEST(basic_menu_value_advanced_empty) {
  CameraSettings s;
  CHECK(basicMenuItemValue(13, s) == "");
}

TEST(basic_menu_adjust_iso_increases) {
  CameraSettings s;
  CHECK(s.analogueGain == 0);
  basicMenuItemAdjustRight(1, s);
  CHECK(s.analogueGain > 0);
}

TEST(basic_menu_adjust_ev_increases) {
  CameraSettings s;
  CHECK(s.exposureValue == 0);
  basicMenuItemAdjustRight(3, s);
  CHECK(s.exposureValue > 0);
}

TEST(basic_menu_adjust_format_cycles) {
  CameraSettings s;
  CHECK(s.captureFormat == OutputFormat::JPEG);
  basicMenuItemAdjustRight(7, s);
  CHECK(s.captureFormat == OutputFormat::DNG);
}

TEST(basic_menu_is_format_card_idx_12) {
  CHECK(basicMenuItemIsFormatCard(12));
  CHECK(!basicMenuItemIsFormatCard(11));
}

TEST(basic_menu_is_advanced_toggle_idx_13) {
  CHECK(basicMenuItemIsAdvancedToggle(13));
  CHECK(!basicMenuItemIsAdvancedToggle(0));
}

// --- Menu mode tests ---

TEST(menu_mode_default_is_basic) {
  CameraSettings s;
  CHECK(s.menuMode == MenuMode::Basic);
}

TEST(menu_mode_persistence_roundtrip) {
  CameraSettings s;
  s.menuMode = MenuMode::Advanced;
  std::string path = "/tmp/test_menu_mode_" +
                     std::to_string(getpid()) + ".conf";
  CHECK(saveSettings(s, path));
  CameraSettings loaded;
  CHECK(loadSettings(loaded, path));
  CHECK(loaded.menuMode == MenuMode::Advanced);
  std::remove(path.c_str());
}

// --- Advanced menu special-item predicate tests ---

using picamera::advancedMenuItemIsAirplane;
using picamera::advancedMenuItemIsBasicToggle;
using picamera::advancedMenuItemIsCopyright;
using picamera::advancedMenuItemIsCustomMode;
using picamera::advancedMenuItemIsDate;
using picamera::advancedMenuItemIsExit;
using picamera::advancedMenuItemIsFormatCard;
using picamera::advancedMenuItemIsReset;
using picamera::advancedMenuItemIsVideo;

TEST(advanced_is_basic_toggle_system_idx_10) {
  CHECK(advancedMenuItemIsBasicToggle(SettingsTab::System, 10));
  CHECK(!advancedMenuItemIsBasicToggle(SettingsTab::System, 0));
}

TEST(advanced_is_format_card_system_idx_2) {
  CHECK(advancedMenuItemIsFormatCard(SettingsTab::System, 2));
  CHECK(!advancedMenuItemIsFormatCard(SettingsTab::Capture, 0));
}

TEST(advanced_is_reset_system_idx_3) {
  CHECK(advancedMenuItemIsReset(SettingsTab::System, 3));
}

TEST(advanced_is_date_system_idx_5) {
  CHECK(advancedMenuItemIsDate(SettingsTab::System, 5));
}

TEST(advanced_is_video_video_idx_0) {
  CHECK(advancedMenuItemIsVideo(SettingsTab::Video, 0));
}

TEST(advanced_is_copyright_system_idx_8) {
  CHECK(advancedMenuItemIsCopyright(SettingsTab::System, 8));
}

TEST(advanced_is_exit_system_idx_11) {
  CHECK(advancedMenuItemIsExit(SettingsTab::System, 11));
}

TEST(advanced_is_airplane_system_idx_4) {
  CHECK(advancedMenuItemIsAirplane(SettingsTab::System, 4));
}

TEST(advanced_is_custom_mode_system_idx_9) {
  CHECK(advancedMenuItemIsCustomMode(SettingsTab::System, 9));
}

// --- All features reachable in Advanced mode ---

TEST(advanced_mode_all_features_reachable) {
  // Total items across all 6 Advanced tabs:
  // 13+14+14+8+5+12 = 66 menu items (including EXIT and BASIC toggle).
  int total = 0;
  for (int t = 0; t < 6; ++t) {
    auto tab = static_cast<SettingsTab>(t);
    total += settingsTabItemCount(tab);
  }
  CHECK(total == 66);
}
