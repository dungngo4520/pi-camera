cmake_minimum_required(VERSION 3.20)
project(picamera VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig REQUIRED)

# Dependencies via pkg-config IMPORTED_TARGET. Linking PkgConfig::<prefix>
# carries the include dirs, compile options, and link flags automatically —
# no manual ${..._INCLUDE_DIRS} / ${..._LIBRARIES} plumbing needed.
pkg_check_modules(LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# libjpeg-turbo for software JPEG encoding (fallback when Pi HW MJPEG
# is unavailable at high resolutions). Optional — if missing, JPEG
# capture will only work with HW MJPEG.
pkg_check_modules(JPEG QUIET IMPORTED_TARGET libjpeg)

# libgpiod is required for SPI display + button input (preview mode).
# Optional on x86 dev builds — preview will print an error if not built with it.
pkg_check_modules(LIBGPIOD QUIET IMPORTED_TARGET libgpiod)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

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
# it. libcamera is header-only here (camera.cpp is NOT in core — cli.h
# includes camera.h which pulls libcamera headers), but linking it PUBLIC
# lets consumers inherit the include dirs. libpng is used directly by
# encoders.cpp. libjpeg is optional (HAVE_JPEG gates the software encode
# path in encoders.cpp). pthread is for image.cpp's std::thread workers.
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
)
target_compile_features(picamera_core PUBLIC cxx_std_20)
target_include_directories(picamera_core PUBLIC src)
target_link_libraries(picamera_core
    PUBLIC
        PkgConfig::LIBCAMERA
        PkgConfig::PNG
        pthread
)
if(JPEG_FOUND)
    target_compile_definitions(picamera_core PUBLIC HAVE_JPEG)
    target_link_libraries(picamera_core PUBLIC PkgConfig::JPEG)
endif()

# --------------------------------------------------------------------------- #
# picamera — the app. Hardware TUs (camera, preview, stream, display, buttons)
# live here; they are not unit-testable on x86. display.cpp/buttons.cpp are
# gated on libgpiod.
# --------------------------------------------------------------------------- #
add_executable(picamera
    src/main.cpp
    src/camera.cpp
    src/preview.cpp
    src/stream.cpp
    src/timelapse_runner.cpp
)
target_link_libraries(picamera PRIVATE picamera_core dl)
if(LIBGPIOD_FOUND)
    target_sources(picamera PRIVATE src/display.cpp src/buttons.cpp)
    target_compile_definitions(picamera PRIVATE HAVE_GPIOD)
    target_link_libraries(picamera PRIVATE PkgConfig::LIBGPIOD)
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
    )
    target_compile_features(picamera_tests PRIVATE cxx_std_20)
    target_include_directories(picamera_tests PRIVATE tests)
    target_link_libraries(picamera_tests PRIVATE picamera_core)

    if(PICAMERA_ENABLE_SANITIZERS)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(picamera_tests PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
            target_link_options(picamera_tests PRIVATE
                -fsanitize=address,undefined)
        endif()
    endif()

    add_test(NAME picamera_tests COMMAND picamera_tests)
endif()
