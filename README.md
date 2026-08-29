# pi-camera

A small, dependency-light C++20 libcamera front-end for the **Raspberry Pi HQ Camera (IMX477)** on a **Pi Zero 2 W**. Captures stills and timelapses at full sensor resolution (4056x3040) and saves them as PPM, PNG, JPEG, DNG, or raw NV12. A live `--preview` mode streams the viewfinder to a Waveshare 1.44" SPI LCD HAT and captures full-res stills on joystick press — headless one-button capture with no daemon required.

## Performance

The capture pipeline is optimized for the Pi Zero 2 W's quad-core Cortex-A53:

- **NEON SIMD NV12→RGB**: The color conversion uses ARM NEON intrinsics to process 16 pixels per iteration (8 UV pairs, two 8-pixel halves), with a scalar fallback for x86 builds and odd-width remainders. ~4–8× faster than the scalar loop.
- **Multi-threaded conversion**: The NV12→RGB conversion splits the image into horizontal strips and processes them in parallel across all 4 cores. Thread count is capped at `hardware_concurrency()` and the number of 2-row pairs.
- **ISP hardware JPEG encode**: `--format jpeg` configures libcamera to output MJPEG directly from the Pi's ISP hardware encoder. The buffer contains a complete JPEG bitstream — no software conversion or encode needed. This is ~10× faster than the NV12→RGB→PNG path for full-resolution captures.
- **Configurable PNG compression**: `--png-level 0–9` controls the zlib compression level. Level 1 (fast) produces ~15% larger files but encodes ~2× faster than the default level 6 — useful when capture speed matters more than file size.

## Image quality

- **DNG raw capture**: `--format dng` captures raw Bayer data (SRGGB10_CSI2P from the Pi ISP) and writes a minimal DNG 1.6 file with CFA pattern, black/white levels, active area, and EXIF metadata (exposure time, ISO, timestamp). The 10-bit MIPI-packed samples are unpacked to 16-bit for compatibility with raw developers (Lightroom, RawTherapee, darktable).
- **HDR bracketing**: `--bracket N,ev1,ev2,...` captures N frames at different EV offsets. For example, `--bracket 3,-2,0,+2` captures 3 frames at -2EV, 0EV, and +2EV. Each frame's exposure time is scaled by 2^ev. Filenames get an `_ev±N` suffix (e.g. `photo_ev-2.0.png`). Works best with manual exposure (`--shutter` + `--iso`).
- **EXIF metadata**: DNG files embed exposure time (as a rational fraction), ISO speed (computed from analogue gain), and DateTimeOriginal (UTC). PNG and JPEG do not currently embed EXIF (the ISP JPEG may include it depending on the Pi firmware).

## Live preview (SPI LCD)

`--preview` runs a live viewfinder that streams low-res NV12 frames to a Waveshare 1.44" LCD HAT (ST7735S, 128x128, SPI) and captures full-resolution stills when the joystick is pressed. The display is driven from userspace via `spidev` + `libgpiod` — no kernel overlay or framebuffer required, which gives the lowest latency (no HTTP, no pipe, no decode).

```bash
# Live preview to the default SPI device (/dev/spidev0.0) at 320x240, 20fps
./picamera --preview

# Rotated display + PNG captures on joystick press
./picamera --preview --display-rotate 90 --capture-format png

# Lower frame rate to save power
./picamera --preview --preview-fps 10

# Show battery level overlay (ADS1115 ADC on /dev/i2c-1)
./picamera --preview --battery
```

The preview loop:
1. Captures NV12 frames at the configured preview resolution (default 320x240) via a `CameraStream` (libcamera Viewfinder role, 4 buffers, continuous re-queue).
2. Converts to RGB565 with center-crop + nearest-neighbor scaling (BT.601 limited-range).
3. Optionally draws a battery icon + percentage overlay (read from an ADS1115 ADC every 3 s).
4. Blits the RGB565 buffer to the ST7735S over SPI in 256-byte chunks.
5. On joystick press (BCM13): stops the stream, waits 500 ms for V4L2 buffer release, captures a full-res still via `CameraApp`, then restarts the stream.
6. Runs until Ctrl+C (SIGINT/SIGTERM).

