#include "camera_mode.h"
#include "settings_menu.h"
#include "test_runner.h"

#include <cmath>
#include <string>

using picamera::SettingsTab;
using picamera::settingsTabItemCount;
using picamera::settingsItemLabel;
using picamera::settingsItemValue;
using picamera::CameraSettings;
using picamera::settingsItemAdjustRight;
using picamera::settingsItemAdjustLeft;
using picamera::DriveMode;
using picamera::MeteringMode;
using picamera::OutputFormat;
using picamera::GridType;
using picamera::AspectRatio;
using picamera::settingsNeedsReconfigure;
using picamera::settingsToCameraConfig;
using picamera::CameraConfig;

// --- Item count tests ---

TEST(settings_tab_shooting_has_12_items) {
    CHECK(settingsTabItemCount(SettingsTab::Shooting) == 12);
}

TEST(settings_tab_image_has_12_items) {
    CHECK(settingsTabItemCount(SettingsTab::Image) == 12);
}

TEST(settings_tab_display_has_5_items) {
    CHECK(settingsTabItemCount(SettingsTab::Display) == 5);
}

TEST(settings_tab_system_has_3_items) {
    CHECK(settingsTabItemCount(SettingsTab::System) == 3);
}

// --- Label tests ---

TEST(settings_label_shooting_drive) {
    CHECK(settingsItemLabel(SettingsTab::Shooting, 0) == "DRIVE");
}

TEST(settings_label_shooting_shutter) {
    CHECK(settingsItemLabel(SettingsTab::Shooting, 1) == "SHUTTER");
}

TEST(settings_label_image_format) {
    CHECK(settingsItemLabel(SettingsTab::Image, 0) == "FORMAT");
}

TEST(settings_label_display_grid) {
    CHECK(settingsItemLabel(SettingsTab::Display, 0) == "GRID");
}

TEST(settings_label_system_exit) {
    CHECK(settingsItemLabel(SettingsTab::System, 2) == "EXIT");
}

TEST(settings_label_out_of_range_returns_question) {
    CHECK(settingsItemLabel(SettingsTab::Shooting, 99) == "??");
}

// --- Value tests ---

TEST(settings_value_drive_default_single) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Shooting, 0, s) == "SINGLE");
}

TEST(settings_value_shutter_default_auto) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Shooting, 1, s) == "AUTO");
}

TEST(settings_value_iso_default_auto) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Shooting, 2, s) == "AUTO");
}

TEST(settings_value_ev_default_zero) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Shooting, 3, s) == "0");
}

TEST(settings_value_format_default_jpeg) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Image, 0, s) == "JPEG");
}

TEST(settings_value_grid_default_off) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::Display, 0, s) == "OFF");
}

TEST(settings_value_battery_default_on) {
    CameraSettings s;
    CHECK(settingsItemValue(SettingsTab::System, 0, s) == "ON");
}

// --- Adjust tests ---

TEST(settings_adjust_drive_mode_cycles) {
    CameraSettings s;
    // Single -> SelfTimer
    settingsItemAdjustRight(SettingsTab::Shooting, 0, s);
    CHECK(s.driveMode == DriveMode::SelfTimer);
    // SelfTimer -> Bracket
    settingsItemAdjustRight(SettingsTab::Shooting, 0, s);
    CHECK(s.driveMode == DriveMode::Bracket);
    // Bracket -> Timelapse
    settingsItemAdjustRight(SettingsTab::Shooting, 0, s);
    CHECK(s.driveMode == DriveMode::Timelapse);
    // Timelapse -> Single (wrap)
    settingsItemAdjustRight(SettingsTab::Shooting, 0, s);
    CHECK(s.driveMode == DriveMode::Single);
}

TEST(settings_adjust_drive_mode_left_cycles) {
    CameraSettings s;
    // Single -> Timelapse (wrap backward)
    settingsItemAdjustLeft(SettingsTab::Shooting, 0, s);
    CHECK(s.driveMode == DriveMode::Timelapse);
}

TEST(settings_adjust_shutter_increases) {
    CameraSettings s;
    CHECK(s.shutterUs == 0);
    settingsItemAdjustRight(SettingsTab::Shooting, 1, s);
    CHECK(s.shutterUs > 0);
    CHECK(s.aeEnable == false); // manual shutter disables AE
}

TEST(settings_adjust_shutter_back_to_auto_reenables_ae) {
    CameraSettings s;
    settingsItemAdjustRight(SettingsTab::Shooting, 1, s);
    CHECK(s.aeEnable == false);
    // Cycle back to AUTO (shutterUs == 0)
    while (s.shutterUs != 0) {
        settingsItemAdjustLeft(SettingsTab::Shooting, 1, s);
    }
    CHECK(s.shutterUs == 0);
    CHECK(s.aeEnable == true); // AE re-enabled
}

