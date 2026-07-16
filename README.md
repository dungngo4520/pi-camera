# pi-camera

A small, dependency-light C++20 libcamera front-end for the **Raspberry Pi HQ Camera (IMX477)** on a **Pi Zero 2 W**. Captures stills and timelapses at full sensor resolution (4056x3040) and saves them as PPM, PNG, or raw NV12. Ships with a GPIO button daemon for headless one-button capture.

## Hardware

| Part | Notes |
|---|---|
| Raspberry Pi Zero 2 W | aarch64, Raspberry Pi OS Lite (Debian 13 trixie) |
| Raspberry Pi HQ Camera (IMX477) | 12.3 MP, 4056x3040 native, 7.9 mm C-mount |
| C-mount lens | any CS/C-mount lens via the HQ Camera's C-CS adapter |
| Push button | wired to GPIO17 + GND (see `scripts/button-daemon.py`) |

Required `config.txt` overlay on the Pi boot partition:

```
dtoverlay=imx477,gpu_mem=256
```

## Software requirements (on the Pi)

```
sudo apt install build-essential cmake pkg-config libcamera-dev libpng-dev
```

Python side (button daemon only):

```
sudo apt install python3-gpiod
```

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
| `--format <type>` | `ppm` | Output format: `ppm`, `raw`, `png` |
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
| Raw NV12 | `.raw` | ~18 MB | Y plane + UV plane as captured by the ISP. No color conversion. |

## Why warmup frames matter

libcamera's auto-exposure (AE) and auto-white-balance (AWB) algorithms need **several frames to converge** — they measure the scene, then adjust exposure and gain over successive frames. If you capture a single frame with no warmup, AE hasn't measured anything yet and the ISP applies a near-zero exposure, producing a **black frame**.

This was a real bug in earlier versions of this project: a 12 MP PNG came out at 36 KB because every pixel was `(0,0,0)` — PNG compresses uniform regions to almost nothing. The fix queues all available buffers, discards the first `--warmup` frames (re-queuing their buffers with `Request::ReuseBuffers`), and saves only the converged frame. Default is 8; bump to 15+ in low light.

Reference: a real 12 MP photo is typically:
- 2-6 MB as JPEG (lossy)
- 10-25 MB as PNG (lossless)
- ~37 MB as uncompressed RGB

If your PNG is suspiciously small (under ~1 MB at full res), it's almost certainly a black or near-uniform frame — increase `--warmup`.

## GPIO button daemon

`scripts/button-daemon.py` watches a push button on GPIO17 (falling edge, internal pull-up) and runs `scripts/capture.sh` on each press. The ACT LED blinks on success, stays solid for 1.5 s on failure.

```bash
# Install as a systemd service (run as root for LED + GPIO access)
sudo cp scripts/button-daemon.py /usr/local/bin/
sudo cp scripts/capture.sh /usr/local/bin/

# /etc/systemd/system/picamera-button.service:
# [Unit]
# Description=Pi Camera Button Daemon
# After=local-fs.target
#
# [Service]
# ExecStart=/usr/local/bin/button-daemon.py
# Restart=on-failure
# User=root
#
# [Install]
# WantedBy=multi-user.target

sudo systemctl enable --now picamera-button
```

`capture.sh` defaults to `~/camera/build/picamera` and `~/photos/`. Use `--quick` for 1920x1080, `--full` for 4056x3040, `--dir <path>` to override the output directory.

## Flashing the Pi (first-time setup)

`make flash` prints step-by-step instructions for flashing Raspberry Pi OS via `rpiboot` (USB mass-storage mode) and configuring SSH + WiFi + the IMX477 overlay on the boot partition. You'll need to copy `config/wpa_supplicant.conf.example` to `config/wpa_supplicant.conf` and edit it with your WiFi credentials first — the real file is gitignored.

## Testing

Unit tests cover the pure-logic parts of the codebase (no camera hardware needed): NV12→RGB conversion, CLI argument parsing, the output writers (PPM/PNG/raw round-trip), and the timelapse filename-pattern validator (including the format-string-vulnerability rejections).

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
├── CMakeLists.txt         CMake build (C++20, libcamera + libpng via pkg-config)
├── Makefile               build / clean / test / test-sanitize / tidy / deploy / cross-build / flash
├── .clang-tidy            clang-tidy checks (correctness-focused, style noise disabled)
├── .github/workflows/ci.yml  CI: build+test+sanitize+tidy (x86-64) + cross-build (aarch64)
├── config/
│   └── wpa_supplicant.conf.example   WiFi config template (copy + edit before flashing)
├── scripts/
│   ├── button-daemon.py   GPIO17 button watcher -> capture.sh, with ACT LED feedback
│   └── capture.sh         Wrapper: picks resolution, timestamps filename, reports size
├── tests/
│   ├── test_runner.h      Minimal test framework (TEST/CHECK/REQUIRE, no deps)
│   ├── test_main.cpp      Runner: executes all registered tests
│   ├── test_image.cpp     NV12->RGB conversion tests
│   ├── test_output.cpp    PPM / PNG / raw writer round-trip tests
│   ├── test_cli.cpp       Argument parsing tests
│   └── test_timelapse.cpp Filename pattern validation tests
└── src/
    ├── main.cpp           Entry point: parse args, init camera, dispatch mode
    ├── camera.{h,cpp}     CameraApp: init/configure/capture/timelapse/listControls
    ├── cli.{h,cpp}        Arg parsing with typed, exception-safe numeric conversion
    ├── image.{h,cpp}      NV12 -> RGB24 conversion (scalar, BT.601 limited-range)
    ├── timelapse.{h,cpp}  Filename pattern formatting + validation
    └── output.{h,cpp}     PPM / PNG / raw NV12 writers (stream-state verified)
```

## Troubleshooting

**Black / tiny PNG output.** Increase `--warmup` (default 8). In low light, AE needs more frames to converge. See "Why warmup frames matter" above.

**`Capture timed out (completed N/N frames)`.** The save path (NV12->RGB + PNG encode at 12 MP) is CPU-bound on the Pi Zero 2 W and can take ~40 s. The internal deadline is 60 s; if you hit it, the file may be truncated. Use `--format ppm` for fastest saves, or lower the resolution.

**`No cameras found`.** Check the IMX477 overlay is in `/boot/firmware/config.txt` (`dtoverlay=imx477,gpu_mem=256`) and reboot. Verify with `libcamera-hello --list-cameras` if installed.

**`Failed to acquire camera`.** Another process (e.g. a leftover `picamera` or `libcamera-hello`) is holding the camera. Kill it and retry.

**Permission errors writing to `/sys/class/leds/ACT/brightness`.** The button daemon needs root for LED control. Run via systemd as `User=root`, or accept that LED feedback is disabled (the daemon logs a warning and continues).

**`configure()` failures.** The Pi VC4 ISP pipeline supports a limited set of resolutions/strides. If you request an unusual size, libcamera's `validate()` may adjust it; check the `Configured: WxH stride:S` log line for the actual negotiated values.