Frame rate is capped at `--preview-fps` (default 20) to avoid burning CPU on the Pi Zero. AE/AWB run continuously. SPI runs at 16 MHz, mode 0; GPIO DC=BCM25, RST=BCM27, BL=BCM24. Default rotation is 180 (MX|MY|BGR) to orient the Waveshare HAT right-way-up; adjust with `--display-rotate` (0/90/180/270).

`--preview` requires **libgpiod** at build time. On x86 dev builds without it, `--preview` prints an error and exits — build on the Pi with `libgpiod-dev` installed.

## Hardware

| Part | Notes |
|---|---|
| Raspberry Pi Zero 2 W | aarch64, Raspberry Pi OS Lite (Debian 13 trixie) |
| Raspberry Pi HQ Camera (IMX477) | 12.3 MP, 4056x3040 native, 7.9 mm C-mount |
| C-mount lens | any CS/C-mount lens via the HQ Camera's C-CS adapter |
| Waveshare 1.44" LCD HAT (ST7735S) | 128x128 SPI display + 3 keys + joystick, for `--preview` |
| ADS1115 ADC (optional) | 16-bit I2C ADC reading LiPo cell voltage for the `--battery` overlay |

Required `config.txt` overlay on the Pi boot partition:

```
dtoverlay=imx477,gpu_mem=256
```

## Software requirements (on the Pi)

```
sudo apt install build-essential cmake pkg-config \
    libcamera-dev libpng-dev libjpeg-dev libgpiod-dev
```

`libgpiod-dev` is required for `--preview` (SPI display + joystick). `libjpeg-dev`
is required for software JPEG encoding when the Pi VC4 pipeline handler rejects
HW MJPEG at full resolution (it falls back to NV12 + libjpeg encode). Both are
optional at build time via CMake `pkg_check_modules QUIET` — without them the
relevant feature prints an error at runtime instead of failing to build.

Verified versions on the target board:

- libcamera 0.7.1
- libpng 1.6.48
- g++ 14.2.0
- cmake 3.31.6

## Building

Three options, in order of simplicity:

### 1. On the Pi (simplest)

