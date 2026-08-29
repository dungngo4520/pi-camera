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

## Preview mode (live LCD streaming)

`--preview` streams the camera to a Waveshare 1.44" LCD HAT (ST7735S, 128x128, SPI)
and captures full-res JPEGs on joystick press.

- **Display driver**: userspace ST7735S via `spidev` + `libgpiod` (no kernel overlay needed).
  - SPI: `/dev/spidev0.0` at 16 MHz, mode 0
  - GPIO: DC=BCM25, RST=BCM27, BL=BCM24
  - SPI data is sent in 256-byte chunks (large single transfers fail on spidev)
  - Default rotation: 180 (MX|MY|BGR), adjustable with `--display-rotate`
- **Camera stream**: `CameraStream` class uses libcamera Viewfinder role with NV12
  at 320x240, 4 buffers, continuous re-queue. Frame data is mmap'd and copied.
- **Button input**: `ButtonInput` class polls libgpiod lines. Joystick press (BCM13)
  triggers full-res capture via `CameraApp`.
- **Capture**: stream is stopped + shut down, 500ms delay for V4L2 buffer release,
  then `CameraApp` captures 4056x3040 JPEG. Stream is restarted after.
- **libgpiod dependency**: optional at build time (CMake `pkg_check_modules QUIET`).
  Without it, `--preview` prints an error. Install `libgpiod-dev` on the Pi.
- **Pi Zero 2 W memory**: 512MB RAM. Build with `-j1` to avoid OOM.
  Add swap (`sudo fallocate -l 512M /swapfile && mkswap /swapfile && swapon /swapfile`).
- **Build on Pi**: `cmake -DPICAMERA_BUILD_TESTS=OFF .. && make -j1`

## JPEG capture (software fallback)

The Pi VC4 pipeline handler does not support HW MJPEG at 4056x3040 — it silently
falls back to YUYV. The code detects this and reconfigures with NV12, then encodes
JPEG in software via libjpeg-turbo.

- **libjpeg dependency**: optional (CMake `pkg_check_modules QUIET libjpeg`).
  Install `libjpeg-dev` on the Pi. Without it, JPEG capture at full res will fail.
- **Buffer count**: reduced to 1 for high-res NV12 (>2MP) to avoid V4L2 ENOMEM.
- **Detection**: checks `sc.pixelFormat` after both `validate()` and `configure()`,
  since the pipeline handler may change the format at either stage.

## Battery monitoring (ADS1115 ADC)

The UPS-Lite V1.2's onboard MAX17040G fuel gauge is a **fake/clone** at I2C
address 0x32 (genuine = 0x36). Its VCELL register always reads 0x0000 and SOC
is garbage (counts down from arbitrary values). The power path works (cell
powers the Pi), but the measurement path is broken.

To work around this, an external **ADS1115** (16-bit I2C ADC) reads the cell
voltage directly. The voltage is converted to LiPo state-of-charge via a
piecewise-linear discharge curve (±5-10% accuracy, vs ±2% for a real gauge).

- **Wiring** (ADS1115 → Pi Zero 2 W):
  - VDD  → Pi 5V (pin 2 or 4)
  - GND  → Pi GND (pin 6)
  - SDA  → Pi GPIO2/SDA1 (pin 3, i2c-1)
  - SCL  → Pi GPIO3/SCL1 (pin 5, i2c-1)
  - A0   → Battery + terminal (on UPS-Lite battery JST connector)
  - ADDR → GND (sets I2C address to 0x48)
  - No voltage divider needed: PGA gain ±6.144V covers LiPo 3.0-4.2V directly
- **I2C address**: 0x48 (default, ADDR→GND)
- **PGA gain**: ±6.144V (config 0x0000, LSB = 187.5µV)
- **Sample rate**: 128 SPS (continuous mode)
- **Battery read interval**: every 3 seconds during preview (I2C is slow)
- **Display overlay**: battery icon (18x9px) + percentage text in top-right
  corner of the Waveshare LCD during `--preview` mode
- **CLI flags**: `--battery` (enable overlay), `--battery-i2c <path>`,
  `--battery-addr <hex>`
- **Code**: `src/battery.cpp` (ADS1115 I2C + LiPo SOC), `src/font.cpp`
  (5x7 bitmap font + battery icon renderer), both unit-tested
- **No new library dependency**: uses raw i2c-dev ioctl (kernel module)
