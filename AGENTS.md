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
  make test           # ctest must pass (37 pure-logic tests)
  make tidy           # clang-tidy: zero warnings on src/
  ```
  Run `make test-sanitize` for any change touching memory/buffer/CLI parsing.
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
