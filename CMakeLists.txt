cmake_minimum_required(VERSION 3.20)
project(picamera VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCAMERA REQUIRED libcamera)
pkg_check_modules(PNG REQUIRED libpng)

add_executable(picamera
    src/main.cpp
    src/camera.cpp
    src/cli.cpp
    src/image.cpp
    src/output.cpp
    src/timelapse.cpp
    src/dng.cpp
)

target_include_directories(picamera PRIVATE
    ${LIBCAMERA_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    src
)

target_link_libraries(picamera
    ${LIBCAMERA_LIBRARIES}
    ${PNG_LIBRARIES}
    dl
    pthread
)

target_compile_options(picamera PRIVATE
    ${LIBCAMERA_CFLAGS_OTHER}
    ${PNG_CFLAGS_OTHER}
)

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
        tests/test_cli.cpp
        tests/test_timelapse.cpp
        tests/test_dng.cpp
        src/image.cpp
        src/output.cpp
        src/cli.cpp
        src/timelapse.cpp
        src/dng.cpp
    )

    add_executable(picamera_tests ${test_sources})
    target_include_directories(picamera_tests PRIVATE
        src tests ${PNG_INCLUDE_DIRS} ${LIBCAMERA_INCLUDE_DIRS})
    target_link_libraries(picamera_tests ${PNG_LIBRARIES})

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
