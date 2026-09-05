#include "test_runner.h"
#include "bt_server.h"
#include "wifi_server.h" // applySettingsJson (reused by BT SET)
#include "camera_mode.h"

#include <string>
#include <vector>

using picamera::BtCommand;
using picamera::BtCommandType;
using picamera::parseBtCommand;
using picamera::fileListJson;
using picamera::btSetToJson;
using picamera::applySettingsJson;
using picamera::CameraSettings;
using picamera::OutputFormat;
using picamera::DriveMode;

// --- parseBtCommand: basic verbs ---

TEST(bt_parse_capture) {
    BtCommand cmd;
    CHECK(parseBtCommand("CAPTURE", cmd));
    CHECK(cmd.type == BtCommandType::Capture);
}

TEST(bt_parse_capture_with_newline_trimmed) {
    BtCommand cmd;
    CHECK(parseBtCommand("CAPTURE\r\n", cmd));
    CHECK(cmd.type == BtCommandType::Capture);
}

TEST(bt_parse_status) {
    BtCommand cmd;
    CHECK(parseBtCommand("STATUS", cmd));
    CHECK(cmd.type == BtCommandType::Status);
}

TEST(bt_parse_settings) {
    BtCommand cmd;
    CHECK(parseBtCommand("SETTINGS", cmd));
    CHECK(cmd.type == BtCommandType::Settings);
}

TEST(bt_parse_list) {
    BtCommand cmd;
    CHECK(parseBtCommand("LIST", cmd));
    CHECK(cmd.type == BtCommandType::List);
}

TEST(bt_parse_quit) {
    BtCommand cmd;
    CHECK(parseBtCommand("QUIT", cmd));
    CHECK(cmd.type == BtCommandType::Quit);
}

// --- parseBtCommand: case insensitivity ---

TEST(bt_parse_case_insensitive) {
    BtCommand cmd;
    CHECK(parseBtCommand("capture", cmd));
    CHECK(cmd.type == BtCommandType::Capture);
    CHECK(parseBtCommand("Capture", cmd));
    CHECK(cmd.type == BtCommandType::Capture);
    CHECK(parseBtCommand("StAtUs", cmd));
    CHECK(cmd.type == BtCommandType::Status);
}

// --- parseBtCommand: SET command ---

TEST(bt_parse_set_basic) {
    BtCommand cmd;
    CHECK(parseBtCommand("SET shutterUs=5000", cmd));
    CHECK(cmd.type == BtCommandType::Set);
    CHECK(cmd.key == "shutterUs");
    CHECK(cmd.value == "5000");
}

TEST(bt_parse_set_with_spaces) {
    BtCommand cmd;
    CHECK(parseBtCommand("SET  awbMode = daylight ", cmd));
    CHECK(cmd.type == BtCommandType::Set);
    CHECK(cmd.key == "awbMode");
    CHECK(cmd.value == "daylight");
}

TEST(bt_parse_set_value_with_equals) {
    // Value may contain '=' — only split on the first '='.
    BtCommand cmd;
    CHECK(parseBtCommand("SET key=val=ue", cmd));
    CHECK(cmd.type == BtCommandType::Set);
    CHECK(cmd.key == "key");
    CHECK(cmd.value == "val=ue");
}

TEST(bt_parse_set_missing_equals_returns_false) {
    BtCommand cmd;
    CHECK(!parseBtCommand("SET novalue", cmd));
}

TEST(bt_parse_set_missing_key_returns_false) {
    BtCommand cmd;
    CHECK(!parseBtCommand("SET =value", cmd));
}

TEST(bt_parse_set_empty_value_allowed) {
    BtCommand cmd;
    CHECK(parseBtCommand("SET key=", cmd));
    CHECK(cmd.type == BtCommandType::Set);
    CHECK(cmd.key == "key");
    CHECK(cmd.value == "");
}

TEST(bt_parse_set_no_args_returns_false) {
    BtCommand cmd;
    CHECK(!parseBtCommand("SET", cmd));
}

// --- parseBtCommand: edge cases ---

TEST(bt_parse_empty_line_returns_false) {
    BtCommand cmd;
    CHECK(!parseBtCommand("", cmd));
    CHECK(!parseBtCommand("   ", cmd));
    CHECK(!parseBtCommand("\r\n", cmd));
}

