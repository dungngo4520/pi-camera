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
)

target_include_directories(picamera PRIVATE
    ${LIBCAMERA_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS}
    src
)

target_link_libraries(picamera
    ${LIBCAMERA_LIBRARIES}
    ${PNG_LIBRARIES}
    dl      # libdl for dynamic loading (used by libcamera internals)
    pthread # POSIX threads (used by libcamera event handling)
)

target_compile_options(picamera PRIVATE ${LIBCAMERA_CFLAGS_OTHER} ${PNG_CFLAGS_OTHER})
