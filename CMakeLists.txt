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

set(picamera_sources
    src/main.cpp
    src/camera.cpp
    src/cli.cpp
    src/image.cpp
    src/output.cpp
    src/output_writer.cpp
    src/timelapse.cpp
    src/dng.cpp
    src/preview.cpp
    src/stream.cpp
    src/battery.cpp
    src/font.cpp
)

if(LIBGPIOD_FOUND)
    list(APPEND picamera_sources src/display.cpp src/buttons.cpp)
endif()

add_executable(picamera ${picamera_sources})

target_compile_features(picamera PRIVATE cxx_std_20)

target_include_directories(picamera PRIVATE src)
target_link_libraries(picamera
    PRIVATE
        PkgConfig::LIBCAMERA
        PkgConfig::PNG
        dl
        pthread
)

if(LIBGPIOD_FOUND)
    target_compile_definitions(picamera PRIVATE HAVE_GPIOD)
    target_link_libraries(picamera PRIVATE PkgConfig::LIBGPIOD)
endif()

if(JPEG_FOUND)
    target_compile_definitions(picamera PRIVATE HAVE_JPEG)
    target_link_libraries(picamera PRIVATE PkgConfig::JPEG)
endif()

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
# Unit tests — pure-logic only (no camera hardware needed). Links the same
# image/output/cli/timelapse TUs as the main binary plus a tiny hand-rolled
# test runner in tests/.
# --------------------------------------------------------------------------- #
if(PICAMERA_BUILD_TESTS)
    enable_testing()

    set(test_sources
        tests/test_main.cpp
        tests/test_image.cpp
        tests/test_output.cpp
        tests/test_output_writer.cpp
        tests/test_cli.cpp
        tests/test_timelapse.cpp
        tests/test_dng.cpp
        tests/test_battery.cpp
        src/image.cpp
        src/output.cpp
        src/output_writer.cpp
        src/cli.cpp
        src/timelapse.cpp
        src/dng.cpp
        src/battery.cpp
        src/font.cpp
    )

    add_executable(picamera_tests ${test_sources})
    target_compile_features(picamera_tests PRIVATE cxx_std_20)

    target_include_directories(picamera_tests PRIVATE src tests)
    # LIBCAMERA is header-only here (camera.cpp is not compiled into tests,
    # but cli.h includes camera.h which pulls libcamera headers). PNG is
    # linked because output.cpp uses libpng directly.
    target_link_libraries(picamera_tests
        PRIVATE
            PkgConfig::LIBCAMERA
            PkgConfig::PNG
    )

    if(JPEG_FOUND)
        target_compile_definitions(picamera_tests PRIVATE HAVE_JPEG)
        target_link_libraries(picamera_tests PRIVATE PkgConfig::JPEG)
    endif()

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