TEST(settings_adjust_iso_back_to_auto_reenables_ae) {
    CameraSettings s;
    settingsItemAdjustRight(SettingsTab::Shooting, 2, s);
    CHECK(s.analogueGain > 0);
    CHECK(s.aeEnable == false);
    // Cycle back to AUTO (gain == 0)
    while (s.analogueGain != 0.0f) {
        settingsItemAdjustLeft(SettingsTab::Shooting, 2, s);
    }
    CHECK(s.analogueGain == 0.0f);
    CHECK(s.aeEnable == true);
}

TEST(settings_adjust_shutter_decreases) {
    CameraSettings s;
    s.shutterUs = 8000; // 1/125
    settingsItemAdjustLeft(SettingsTab::Shooting, 1, s);
    CHECK(s.shutterUs < 8000);
}

TEST(settings_adjust_iso_increases) {
    CameraSettings s;
    CHECK(s.analogueGain == 0);
    settingsItemAdjustRight(SettingsTab::Shooting, 2, s);
    CHECK(s.analogueGain > 0);
}

TEST(settings_adjust_ev_increases) {
    CameraSettings s;
    CHECK(s.exposureValue == 0);
    settingsItemAdjustRight(SettingsTab::Shooting, 3, s);
    CHECK(s.exposureValue > 0);
}

TEST(settings_adjust_ev_decreases) {
    CameraSettings s;
    s.exposureValue = 1.0f;
    settingsItemAdjustLeft(SettingsTab::Shooting, 3, s);
    CHECK(s.exposureValue < 1.0f);
}

TEST(settings_adjust_metering_cycles) {
    CameraSettings s;
    CHECK(s.meteringMode == MeteringMode::Matrix);
    settingsItemAdjustRight(SettingsTab::Shooting, 4, s);
    CHECK(s.meteringMode == MeteringMode::Centre);
    settingsItemAdjustRight(SettingsTab::Shooting, 4, s);
    CHECK(s.meteringMode == MeteringMode::Spot);
    settingsItemAdjustRight(SettingsTab::Shooting, 4, s);
    CHECK(s.meteringMode == MeteringMode::Matrix);
}

TEST(settings_adjust_format_cycles_right) {
    CameraSettings s;
    CHECK(s.captureFormat == OutputFormat::JPEG);
    settingsItemAdjustRight(SettingsTab::Image, 0, s);
    CHECK(s.captureFormat == OutputFormat::DNG);
    settingsItemAdjustRight(SettingsTab::Image, 0, s);
    CHECK(s.captureFormat == OutputFormat::PNG);
}

TEST(settings_adjust_format_cycles_left) {
    CameraSettings s;
    CHECK(s.captureFormat == OutputFormat::JPEG);
    settingsItemAdjustLeft(SettingsTab::Image, 0, s);
    CHECK(s.captureFormat == OutputFormat::RAW_NV12);
}

TEST(settings_adjust_quality_increases) {
    CameraSettings s;
    CHECK(s.jpegQuality == 90);
    settingsItemAdjustRight(SettingsTab::Image, 1, s);
    CHECK(s.jpegQuality == 91);
}

TEST(settings_adjust_quality_decreases) {
    CameraSettings s;
    s.jpegQuality = 50;
    settingsItemAdjustLeft(SettingsTab::Image, 1, s);
    CHECK(s.jpegQuality == 49);
}

TEST(settings_adjust_quality_min_10) {
    CameraSettings s;
    s.jpegQuality = 10;
    settingsItemAdjustLeft(SettingsTab::Image, 1, s);
    CHECK(s.jpegQuality == 10); // clamped
}

TEST(settings_adjust_quality_max_100) {
    CameraSettings s;
    s.jpegQuality = 100;
    settingsItemAdjustRight(SettingsTab::Image, 1, s);
    CHECK(s.jpegQuality == 100); // clamped
}

TEST(settings_adjust_grid_cycles) {
    CameraSettings s;
    CHECK(s.gridType == GridType::Off);
    settingsItemAdjustRight(SettingsTab::Display, 0, s);
    CHECK(s.gridType == GridType::Thirds);
    settingsItemAdjustRight(SettingsTab::Display, 0, s);
    CHECK(s.gridType == GridType::Square);
    settingsItemAdjustRight(SettingsTab::Display, 0, s);
    CHECK(s.gridType == GridType::Off);
}

TEST(settings_adjust_brightness_increases) {
    CameraSettings s;
    CHECK(s.displayBrightness == 100);
    // Already at max — should stay
    settingsItemAdjustRight(SettingsTab::Display, 4, s);
    CHECK(s.displayBrightness == 100);
}

TEST(settings_adjust_brightness_decreases) {
    CameraSettings s;
    s.displayBrightness = 50;
    settingsItemAdjustLeft(SettingsTab::Display, 4, s);
    CHECK(s.displayBrightness == 40);
}

TEST(settings_adjust_brightness_min_10) {
    CameraSettings s;
    s.displayBrightness = 10;
    settingsItemAdjustLeft(SettingsTab::Display, 4, s);
    CHECK(s.displayBrightness == 10); // clamped
}

