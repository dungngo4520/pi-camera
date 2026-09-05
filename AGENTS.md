# picamera — build & verification commands

C++20 libcamera front-end for the Raspberry Pi HQ Camera (IMX477) on Pi Zero 2 W.

## Appliance mode

Running `picamera` with no arguments launches preview mode with sensible
defaults (320x240 viewfinder, 4056x3040 JPEG capture, battery overlay). This
is the "mirrorless camera" experience — power on and shoot, no CLI needed.

Install as a systemd service for auto-start on boot:
```bash
sudo make install-service   # on the Pi, after building
```

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
make test               # build + run unit tests via ctest (pure-logic, no camera)
make test-sanitize      # build + run under ASan + UBSan with leak detection
make tidy               # clang-tidy on src/ (zero warnings expected; system-header noise filtered)
```

### Hardware verification (requires Pi connected via SSH)

When a Pi Zero 2 W + IMX477 + Waveshare LCD HAT is connected:

```bash
make hw-test PI_PASS=<pi_password>    # deploy + build + unit tests + JPEG/DNG capture + service restart
make hw-deploy PI_PASS=<pi_password>  # deploy + build only (no tests)
make hw-restart PI_PASS=<pi_password> # restart the picamera service
make hw-status PI_PASS=<pi_password>  # show service status
make hw-logs PI_PASS=<pi_password>    # show recent journal logs
```

Set `PI_HOST` and `PI_USER` if your Pi isn't at `raspberrypi.local` as `pi`.
`hw-test` stops the service before standalone capture tests (frees the camera),
then restarts it. All tests must pass: unit tests + JPEG capture + DNG capture +
service running.

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
  The `make tidy` target and CI filter only fail on warnings in `src/*.cpp` files;
  header warnings (`.clang-tidy` `HeaderFilterRegex: 'src/.*'`) are emitted but not
  enforced, to keep the gate focused on TU-local diagnostics.
- No external test framework; `tests/test_runner.h` is a minimal TEST/CHECK/REQUIRE runner.
- Pure-logic code (image, output, cli, timelapse, safe_path, camera_mode,
  settings_menu, preview_helpers, hardware_config, encoders, wb_utils,
  wifi_server protocol, bt_server protocol) is unit-tested; camera.cpp,
  preview.cpp, display, and battery I2C require hardware.
- `formatTimelapseName` validates `--output` patterns to prevent format-string vulns (see test_timelapse.cpp).
- Wi-Fi (`--wifi`) and Bluetooth (`--bt`) are optional remote-control transports,
  gated by CMake `pkg_check_modules QUIET` (libbluetooth-dev for `--bt`).
  Wire-protocol parsers are unit-tested; socket I/O needs hardware.

## Security hardening

- **Path safety** (`safe_path.{h,cpp}`): all capture filenames built via
  `safeCapturePath()` — rejects `..`, control chars, absolute paths, overlong
  paths. CLI validates `--capture-prefix`, `--spi-device`, `--battery-i2c`,
  `--battery-addr`, `--display-rotate`, and all width/height args.
  Unit-tested in `tests/test_safe_path.cpp`.
- **File creation**: encoders and DNG writer use `O_CREAT|O_EXCL|O_NOFOLLOW`
  with 0640 perms to prevent symlink attacks and accidental overwrites.
- **Integer overflow**: `checkedMul`/`checkedAdd` helpers used in image
  processing, DNG unpack, mmap length, and buffer-size calculations.
- **libjpeg error handling**: `writeJpegRgb` installs a setjmp/longjmp error
  handler so a corrupted buffer returns false instead of calling `exit()`.
- **Systemd unit**: runs as `pi` user (not root), `DevicePolicy=closed` with
  allowlist, `ProtectSystem=full`, `ProtectHome=read-only`, `MemoryDenyWriteExecute`,
  `SystemCallFilter=@system-service`, `NoNewPrivileges`, `UMask=0077`.

## Preview mode (build notes)

`--preview` streams the camera to a Waveshare 1.44" LCD HAT (ST7735S, 128x128, SPI)
and captures full-res JPEGs on joystick press. Key build constraints:

- **libgpiod dependency**: optional at build time (CMake `pkg_check_modules QUIET`).
  Without it, `--preview` prints an error. Install `libgpiod-dev` on the Pi.
- **Pi Zero 2 W memory**: 512MB RAM. Build with `-j1` to avoid OOM.
  Add swap (`sudo fallocate -l 512M /swapfile && mkswap /swapfile && swapon /swapfile`).
- **Build on Pi**: `cmake -DPICAMERA_BUILD_TESTS=OFF .. && make -j1`
- **DualStream**: Viewfinder + StillCapture streams run simultaneously on one
  camera (no stop/restart on capture). HW MJPEG fallback detection preserved.
- **Mode state machine**: Viewfinder / Review / Playback / Settings / Splash,
  navigated via buttons (shutter=BCM13, Key1=BCM21, Key2=BCM20, Key3=BCM16).

## JPEG capture (software fallback)

The Pi VC4 pipeline handler does not support HW MJPEG at 4056x3040 — it silently
falls back to YUYV. The code detects this and reconfigures with NV12, then encodes
JPEG in software via libjpeg-turbo.

- **libjpeg dependency**: optional (CMake `pkg_check_modules QUIET libjpeg`).
  Install `libjpeg-dev` on the Pi. Without it, JPEG capture at full res will fail.
- **Buffer count**: reduced to 1 for high-res NV12 (>2MP) to avoid V4L2 ENOMEM.
- **Detection**: checks `sc.pixelFormat` after both `validate()` and `configure()`.

## Battery monitoring (ADS1115 ADC)

The UPS-Lite V1.2's onboard MAX17040G fuel gauge is a **fake/clone** (I2C 0x32,
genuine = 0x36) — VCELL always reads 0x0000, SOC is garbage. An external
**ADS1115** (16-bit I2C ADC at 0x48) reads the cell voltage directly instead.
Voltage → LiPo SOC via a piecewise-linear discharge curve (±5-10% accuracy).

- **CLI flags**: `--battery`, `--battery-i2c <path>`, `--battery-addr <hex>`
- **Code**: `src/battery.cpp` (ADS1115 I2C + LiPo SOC), `src/font.cpp`
  (battery icon renderer), both unit-tested. No new library dependency
  (raw i2c-dev ioctl).
- **Wiring**: VDD→Pi 5V, GND→Pi GND, SDA→GPIO2, SCL→GPIO3,
  A0→Battery +, ADDR→GND (sets address 0x48). No voltage divider needed
  (PGA gain ±6.144V covers LiPo 3.0-4.2V).

---

# Agent workflow & cooperation rules

These rules apply to **every agent** (Devin CLI, subagents, cloud Devins, and
other AGENTS.md-aware tools) working in this repo. They override defaults when
in conflict. Keep them concise — see the Devin docs on
[rules](https://docs.devin.ai/cli/extensibility/rules) and
[subagents](https://docs.devin.ai/cli/subagents) for the mechanics.

## 1. Plan before acting, track all progress

- **Always make a plan before doing anything.** For any task with more than one
  step, use `todo_write` to break it down before touching files. Surface the
  plan to the user first; in Plan mode, do not edit until approved.
- **Track progress continuously.** Keep exactly one todo `in_progress`, mark
  items `completed` the moment they're done, and add follow-up todos as they're
  discovered. Never batch completions.
- **Investigate before answering.** Use search/read tools to verify the real
  state of the world before confirming a belief or proposing a fix. Don't guess.

## 2. Branch per feature, merge to main when done

- **Never develop directly on `main`.** Before starting any non-trivial change,
  create a branch off `main`:
  - `feat/<short>` — new functionality
  - `fix/<short>` — bug fixes
  - `chore/<short>` — tooling, docs, refactor, rules
  - `exp/<short>` — throwaway experiments
- **All feature work merges to `main` only after the work is done** and
  verified (build + tests + tidy, see §5).
- **One concern per branch.** If a task sprawls, split it into multiple
  branches/PRs rather than mixing unrelated changes.
- **Keep branches short-lived.** Rebase onto `main` before opening a PR.

## 3. If there is in-progress work, ask what to do next

- **At session start, check git state first:** run `git status` and
  `git branch --show-current`. If there are uncommitted changes or an active
  feature branch, **do not start new work** — ask the user whether to:
  1. resume the in-progress work,
  2. commit/stash it first, or
  3. start the new task on a fresh branch.
- **Never abandon or overwrite uncommitted work.** Don't run
  `git checkout`/`reset`/`clean` over dirty state, and don't `rm` files you
  didn't just create, without explicit confirmation for that specific action.
- **If a previous agent left a `.devin/HANDOFF.md`** (see §6), read it before
  doing anything else and follow its resume instructions.

## 4. Commits & PRs

- **Conventional Commits** style: `type(scope): subject` (e.g.
  `feat(preview): add battery overlay`, `fix(camera): handle NV12 fallback`).
  Subject ≤ 72 chars, imperative mood. Body explains *why*, not *what*.
- **Small, focused commits.** Each commit should be reviewable on its own and
  pass build+tests. Don't bundle unrelated changes into one commit.
- **Never commit secrets** (keys, tokens, `.env`, credentials). If you spot one
  already tracked, stop and tell the user.
- **PRs via `gh`.** Title = conventional-commit summary; body has `## Summary`
  bullets and an `#### Test plan` checklist. Reference the branch, not `main`.
- **Co-author trailer** on commits made by Devin:
  `Co-Authored-By: Devin <158243242+devin-ai-integration[bot]@users.noreply.github.com>`

## 5. Verify before marking anything done

- **Run the project's verification before claiming a task is complete.** For
  this repo that means, at minimum:
  ```bash
  make build          # must compile clean (C++20, -Wall -Wextra -Wpedantic)
  make test           # ctest must pass (668 pure-logic tests)
  make tidy           # clang-tidy: zero warnings on src/
  ```
  Run `make test-sanitize` for any change touching memory/buffer/CLI parsing.
  If a Pi is connected, run `make hw-test PI_PASS=...` to verify on hardware.
- **For hardware-only code** (camera.cpp, preview.cpp, display, battery I2C),
  note in the PR that it's untested on x86 and why, and ensure pure-logic
  helpers stay unit-tested.
- **Self-critique edge cases** before declaring done. If verification can't
  run, say so explicitly rather than implying success.

## 6. Handoff state for multi-agent continuity

- **When stopping mid-task, write `.devin/HANDOFF.md`** so the next agent can
  resume without re-deriving context. Include:
  - Current branch and what it's for
  - What's done, what's in progress (link to todos/commit hashes)
  - Exact next step and any command to run
  - Blockers, open questions, or things awaiting user input
  - Uncommitted/`git stash` state if any
- **Delete `.devin/HANDOFF.md` once the task is fully merged to `main`** so
  stale handoffs don't mislead future agents.
- **`/handoff` to cloud Devin** is preferred for long-running or
  browser/VM/CI work — it carries branch + context + uncommitted diff.

## 7. Subagent delegation

- **Use `run_subagent` for self-contained or parallelizable work** to keep the
  main context clean and speed up the task. Match the profile to the work:
  - `subagent_explore` (read-only, cheap SWE model) for research, codebase
    searches, tracing dependencies, "where is X handled?".
  - `subagent_general` (full tools, parent's model) for code changes or
    anything needing write access.
- **Front-load context.** Subagents can't see your conversation or ask
  clarifying questions — give them file paths, names, what you already know,
  and exactly what to return.
- **Keep parallel subagents self-contained** so they don't write to the same
  files. Don't fan out into many `subagent_general` calls on a premium model
  without reason — cost scales with count.
- **Don't use a subagent for trivial single-step work** (one read, one grep,
  one edit) — do it directly.

## 8. Ambiguity & user interaction

- **When a request is unclear:** first try to interpret it from context and
  codebase evidence; if still uncertain, ask **one focused question** with
  concrete options via `ask_user_question`. Don't guess and don't pepper the
  user with many small questions.
- **Confirm destructive operations** (force-push, history rewrite, branch
  delete, `rm -rf`, DB drops, sending emails, payments) before running — even
  if a prior approval seemed related. A new destructive action needs its own
  confirmation.
- **Never push, merge to `main`, or force-push without explicit user approval
  for that specific action.** "Don't push unless asked" is the default.

## 9. Code conventions (recap)

- C++20, `-Wall -Wextra -Wpedantic`, clang-tidy clean (see `.clang-tidy`).
- No external test framework — `tests/test_runner.h` is the minimal runner.
- Pure-logic code is unit-tested; hardware code is not.
- Mimic existing patterns/libraries in the file you're editing — don't pull in
  a new dependency without checking `CMakeLists.txt` and neighboring files
  first. Prefer `pkg_check_modules QUIET` for optional deps.
- Don't add/remove comments unless asked. If you accidentally delete one,
  restore it.
- Compact, idiomatic code; error handling at the right boundary, not
  try/catch on every line.

## 10. Useful slash commands & references

- `/plan` — Plan mode: explore + propose, no edits until approved.
- `/ask <q>` — one-shot question, no code changes.
- `/handoff [task]` — hand off to a cloud Devin session.
- `/compact` — force context compaction when the session gets long.
- `/hooks` — list loaded hooks (treat hook feedback as user feedback).
- Docs: <https://docs.devin.ai/cli> — rules, subagents, handoff, permissions.