```bash
make build          # mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

Or manually:

```bash
mkdir -p build && cd build && cmake .. && make -j$(nproc)
```

The binary lands at `build/picamera`.

### 2. Cross-build in Docker on your x86 host (no Pi needed for builds)

Uses Docker + qemu-user-static binfmt emulation to build an aarch64 binary inside a Debian trixie container with the Raspberry Pi apt archive (so libcamera matches the Pi's 0.7.x). One command:

```bash
make cross-build    # runs scripts/cross-build.sh, outputs ./picamera-arm64
```

Or directly:

```bash
./scripts/cross-build.sh
```

Requirements on the host:
- `docker` (or `podman`)
- `qemu-user-static` + binfmt_misc registered for aarch64
  - Arch: `pacman -S qemu-user-static && sudo systemctl restart systemd-binfmt`
  - Verify: `cat /proc/sys/fs/binfmt_misc/qemu-aarch64` should say `enabled`

The script builds the Docker image, extracts the binary, and verifies it with `file`. Output: `./picamera-arm64` (~90 KB, dynamically linked aarch64 ELF).

Transfer to the Pi:

```bash
make cross-deploy   # cross-build + scp to Pi in one step
# or manually:
scp picamera-arm64 pi@raspberrypi.local:~/camera/build/picamera
```

The binary is dynamically linked against `libcamera.so.0.7`, `libpng16.so.16`, etc. — all present on the Pi from `apt install libcamera-dev libpng-dev`. No extra runtime deps need to be copied.

First build takes ~3 min (downloads Debian base + apt packages). Subsequent builds are cached and take ~20 s (just the `cmake && make` step).

### 3. Remote build via SSH (deploy source, build on Pi)

The Makefile automates deploy + remote build over SSH. Override the host/user with `PI_HOST=` / `PI_USER=` if needed (defaults: `raspberrypi.local` / `pi`).

```bash
make remote-deploy   # rsync source to Pi, then build on the Pi
make remote-run ARGS="--list-controls"
make remote-clean    # wipe the remote build dir
```

## Usage

```
picamera --capture <file>              Capture a still
picamera --list-controls               List camera controls & properties
picamera --timelapse <sec> [options]   Timelapse mode
```

Options:

| Flag | Default | Description |
|---|---|---|
| `--format <type>` | `ppm` | Output format: `ppm`, `raw`, `png`, `jpeg`, `dng` |
| `--png-level <0-9>` | `6` | PNG compression level (0=none, 1=fast, 6=default, 9=best) |
| `--bracket <n,ev...>` | — | HDR bracketing: `N,ev1,ev2,...` (e.g. `3,-2,0,+2`) |
| `--preview` | — | Live preview mode (streams to SPI LCD, joystick = capture) |
| `--preview-w <px>` | `320` | Preview stream width |
| `--preview-h <px>` | `240` | Preview stream height |
| `--preview-fps <n>` | `20` | Preview max frame rate |
| `--capture-w <px>` | `4056` | Still capture width (during `--preview`) |
| `--capture-h <px>` | `3040` | Still capture height (during `--preview`) |
| `--capture-format <type>` | `jpeg` | Still capture format: `jpeg`, `png`, `ppm`, `dng` |
| `--capture-dir <path>` | `.` | Directory for captured stills (during `--preview`) |
| `--capture-prefix <str>` | `capture` | Filename prefix for captures (during `--preview`) |
| `--spi-device <path>` | `/dev/spidev0.0` | SPI device for the ST7735S display |
| `--display-rotate <deg>` | `180` | Display rotation: `0`, `90`, `180`, `270` |
| `--battery` | off | Show battery level overlay on preview (ADS1115 ADC) |
| `--battery-i2c <path>` | `/dev/i2c-1` | I2C device for the ADS1115 |
| `--battery-addr <hex>` | `0x48` | ADS1115 I2C address |
| `--output <pattern>` | `capture_%04d.ppm` | Timelapse filename pattern. `%04d` = sequence index, else `strftime` |
| `--count <n>` | `1` | Number of timelapse shots (`0` = infinite) |
| `--width <px>` | `4056` | Image width |
| `--height <px>` | `3040` | Image height |
| `--iso <gain>` | auto | Analogue gain (e.g. `1.0`, `2.0`, `4.0`). Setting this or `--shutter` auto-disables AE. |
| `--digital-gain <gain>` | auto | Digital gain (e.g. `1.0`, `2.0`) |
| `--shutter <us>` | auto | Exposure time in microseconds. Setting this or `--iso` auto-disables AE. |
| `--awb <mode>` | `auto` | White balance: `auto`, `daylight`, `cloudy`, `incandescent`, `tungsten`, `fluorescent`, `indoor` |
| `--ae-disable` | off | Disable auto exposure (use with `--shutter` / `--iso`) |
| `--awb-disable` | off | Disable auto white balance |
| `--warmup <n>` | `8` | Frames to discard before saving, letting AE/AWB converge |

### Examples

```bash
# Full-resolution PNG still
./build/picamera --capture photo.png --format png

# Raw NV12 dump (Y + UV planes, no conversion)
./build/picamera --capture frame.raw --format raw

# Manual exposure: ISO 2.0, 30 ms shutter (--ae-disable is implicit when
# --shutter or --iso is given)
./build/picamera --capture photo.ppm --iso 2.0 --shutter 30000

# 10-frame timelapse, one shot per minute, PNG
./build/picamera --timelapse 60 --count 10 \
    --output timelapse_%04d.png --format png

# ISP hardware JPEG (~10x faster than the PNG path)
./build/picamera --capture photo.jpg --format jpeg

# Raw Bayer DNG for raw development (Lightroom, darktable, RawTherapee)
./build/picamera --capture photo.dng --format dng

# Live preview with battery overlay; joystick press captures full-res JPEG
./build/picamera --preview --battery

