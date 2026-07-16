# picamera — build & verification commands

C++20 libcamera front-end for the Raspberry Pi HQ Camera (IMX477) on Pi Zero 2 W.

## Build

```bash
make build              # native x86-64 build (for development), outputs build/picamera
make clean              # remove build/ and build-san/
```

The production target is aarch64 (Pi). Two ways to build for it:
```bash
make cross-build        # Docker + qemu-user-static -> ./picamera-arm64
make remote-deploy      # rsync source to Pi + build on Pi (PI_HOST=, PI_USER= override)
```

## Test & verify

```bash
make test               # build + run unit tests via ctest (37 tests, pure-logic, no camera)
make test-sanitize      # build + run under ASan + UBSan with leak detection
make tidy               # clang-tidy on src/ (zero warnings expected; system-header noise filtered)
```

CMake options:
- `-DPICAMERA_BUILD_TESTS=OFF` — skip the test target
- `-DPICAMERA_ENABLE_SANITIZERS=ON` — ASan + UBSan on the test binary
- `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` — for clang-tidy / IDE integration (make build does this)

## CI

`.github/workflows/ci.yml` — two jobs:
1. `build-and-test` (x86-64): build + ctest + sanitizers + clang-tidy
2. `cross-build` (aarch64): Docker cross-build, uploads `picamera-arm64` artifact

## Conventions

- C++20, `-Wall -Wextra -Wpedantic` baseline (set in CMakeLists).
- clang-tidy config in `.clang-tidy` — correctness checks enabled, style noise disabled.
- No external test framework; `tests/test_runner.h` is a minimal TEST/CHECK/REQUIRE runner.
- Pure-logic code (image, output, cli, timelapse) is unit-tested; camera.cpp requires hardware.
- `formatTimelapseName` validates `--output` patterns to prevent format-string vulns (see test_timelapse.cpp).