TEST(settings_adjust_awb_cycles_through_modes) {
    CameraSettings s;
    CHECK(s.awbEnable == true);
    CHECK(s.awbMode == "auto");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "daylight");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "cloudy");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "incandescent");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "tungsten");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "fluorescent");
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "indoor");
    // Next step turns AWB off
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbEnable == false);
    // Next step turns it back on with "auto"
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbEnable == true);
    CHECK(s.awbMode == "auto");
}

TEST(settings_adjust_bracket_increases_spread) {
    CameraSettings s;
    CHECK(s.bracketEv.empty());
    settingsItemAdjustRight(SettingsTab::Shooting, 9, s);
    CHECK(s.bracketEv.size() == 3);
    CHECK(s.bracketEv[0] < 0);
    CHECK(s.bracketEv[1] == 0);
    CHECK(s.bracketEv[2] > 0);
}

TEST(settings_adjust_bracket_decreases_to_off) {
    CameraSettings s;
    s.bracketEv = {-0.5f, 0.0f, 0.5f};
    settingsItemAdjustLeft(SettingsTab::Shooting, 9, s);
    CHECK(s.bracketEv.empty());
}

TEST(settings_adjust_flicker_cycles) {
    CameraSettings s;
    CHECK(s.antiFlicker == false);
    settingsItemAdjustRight(SettingsTab::Shooting, 7, s);
    CHECK(s.antiFlicker == true);
    CHECK(s.flickerHz == 50);
    settingsItemAdjustRight(SettingsTab::Shooting, 7, s);
    CHECK(s.flickerHz == 60);
    settingsItemAdjustLeft(SettingsTab::Shooting, 7, s);
    CHECK(s.flickerHz == 50);
    settingsItemAdjustLeft(SettingsTab::Shooting, 7, s);
    CHECK(s.antiFlicker == false);
}

// --- Reconfigure detection ---

TEST(settings_needs_reconfigure_format_change) {
    CameraSettings a, b;
    b.captureFormat = OutputFormat::DNG;
    CHECK(settingsNeedsReconfigure(a, b));
}

TEST(settings_needs_reconfigure_aspect_change) {
    CameraSettings a, b;
    b.aspectRatio = AspectRatio::Ratio169;
    CHECK(settingsNeedsReconfigure(a, b));
}

TEST(settings_no_reconfigure_on_exposure_only) {
    CameraSettings a, b;
    b.exposureValue = 1.0f;
    CHECK(!settingsNeedsReconfigure(a, b));
}

TEST(settings_no_reconfigure_on_same_values) {
    CameraSettings a, b;
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

// --- formatShutter edge cases (tested via settingsItemValue) ---

TEST(settings_value_shutter_near_1s_shows_1s) {
    CameraSettings s;
    s.shutterUs = 999999; // just under 1s — denom would be 1
    CHECK(settingsItemValue(SettingsTab::Shooting, 1, s) == "1\"");
}

TEST(settings_value_shutter_half_second) {
    CameraSettings s;
    s.shutterUs = 500000; // 1/2
    CHECK(settingsItemValue(SettingsTab::Shooting, 1, s) == "1/2");
}

TEST(settings_value_shutter_one_minute) {
    CameraSettings s;
    s.shutterUs = 60000000; // 60s = 1'
    CHECK(settingsItemValue(SettingsTab::Shooting, 1, s) == "1'");
}

// --- Float slider clamp regression tests ---

TEST(settings_adjust_wb_red_clamps_min) {
    CameraSettings s;
    s.wbRedGain = 0.05f; // below min
    settingsItemAdjustLeft(SettingsTab::Image, 5, s);
    CHECK(s.wbRedGain == 0.1f); // clamped to min
}

TEST(settings_adjust_sharpness_clamps_max) {
    CameraSettings s;
    s.sharpness = 15.8f;
    settingsItemAdjustRight(SettingsTab::Image, 10, s);
    CHECK(s.sharpness == 16.0f); // clamped to max
}

TEST(settings_adjust_brightness_clamps_min) {
    CameraSettings s;
    s.brightness = -0.95f;
    settingsItemAdjustLeft(SettingsTab::Image, 7, s);
    CHECK(s.brightness == -1.0f); // clamped to min
}

// --- AWB unknown mode fallback regression test ---

TEST(settings_adjust_awb_unknown_mode_falls_back_to_auto) {
    CameraSettings s;
    s.awbMode = "bogus"; // not a valid mode
    settingsItemAdjustLeft(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "auto"); // fallback
    s.awbMode = "bogus";
    settingsItemAdjustRight(SettingsTab::Image, 3, s);
    CHECK(s.awbMode == "auto"); // fallback
}

// --- Flicker right-cycle wrap to OFF regression test ---

TEST(settings_adjust_flicker_right_wraps_to_off) {
    CameraSettings s;
    s.antiFlicker = true;
    s.flickerHz = 60;
    settingsItemAdjustRight(SettingsTab::Shooting, 7, s);
    CHECK(s.antiFlicker == false); // 60Hz -> OFF
}
