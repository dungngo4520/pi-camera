cmake_minimum_required(VERSION 3.20)
project(picamera VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Dependencies via pkg-config IMPORTED_TARGET. Linking PkgConfig::<prefix>
# carries the include dirs, compile options, and link flags automatically —
# no manual ${..._INCLUDE_DIRS} / ${..._LIBRARIES} plumbing needed.
pkg_check_modules(LIBCAMERA QUIET IMPORTED_TARGET libcamera)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# libjpeg-turbo for software JPEG encoding (fallback when Pi HW MJPEG
# is unavailable at high resolutions). Optional — if missing, JPEG
# capture will only work with HW MJPEG.
pkg_check_modules(JPEG QUIET IMPORTED_TARGET libjpeg)

# libgpiod is required for SPI display + button input (preview mode).
# Optional on x86 dev builds — preview will print an error if not built with it.
pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod>=2.0)

# FreeType2 for TrueType font rendering (DejaVu Sans Mono) on the LCD overlay.
# Optional — falls back to the bitmap font if not found.
pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)

# BlueZ for Bluetooth RFCOMM remote control (optional). When not found, a stub
# BtServer is compiled that prints an error. Install libbluetooth-dev on the Pi.
pkg_check_modules(BLUEZ QUIET IMPORTED_TARGET bluez)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Force -std=c++20 in compile_commands.json even when the compiler defaults
# to C++20. clang-tidy reads compile_commands.json but does not inherit the
# compiler's default standard, so without an explicit -std= flag it falls
# back to C++17 and fails on std::span, std::endian, etc.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Warnings as a baseline for all targets.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(PICAMERA_BUILD_TESTS "Build unit tests" ON)
option(PICAMERA_ENABLE_SANITIZERS "Enable ASan+UBSan for the test build" OFF)

# --------------------------------------------------------------------------- #
# picamera_core — pure-logic TUs shared by the app and the unit tests.
#
# Built once as a STATIC library so the source list lives in exactly one
# place; both the picamera executable and the picamera_tests executable link
# it. libpng is used directly by encoders.cpp. libjpeg is optional
# (HAVE_JPEG gates the software encode path in encoders.cpp). pthread is
# for image.cpp's std::thread workers. libcamera is NOT needed here —
# camera_config.h is a plain data header with no libcamera dependency.
# --------------------------------------------------------------------------- #
add_library(picamera_core STATIC
    src/image.cpp
    src/encoders.cpp
    src/output_writer.cpp
    src/cli.cpp
    src/timelapse.cpp
    src/dng.cpp
    src/battery.cpp
    src/font.cpp
    src/safe_path.cpp
    src/camera_mode.cpp
    src/image_decode.cpp
    src/preview_helpers.cpp
    src/file_util.cpp
    src/hardware_config.cpp
    src/settings_menu.cpp
    src/wifi_server.cpp
    src/bt_server.cpp
)
target_compile_features(picamera_core PUBLIC cxx_std_20)
target_include_directories(picamera_core PUBLIC src)
target_link_libraries(picamera_core
    PUBLIC
        PkgConfig::PNG
        Threads::Threads
)
if(JPEG_FOUND)
    target_compile_definitions(picamera_core PUBLIC HAVE_JPEG)
    target_link_libraries(picamera_core PUBLIC PkgConfig::JPEG)
endif()
if(FREETYPE_FOUND)
    target_compile_definitions(picamera_core PUBLIC HAVE_FREETYPE)
    target_link_libraries(picamera_core PUBLIC PkgConfig::FREETYPE)
endif()
if(BLUEZ_FOUND)
    target_compile_definitions(picamera_core PUBLIC HAVE_BLUEZ)
    target_link_libraries(picamera_core PUBLIC PkgConfig::BLUEZ)
endif()

# --------------------------------------------------------------------------- #
# picamera — the app. Hardware TUs (camera, preview, dual_stream, display,
# buttons) live here; they are not unit-testable on x86. display.cpp/
# buttons.cpp are gated on libgpiod. The entire executable requires libcamera.
# --------------------------------------------------------------------------- #
if(LIBCAMERA_FOUND)
add_executable(picamera
    src/main.cpp
    src/camera.cpp
    src/camera_handle.cpp
    src/controls.cpp
    src/preview.cpp
    src/dual_stream.cpp
    src/timelapse_runner.cpp
)
target_compile_features(picamera PRIVATE cxx_std_20)
target_link_libraries(picamera PRIVATE picamera_core PkgConfig::LIBCAMERA)
if(LIBGPIOD_FOUND)
    target_sources(picamera PRIVATE src/display.cpp src/buttons.cpp)
    target_compile_definitions(picamera PRIVATE HAVE_GPIOD)
    target_link_libraries(picamera PRIVATE PkgConfig::LIBGPIOD)
endif()
else()
    message(STATUS "libcamera not found — building tests only (no picamera executable)")
endif()

# --------------------------------------------------------------------------- #
# Unit tests — pure-logic only (no camera hardware needed). Links picamera_core
# plus a tiny hand-rolled test runner in tests/.
# --------------------------------------------------------------------------- #
if(PICAMERA_BUILD_TESTS)
    enable_testing()

    add_executable(picamera_tests
        tests/test_main.cpp
        tests/test_image.cpp
        tests/test_output.cpp
        tests/test_output_writer.cpp
        tests/test_cli.cpp
        tests/test_timelapse.cpp
        tests/test_dng.cpp
        tests/test_battery.cpp
        tests/test_safe_path.cpp
        tests/test_image_decode.cpp
        tests/test_font.cpp
        tests/test_preview_helpers.cpp
        tests/test_camera_mode.cpp
        tests/test_stop_flag.cpp
        tests/test_hardware_config.cpp
        tests/test_settings_menu.cpp
        tests/test_wb_utils.cpp
        tests/test_wifi_server.cpp
        tests/test_bt_server.cpp
        tests/test_encoders.cpp
    )
    target_compile_features(picamera_tests PRIVATE cxx_std_20)
    target_include_directories(picamera_tests PRIVATE tests)
    target_link_libraries(picamera_tests PRIVATE picamera_core)

    if(PICAMERA_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(picamera_core PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
            target_link_options(picamera_core PUBLIC
                -fsanitize=address,undefined)
            target_compile_options(picamera_tests PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
        endif()
    endif()

    add_test(NAME picamera_tests COMMAND picamera_tests)
endif()

# --------------------------------------------------------------------------- #
# Install — deploys the binary and systemd service for the appliance mode
# (power-on = camera works). On the Pi:
#   cmake --install .  (or sudo make install)
#   sudo systemctl enable picamera
#   sudo systemctl start picamera
# --------------------------------------------------------------------------- #
if(LIBCAMERA_FOUND)
install(TARGETS picamera RUNTIME DESTINATION bin)
install(FILES config/systemd/picamera.service
        DESTINATION lib/systemd/system)
endif()