TEST(bt_parse_unknown_command_returns_false) {
    BtCommand cmd;
    CHECK(!parseBtCommand("FOOBAR", cmd));
    CHECK(cmd.type == BtCommandType::Unknown);
}

TEST(bt_parse_trims_leading_whitespace) {
    BtCommand cmd;
    CHECK(parseBtCommand("  CAPTURE  ", cmd));
    CHECK(cmd.type == BtCommandType::Capture);
}

// --- fileListJson ---

TEST(bt_file_list_json_empty) {
    std::vector<std::string> files;
    std::string json = fileListJson(files);
    CHECK(json == "[]");
}

TEST(bt_file_list_json_single_file) {
    std::vector<std::string> files = {"/home/pi/captures/photo.jpg"};
    std::string json = fileListJson(files);
    CHECK(json == "[\"photo.jpg\"]");
}

TEST(bt_file_list_json_multiple_files) {
    std::vector<std::string> files = {"/a/b/c.jpg", "/a/b/d.dng"};
    std::string json = fileListJson(files);
    CHECK(json == "[\"c.jpg\",\"d.dng\"]");
}

TEST(bt_file_list_json_bare_filename) {
    std::vector<std::string> files = {"photo.jpg"};
    std::string json = fileListJson(files);
    CHECK(json == "[\"photo.jpg\"]");
}

TEST(bt_file_list_json_escapes_quotes) {
    std::vector<std::string> files = {"bad\"name.jpg"};
    std::string json = fileListJson(files);
    CHECK(json.find("\\\"") != std::string::npos);
}

// --- btSetToJson ---

TEST(bt_set_to_json_basic) {
    std::string json = btSetToJson("shutterUs", "5000");
    CHECK(json == "{\"shutterUs\":\"5000\"}");
}

TEST(bt_set_to_json_string_value) {
    std::string json = btSetToJson("awbMode", "daylight");
    CHECK(json == "{\"awbMode\":\"daylight\"}");
}

TEST(bt_set_to_json_escapes_special_chars) {
    std::string json = btSetToJson("key", "val\"ue");
    CHECK(json.find("\\\"") != std::string::npos);
}

TEST(bt_set_to_json_empty_value) {
    std::string json = btSetToJson("key", "");
    CHECK(json == "{\"key\":\"\"}");
}

// --- Integration: btSetToJson -> applySettingsJson ---

TEST(bt_set_to_json_applies_numeric_setting) {
    // The BT SET command builds {"key":"value"} and feeds it to
    // applySettingsJson, which handles quoted numeric values.
    CameraSettings s;
    s.shutterUs = 0;
    std::string json = btSetToJson("shutterUs", "5000");
    applySettingsJson(json, s);
    CHECK(s.shutterUs == 5000);
}

TEST(bt_set_to_json_applies_string_setting) {
    CameraSettings s;
    s.awbMode = "auto";
    std::string json = btSetToJson("awbMode", "daylight");
    applySettingsJson(json, s);
    CHECK(s.awbMode == "daylight");
}

TEST(bt_set_to_json_applies_bool_setting) {
    CameraSettings s;
    s.aeEnable = true;
    std::string json = btSetToJson("aeEnable", "false");
    applySettingsJson(json, s);
    CHECK(s.aeEnable == false);
}

TEST(bt_set_to_json_applies_float_setting) {
    CameraSettings s;
    s.analogueGain = 0;
    std::string json = btSetToJson("analogueGain", "4.0");
    applySettingsJson(json, s);
    CHECK(s.analogueGain == 4.0f);
}

TEST(bt_set_to_json_applies_drive_mode) {
    CameraSettings s;
    s.driveMode = DriveMode::Single;
    std::string json = btSetToJson("driveMode", "bracket");
    applySettingsJson(json, s);
    CHECK(s.driveMode == DriveMode::Bracket);
}

TEST(bt_set_to_json_applies_format) {
    CameraSettings s;
    s.captureFormat = OutputFormat::JPEG;
    std::string json = btSetToJson("captureFormat", "dng");
    applySettingsJson(json, s);
    CHECK(s.captureFormat == OutputFormat::DNG);
}