# Quick 1080p capture
./build/picamera --capture quick.ppm --width 1920 --height 1080
```

### Timelapse `--output` patterns

The `--output` pattern is either a printf-style integer template (containing a `%d`/`%04d`-style conversion, substituted with the shot index) or a `strftime` template (expanded with the current local time). The two styles cannot be mixed in one pattern — a pattern with both `%04d` and `%Y` is rejected. Only integer printf conversions are allowed in the printf path (`%s`, `%n`, `%p`, etc. are rejected) so a user-supplied pattern can't read garbage off the stack.

Press `Ctrl-C` (SIGINT/SIGTERM) during a timelapse to stop gracefully after the current shot completes; the camera is released cleanly and no half-written frame is left behind.

### Output formats

| Format | Extension | Size at 4056x3040 | Notes |
|---|---|---|---|
| PPM | `.ppm` | ~37 MB | Uncompressed RGB, no encoder dependency. Fastest to write. |
| PNG | `.png` | ~5-15 MB | Lossless, libpng. CPU-heavy on the Pi Zero (~40 s for 12 MP). |
| JPEG | `.jpg` | ~2-6 MB | ISP hardware MJPEG (Pi only), ~10x faster than PNG. Falls back to software libjpeg encode if the VC4 pipeline rejects HW MJPEG at full res. |
| DNG | `.dng` | ~24 MB | Raw Bayer (SRGGB10_CSI2P) unpacked to 16-bit, DNG 1.6 + EXIF. For raw development. |
| Raw NV12 | `.raw` | ~18 MB | Y plane + UV plane as captured by the ISP. No color conversion. |

## Why warmup frames matter

libcamera's auto-exposure (AE) and auto-white-balance (AWB) algorithms need **several frames to converge** — they measure the scene, then adjust exposure and gain over successive frames. If you capture a single frame with no warmup, AE hasn't measured anything yet and the ISP applies a near-zero exposure, producing a **black frame**.

This was a real bug in earlier versions of this project: a 12 MP PNG came out at 36 KB because every pixel was `(0,0,0)` — PNG compresses uniform regions to almost nothing. The fix queues all available buffers, discards the first `--warmup` frames (re-queuing their buffers with `Request::ReuseBuffers`), and saves only the converged frame. Default is 8; bump to 15+ in low light.

Reference: a real 12 MP photo is typically:
- 2-6 MB as JPEG (lossy)
- 10-25 MB as PNG (lossless)
- ~37 MB as uncompressed RGB

If your PNG is suspiciously small (under ~1 MB at full res), it's almost certainly a black or near-uniform frame — increase `--warmup`.

## Headless one-button capture (`--preview`)

The `--preview` mode is the headless capture path: the Waveshare HAT's joystick
press (BCM13) triggers a full-res still capture without needing a separate
daemon or SSH session. The display flashes white-then-black for visual feedback,
the stream is briefly stopped to release the camera, the still is captured, and
the stream restarts automatically. Captures are saved as
`<prefix>_YYYYMMDD-HHMMSS.<ext>` in `--capture-dir`.

```bash
./picamera --preview --capture-format jpeg --capture-dir ~/photos
```

No systemd unit is needed — just run `--preview` (e.g. from a tty or an
autostart script). Ctrl+C exits cleanly and releases the camera.

## Flashing the Pi (first-time setup)

`make flash` prints step-by-step instructions for flashing Raspberry Pi OS via `rpiboot` (USB mass-storage mode) and configuring SSH + WiFi + the IMX477 overlay on the boot partition. You'll need to copy `config/wpa_supplicant.conf.example` to `config/wpa_supplicant.conf` and edit it with your WiFi credentials first — the real file is gitignored.

## Testing

Unit tests cover the pure-logic parts of the codebase (no camera hardware needed): NV12→RGB conversion, CLI argument parsing, the output writers (PPM/PNG/raw/JPEG/DNG round-trip), the DNG/TIFF writer, the LiPo battery SOC curve + ADS1115, the font renderer, and the timelapse filename-pattern validator (including the format-string-vulnerability rejections).

```bash
make test             # build + run unit tests (ctest)
make test-sanitize    # build + run under ASan + UBSan with leak detection
make tidy             # run clang-tidy on src/ (ignores system-header warnings)
```

The test binary (`build/picamera_tests`) uses a tiny hand-rolled runner in `tests/test_runner.h` — no external test framework dependency. Sanitizers are opt-in via the CMake option `-DPICAMERA_ENABLE_SANITIZERS=ON`.

CI (`.github/workflows/ci.yml`) runs the native build + tests + sanitizers + clang-tidy on x86-64, and a Docker cross-build for aarch64 (uploading `picamera-arm64` as an artifact).

## Project layout

```
.
├── CMakeLists.txt         CMake build (C++20, libcamera/libpng/libjpeg/libgpiod via pkg-config)
├── CMakePresets.json      native + asan configure/build/test presets
├── Makefile               build / clean / test / test-sanitize / tidy / deploy / cross-build / flash
├── .clang-tidy            clang-tidy checks (correctness-focused, style noise disabled)
├── .github/workflows/ci.yml  CI: build+test+sanitize+tidy (x86-64) + cross-build (aarch64)
├── config/
│   └── wpa_supplicant.conf.example   WiFi config template (copy + edit before flashing)
├── scripts/
│   └── cross-build.sh     Docker + qemu-user-static aarch64 cross-build
├── tests/
│   ├── test_runner.h      Minimal test framework (TEST/CHECK/REQUIRE, no deps)
│   ├── test_main.cpp      Runner: executes all registered tests
│   ├── test_image.cpp     NV12->RGB conversion tests
│   ├── test_output.cpp    PPM / PNG / raw writer round-trip tests
│   ├── test_output_writer.cpp  OutputWriter factory + per-format writer tests
│   ├── test_cli.cpp       Argument parsing tests
│   ├── test_timelapse.cpp Filename pattern validation tests
│   ├── test_dng.cpp       DNG/TIFF writer tests
│   └── test_battery.cpp   LiPo SOC curve + font renderer tests
└── src/
    ├── main.cpp           Entry point: parse args, init camera, dispatch mode
    ├── camera_config.h    OutputFormat enum + CameraConfig struct
    ├── camera.{h,cpp}     CameraApp: init/configure/capture/bracket/timelapse/listControls
    ├── cli.{h,cpp}        Arg parsing with typed, exception-safe numeric conversion
    ├── image.{h,cpp}      NV12 -> RGB24 / RGB565 conversion (NEON SIMD + multi-threaded, BT.601)
    ├── encoders.{h,cpp}    Low-level encoders: writePng/writePpm/writeRaw/writeJpeg/writeJpegRgb
    ├── output_writer.{h,cpp}  OutputWriter Strategy + factory (per-format writers)
    ├── timelapse.{h,cpp}  Filename pattern formatting + validation
    ├── dng.{h,cpp}        DNG/TIFF raw Bayer writer with EXIF metadata
    ├── preview.{h,cpp}    Live SPI LCD preview loop (ST7735S viewfinder + joystick capture)
    ├── stream.{h,cpp}     CameraStream: continuous Viewfinder-role NV12 streaming
    ├── display.{h,cpp}    ST7735S SPI display driver (spidev + libgpiod)
    ├── buttons.{h,cpp}    GPIO button/joystick input (libgpiod v2 edge events)
    ├── battery.{h,cpp}    ADS1115 I2C ADC battery monitor + LiPo SOC curve
    └── font.{h,cpp}       5x7 bitmap font + battery icon renderer for RGB565 overlays
```

## Troubleshooting

**Black / tiny PNG output.** Increase `--warmup` (default 8). In low light, AE needs more frames to converge. See "Why warmup frames matter" above.

**`Capture timed out (completed N/N frames)`.** The save path (NV12->RGB + PNG encode at 12 MP) is CPU-bound on the Pi Zero 2 W and can take ~40 s. The internal deadline is 60 s; if you hit it, the file may be truncated. Use `--format ppm` for fastest saves, or lower the resolution.

**`No cameras found`.** Check the IMX477 overlay is in `/boot/firmware/config.txt` (`dtoverlay=imx477,gpu_mem=256`) and reboot. Verify with `libcamera-hello --list-cameras` if installed.

**`Failed to acquire camera`.** Another process (e.g. a leftover `picamera` or `libcamera-hello`) is holding the camera. Kill it and retry.

**`Preview: libgpiod was not available at build time.`** `--preview` was built without libgpiod. Rebuild on the Pi with `libgpiod-dev` installed.

**`configure()` failures.** The Pi VC4 ISP pipeline supports a limited set of resolutions/strides. If you request an unusual size, libcamera's `validate()` may adjust it; check the `Configured: WxH stride:S` log line for the actual negotiated values.
