#include "hardware_config.h"
#include "test_runner.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using picamera::HardwareConfig;
using picamera::loadHardwareConfig;

namespace {

std::string writeTempConfig(const std::string &content) {
    char tmpl[] = "/tmp/picamera_test_XXXXXX";
    int fd = mkstemp(tmpl);
    REQUIRE(fd >= 0);
    FILE *f = fdopen(fd, "w");
    REQUIRE(f != nullptr);
    fputs(content.c_str(), f);
    fclose(f);
    return std::string(tmpl);
}

}

TEST(hardware_config_missing_file_returns_false) {
    HardwareConfig cfg;
    CHECK(!loadHardwareConfig("/nonexistent/path/picamera.conf", cfg));
}

TEST(hardware_config_empty_file_returns_true) {
    std::string path = writeTempConfig("");
    HardwareConfig cfg;
    CHECK(loadHardwareConfig(path, cfg));
    std::filesystem::remove(path);
}

TEST(hardware_config_defaults_unchanged_on_empty_file) {
    std::string path = writeTempConfig("");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev0.0");
    CHECK(cfg.displayRotation == 90);
    CHECK(cfg.batteryI2cDevice == "/dev/i2c-1");
    CHECK(cfg.batteryI2cAddress == 0x48);
    CHECK(cfg.captureDir == "/home/pi/captures");
    CHECK(cfg.previewWidth == 320);
    CHECK(cfg.previewHeight == 240);
    CHECK(cfg.maxFps == 20);
    CHECK(cfg.captureWidth == 4056);
    CHECK(cfg.captureHeight == 3040);
    CHECK(cfg.enableBattery == true);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_spi_device) {
    std::string path = writeTempConfig("spi_device = /dev/spidev1.0\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_display_rotation) {
    std::string path = writeTempConfig("display_rotate = 180\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.displayRotation == 180);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_battery_i2c) {
    std::string path = writeTempConfig("battery_i2c = /dev/i2c-3\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.batteryI2cDevice == "/dev/i2c-3");
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_battery_addr_hex) {
    std::string path = writeTempConfig("battery_addr = 0x49\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.batteryI2cAddress == 0x49);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_battery_addr_decimal) {
    std::string path = writeTempConfig("battery_addr = 73\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.batteryI2cAddress == 73);
    std::filesystem::remove(path);
}

TEST(hardware_config_rejects_empty_hex_addr) {
    // "0x" with no digits should be rejected, leaving default unchanged
    std::string path = writeTempConfig("battery_addr = 0x\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.batteryI2cAddress == 0x48);
    std::filesystem::remove(path);
}

TEST(hardware_config_rejects_invalid_hex_addr) {
    // "0xGG" is not valid hex
    std::string path = writeTempConfig("battery_addr = 0xGG\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.batteryI2cAddress == 0x48);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_capture_dir) {
    std::string path = writeTempConfig("capture_dir = /tmp/captures\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.captureDir == "/tmp/captures");
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_preview_dimensions) {
    std::string path = writeTempConfig(
        "preview_width = 640\npreview_height = 480\npreview_fps = 30\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.previewWidth == 640);
    CHECK(cfg.previewHeight == 480);
    CHECK(cfg.maxFps == 30);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_capture_dimensions) {
    std::string path = writeTempConfig(
        "capture_width = 2028\ncapture_height = 1520\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.captureWidth == 2028);
    CHECK(cfg.captureHeight == 1520);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_enable_battery_true) {
    std::string path = writeTempConfig("enable_battery = true\n");
    HardwareConfig cfg;
    cfg.enableBattery = false;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.enableBattery == true);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_enable_battery_false) {
    std::string path = writeTempConfig("enable_battery = false\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.enableBattery == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_enable_battery_yes_no) {
    std::string path = writeTempConfig("enable_battery = no\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.enableBattery == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_wifi_enabled_true) {
    std::string path = writeTempConfig("wifi_enabled = true\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.wifiEnabled == true);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_wifi_enabled_false) {
    std::string path = writeTempConfig("wifi_enabled = false\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.wifiEnabled == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_wifi_enabled_default_false) {
    std::string path = writeTempConfig("");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.wifiEnabled == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_bt_enabled_true) {
    std::string path = writeTempConfig("bt_enabled = true\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.btEnabled == true);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_bt_enabled_false) {
    std::string path = writeTempConfig("bt_enabled = false\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.btEnabled == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_bt_enabled_default_false) {
    std::string path = writeTempConfig("");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.btEnabled == false);
    std::filesystem::remove(path);
}

TEST(hardware_config_skips_comments) {
    std::string path = writeTempConfig(
        "# This is a comment\n"
        "spi_device = /dev/spidev1.0\n"
        "# Another comment\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    std::filesystem::remove(path);
}

TEST(hardware_config_skips_empty_lines) {
    std::string path = writeTempConfig(
        "\n\n"
        "spi_device = /dev/spidev1.0\n"
        "\n\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    std::filesystem::remove(path);
}

TEST(hardware_config_trims_whitespace) {
    std::string path = writeTempConfig(
        "  spi_device   =   /dev/spidev1.0  \n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    std::filesystem::remove(path);
}

TEST(hardware_config_unknown_key_ignored) {
    std::string path = writeTempConfig(
        "unknown_key = value\n"
        "spi_device = /dev/spidev1.0\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    std::filesystem::remove(path);
}

TEST(hardware_config_invalid_rotation_ignored) {
    std::string path = writeTempConfig("display_rotate = 45\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.displayRotation == 90);
    std::filesystem::remove(path);
}

TEST(hardware_config_parses_capture_prefix) {
    std::string path = writeTempConfig("capture_prefix = photo\n");
    HardwareConfig cfg;
    loadHardwareConfig(path, cfg);
    CHECK(cfg.capturePrefix == "photo");
    std::filesystem::remove(path);
}

TEST(hardware_config_multiple_keys) {
    std::string path = writeTempConfig(
        "spi_device = /dev/spidev1.0\n"
        "display_rotate = 270\n"
        "battery_i2c = /dev/i2c-3\n"
        "battery_addr = 0x49\n"
        "capture_dir = /tmp/captures\n"
        "preview_width = 640\n"
        "preview_height = 480\n"
        "preview_fps = 30\n"
        "capture_width = 2028\n"
        "capture_height = 1520\n"
        "enable_battery = true\n");
    HardwareConfig cfg;
    CHECK(loadHardwareConfig(path, cfg));
    CHECK(cfg.spiDevice == "/dev/spidev1.0");
    CHECK(cfg.displayRotation == 270);
    CHECK(cfg.batteryI2cDevice == "/dev/i2c-3");
    CHECK(cfg.batteryI2cAddress == 0x49);
    CHECK(cfg.captureDir == "/tmp/captures");
    CHECK(cfg.previewWidth == 640);
    CHECK(cfg.previewHeight == 480);
    CHECK(cfg.maxFps == 30);
    CHECK(cfg.captureWidth == 2028);
    CHECK(cfg.captureHeight == 1520);
    CHECK(cfg.enableBattery == true);
    std::filesystem::remove(path);
}
