#include "preview.h"
#include "camera.h"
#include "cli.h"
#include "image.h"
#include "image_decode.h"
#include "stop_flag.h"
#include "dual_stream.h"
#include "camera_mode.h"
#include "safe_path.h"
#include "preview_helpers.h"

#ifdef HAVE_GPIOD
#include "display.h"
#include "buttons.h"
#include "font.h"
#endif

#include <iostream>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <ctime>
#include <filesystem>

namespace picamera {

namespace {

// --- Preview-mode constants ---
constexpr int kFrameTimeoutMs = 2000;
constexpr int kBatteryReadIntervalSec = 3;
constexpr uint32_t kStatsIntervalFrames = 60;

#ifdef HAVE_GPIOD

// Splash screen duration (ms)
constexpr int kSplashDurationMs = 1500;
// Review screen duration (ms) — how long the last capture is shown
constexpr int kReviewDurationMs = 2000;
// Inactivity timeout before the display backlight is turned off (auto-power-off).
constexpr int kSleepTimeoutSec = 30;
// Shutter press duration threshold: below = capture (quick tap), at/above = metering lock.
constexpr int kShutterHoldMs = 500;
// Number of playback entries visible without scrolling.
constexpr int kPlaybackVisible = 8;
// Number of items in the on-camera settings menu.
constexpr int kSettingsCount = 7;
// Named indices for the settings menu (matches the order drawn by
// drawSettingsScreen). Using named constants prevents silent breakage
// when settings are reordered or added.
enum SettingIndex : int {
    kSettingFormat = 0,
    kSettingJpegQuality = 1,
    kSettingGrid = 2,
    kSettingHistogram = 3,
    kSettingBrightness = 4,
    kSettingTimer = 5,
    kSettingExit = 6,
};

// Non-blocking capture: queues the still and launches a background thread
// to wait for completion. The viewfinder keeps streaming. The caller polls
// captureDone to check completion, then reads captureSuccess/captureFilename.
// Like a real camera: shutter fires, "CAPTURING..." shows briefly, VF continues.
//
// Returns the worker thread (joinable). The caller MUST join it before
// destroying the referenced arguments (cam, the atomics, the strings) —
// typically by calling joinCaptureThread() before shutdown/reconfigure.
// On early-failure paths (no disk space, filename collision, queue
// failure) no thread is launched and a non-joinable std::thread is
// returned; captureDone is set to true and errorMsg is filled in.
//
// Thread-safety: errorMsg, captureFilename are protected by captureMtx.
// The worker thread locks captureMtx when writing the strings, then stores
// captureDone=true (release). The main thread loads captureDone=true
// (acquire), joins the thread, then locks captureMtx to read the strings.
std::jthread captureStillAsync(DualStream &cam, const PreviewConfig &pcfg,
                              const CameraSettings &settings,
                              std::atomic<bool> &captureDone,
                              std::atomic<bool> &captureSuccess,
                              std::string &captureFilename,
                              std::string &errorMsg,
                              std::mutex &captureMtx) {
    captureDone.store(false, std::memory_order_release);
    captureSuccess.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(captureMtx);
        errorMsg.clear();
    }

    // Use the actual still stream format (not the UI setting) to build
    // the filename extension — the file content is determined by stillFmt_,
    // which only changes on reconfigureStill(). This prevents mismatched
    // extensions when the user changes the format in settings but hasn't
    // exited settings yet (which triggers reconfigureStill).
    OutputFormat fmt = cam.stillFormat();

    // Size the disk-space threshold to the capture format:
    // - JPEG/PNG: full-res ~5-10 MB, 50 MB is generous.
    // - DNG/RAW: full-res IMX477 10-bit ~15-20 MB, need 64 MB headroom.
    uint64_t minBytes = (fmt == OutputFormat::DNG || fmt == OutputFormat::RAW_NV12)
                            ? 64ull * 1024 * 1024
                            : 50ull * 1024 * 1024;
    if (!hasDiskSpace(pcfg.captureDir, minBytes)) {
        std::cerr << "Preview: insufficient disk space for capture\n";
        {
            std::lock_guard<std::mutex> lk(captureMtx);
            errorMsg = "CARD FULL";
        }
        captureDone.store(true, std::memory_order_release);
        return {};
    }

    // Start from the CLI camera config (preserves exposure/gain/AWB)
    // and override only the JPEG quality from the settings menu.
    CameraConfig stillCfg = pcfg.cameraCfg;
    stillCfg.jpegQuality = settings.jpegQuality;
    cam.updateStillConfig(stillCfg);

    // Pass the timestamped name straight to the writer; it opens with
    // O_EXCL and, on a collision, appends a _2/_3 suffix atomically,
    // reporting the real path via waitCaptureDone()'s savedPath out-param.
    // Probing with lstat first would be a redundant TOCTOU race that
    // duplicates the writer's own suffix logic.
    std::string filename =
        makeCaptureFilename(pcfg.captureDir, pcfg.capturePrefix, fmt);

    if (filename.empty()) {
        {
            std::lock_guard<std::mutex> lk(captureMtx);
            errorMsg = "BAD FILENAME";
        }
        captureDone.store(true, std::memory_order_release);
        return {};
    }

    if (!cam.captureStill(filename)) {
        {
            std::lock_guard<std::mutex> lk(captureMtx);
            errorMsg = "CAPTURE FAIL";
        }
        captureDone.store(true, std::memory_order_release);
        return {};
    }

    // Launch a background thread to wait for the capture to complete.
    // The viewfinder keeps streaming while this runs. The thread is
    // returned to the caller, which must join it before shutdown so the
    // captured-by-reference locals are not destroyed while it runs.
    // DualStream::stop() notifies stillCv_ so this thread unblocks
    // promptly even if the capture never completes.
    //
    // We ask waitCaptureDone for the actual saved path — the writer may
    // have appended a _2/_3 suffix if the requested filename collided
    // (rare, but possible under burst shooting or same-ms timestamps).
    // Using the real path ensures the review screen decodes the file
    // that was actually written, not a nonexistent one.
    return std::jthread([&cam, &captureDone, &captureSuccess,
                        &captureFilename, &errorMsg, &captureMtx, filename]() {
        constexpr int kStillTimeoutMs = 5000;
        std::string savedPath;
        bool ok = cam.waitCaptureDone(kStillTimeoutMs, &savedPath);
        if (ok) {
            {
                std::lock_guard<std::mutex> lk(captureMtx);
                captureFilename = savedPath.empty() ? filename : savedPath;
            }
            std::cout << "Preview: saved " << (savedPath.empty() ? filename : savedPath) << "\n";
            captureSuccess.store(true, std::memory_order_release);
        } else {
            std::cerr << "Preview: still capture timed out or failed\n";
            {
                std::lock_guard<std::mutex> lk(captureMtx);
                errorMsg = "CAPTURE TIMEOUT";
            }
        }
        captureDone.store(true, std::memory_order_release);
    });
}

// ---------------------------------------------------------------------------
// Preview loop state + extracted helpers.
//
// runPreview() was a ~780-line monolith. The loop body is decomposed into
// focused free functions below, each taking the loop state (PreviewState&) and
// the service objects (DualStream&, St7735Display&, etc.) it needs. This is a
// pure refactor — behavior is identical to the original inline code.
// ---------------------------------------------------------------------------

// All mutable state that lived as locals in runPreview()'s loop, bundled so
// helpers can take a single state reference instead of ~25 loose parameters.
// Pure-data aggregate (no member functions) — mirrors the existing
// OverlayState/CameraSettings pattern.
struct PreviewState {
    // Framebuffer + timing
    size_t dispPixels = 0;
    std::vector<uint8_t> rgb565;
    std::chrono::microseconds frameDelay{50000};
    uint32_t frameCount = 0;
    uint32_t captureCount = 0;

    // Mode FSM
    CameraMode mode = CameraMode::Viewfinder;
    OverlayState overlay;
    OutputFormat capFmt = OutputFormat::JPEG; // active still format (post-reconfigure)
    std::string lastCapturePath;
    std::chrono::steady_clock::time_point reviewStart;

    // Playback browser state
    std::vector<std::string> playbackFiles;
    int playbackIdx = 0;
    int playbackScroll = 0;

    // Image viewer state (decoded image for full-screen view)
    std::vector<uint8_t> imageViewPixels;
    std::string imageViewPath;
    // Delete confirmation: first Key3 press arms a 3s window; a second
    // Key3 press within that window deletes. Epoch = no pending confirm.
    std::chrono::steady_clock::time_point deleteConfirmDeadline;

    // Review image state (decoded last capture for review screen)
    std::vector<uint8_t> reviewPixels;

    // Self-timer state
    bool timerActive = false;
    std::chrono::steady_clock::time_point timerEndTime;

    // Metering lock state (AE/AWB lock via long shutter hold)
    bool meteringLocked = false;

    // Error message with expiry (for on-screen display)
    std::string persistentError;
    std::chrono::steady_clock::time_point errorExpiry;

    // Power management: sleep after inactivity, wake on button press.
    std::chrono::steady_clock::time_point lastActivity;
    bool sleeping = false;

    // Non-blocking capture state (worker thread + shared completion flags).
    std::atomic<bool> captureDone{false};
    std::atomic<bool> captureSuccess{false};
    std::string captureFilename;
    std::jthread captureThread;
    std::string captureErrorMsg;
    std::mutex captureMtx;
    bool captureActive = false;

    // Dirty flag for static modes (Settings/Playback/Review/ImageView).
    bool screenDirty = true;
    CameraMode lastRenderedMode = CameraMode::Splash;

    // Settings menu state
    int settingsIdx = 0;
    SettingsTab settingsTab = SettingsTab::Shooting;

    // Battery
    bool batteryOk = false;
    BatteryReading lastBattery;
    std::chrono::steady_clock::time_point lastBatteryRead;
};

// Canonicalize and verify/create the capture directory. Returns false on
// failure (after logging). Mutates pcfg.captureDir to the canonicalized path.
bool prepareCaptureDir(PreviewConfig &pcfg) {
    {
        std::string canonDir = canonicalizeDir(pcfg.captureDir);
        if (canonDir.empty()) {
            std::cerr << "Preview: invalid capture directory: " << pcfg.captureDir << "\n";
            return false;
        }
        pcfg.captureDir = canonDir;
    }
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(pcfg.captureDir, ec)) {
            if (!fs::create_directories(pcfg.captureDir, ec)) {
                std::cerr << "Preview: capture directory does not exist and"
                          << " could not be created: " << pcfg.captureDir
                          << " (" << ec.message() << ")\n";
                return false;
            }
        }
    }
    return true;
}

// Periodically refresh the battery reading (I2C is slow — every 3s).
void updateBatteryReading(PreviewState &s, BatteryMonitor &battery) {
    if (!s.batteryOk) return;
    auto now = std::chrono::steady_clock::now();
    if (now - s.lastBatteryRead > std::chrono::seconds(kBatteryReadIntervalSec)) {
        s.lastBattery = battery.read();
        s.lastBatteryRead = now;
    }
}

// While sleeping (backlight off, low-power): poll buttons with a blocking
// timeout so we wake promptly on any press. Returns true if the main loop
// should `continue` (skip render + normal button handling to consume the wake).
bool handleSleepPoll(PreviewState &s, ButtonInput &buttons, St7735Display &display) {
    if (!s.sleeping) return false;
    ButtonEvent wakeEvt = buttons.poll(200);
    if (wakeEvt.pressed) {
        s.sleeping = false;
        s.lastActivity = std::chrono::steady_clock::now();
        display.setBacklight(true);
        std::cout << "Preview: waking from sleep\n";
    }
    return true; // skip render + normal button handling (consume wake)
}

// Software dimming: scale RGB565 pixel values by brightness % (the ST7735S
// backlight is on/off only, so we scale pixel values).
void applyBrightnessDimming(uint8_t *rgb565, size_t dispPixels, int displayBrightness) {
    // Clamp to valid range [0, 100] — values >= 100 mean no dimming.
    int scale = std::clamp(displayBrightness, 0, 100);
    if (scale >= 100) return;
    if (dispPixels > SIZE_MAX / 2) return;  // guard overflow on huge displays
    // RGB565: scale each channel. R=5bit, G=6bit, B=5bit.
    for (size_t i = 0; i < dispPixels * 2; i += 2) {
        uint16_t px = (static_cast<uint16_t>(rgb565[i]) << 8) | rgb565[i + 1];
        uint8_t r5 = (px >> 11) & 0x1F;
        uint8_t g6 = (px >> 5) & 0x3F;
        uint8_t b5 = px & 0x1F;
        r5 = static_cast<uint8_t>((r5 * scale) / 100);
        g6 = static_cast<uint8_t>((g6 * scale) / 100);
        b5 = static_cast<uint8_t>((b5 * scale) / 100);
        uint16_t dim = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
        rgb565[i] = static_cast<uint8_t>(dim >> 8);
        rgb565[i + 1] = static_cast<uint8_t>(dim & 0xFF);
    }
}

// Join a completed capture worker and transition to review/error state.
// The acquire load synchronizes with the worker's release store of captureDone.
void checkCaptureCompletion(PreviewState &s, St7735Display &display) {
    if (!s.captureDone.load(std::memory_order_acquire)) return;
    if (s.captureThread.joinable()) s.captureThread.join();
    if (s.captureActive) s.captureActive = false;
    std::string savedPath, errMsg;
    {
        std::lock_guard<std::mutex> lk(s.captureMtx);
        savedPath = s.captureFilename;
        errMsg = s.captureErrorMsg;
    }
    if (s.captureSuccess.load(std::memory_order_acquire)) {
        s.lastCapturePath = savedPath;
        ++s.captureCount;  // increment only on successful capture
        // Decode for review screen (like a real camera)
        s.reviewPixels = decodeImageToRgb565(
            savedPath, display.width(), display.height());
        // Only switch to Review if we're in Viewfinder — if the user
        // navigated to Playback/Settings/ImageView during a long capture,
        // don't yank them out of their current mode.
        if (s.mode == CameraMode::Viewfinder) {
            s.mode = CameraMode::Review;
            s.reviewStart = std::chrono::steady_clock::now();
            s.screenDirty = true;
        }
    } else if (!errMsg.empty()) {
        s.persistentError = errMsg;
        s.errorExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        // Force redraw so the error is visible even in static modes
        // (Review/Playback/Settings) that only render when screenDirty.
        s.screenDirty = true;
    }
    // Reset captureDone so the completion handler doesn't re-trigger next frame.
    s.captureDone.store(false, std::memory_order_release);
}

// Refresh overlay fields from current state + camera metadata (shutter/ISO).
void updateOverlayState(PreviewState &s, DualStream &cam) {
    s.overlay.mode = s.mode;
    s.overlay.captureCount = s.captureCount;
    s.overlay.frameCount = s.frameCount;
    s.overlay.battery = s.lastBattery;
    s.overlay.batteryValid = s.batteryOk && s.lastBattery.valid;
    s.overlay.captureInProgress = s.captureActive;
    s.overlay.lastCapturePath = s.lastCapturePath;
    s.overlay.meteringLocked = s.meteringLocked;
    s.overlay.shutterMs = cam.lastShutterMs();
    s.overlay.iso = cam.lastIso();
    s.overlay.errorMessage.clear();

    // Persistent error (from capture failure etc.) — show for 3 seconds
    if (std::chrono::steady_clock::now() < s.errorExpiry) {
        s.overlay.errorMessage = s.persistentError;
    }
    // Low battery warning — takes priority over transient errors.
    if (s.batteryOk && s.lastBattery.valid && s.lastBattery.percent < 15) {
        s.overlay.errorMessage = "LOW BATTERY";
    }
}

// Advance the self-timer: draw countdown, capture when expired. Defers the
// shot if a previous capture is still in flight (rare edge case).
void handleSelfTimer(PreviewState &s, DualStream &cam, const PreviewConfig &pcfg,
                     St7735Display &display) {
    if (!s.timerActive) {
        s.overlay.timerRemaining = 0;
        return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= s.timerEndTime) {
        if (s.captureActive) {
            // Defer: keep timerActive true so we retry next frame.
            s.overlay.timerRemaining = 1;
        } else {
            // Timer expired — capture.
            s.timerActive = false;
            s.overlay.timerRemaining = 0;
            std::cout << "Preview: self-timer expired — capturing...\n";
            display.flash();
            {
                std::lock_guard<std::mutex> lk(s.captureMtx);
                s.captureErrorMsg.clear();
            }
            s.captureThread = captureStillAsync(cam, pcfg, s.overlay.settings,
                                              s.captureDone, s.captureSuccess,
                                              s.captureFilename, s.captureErrorMsg,
                                              s.captureMtx);
            s.captureActive = s.captureThread.joinable();
        }
    } else {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            s.timerEndTime - now).count() + 1;
        s.overlay.timerRemaining = static_cast<uint32_t>(remaining);
    }
}

// Render one viewfinder frame: grab, convert, dim, overlay, histogram, blit.
// Returns true if the main loop should `continue` (stop requested mid-grab),
// preserving the original early-`continue` on stop during a frame timeout.
bool renderViewfinder(PreviewState &s, DualStream &cam, St7735Display &display,
                      const PreviewConfig &pcfg, const StopFlag &stop) {
    auto frame = cam.grabFrame(kFrameTimeoutMs);
    if (!frame.y()) {
        if (stop.stopRequested()) return true; // -> continue
        std::cerr << "Preview: frame timeout\n";
        // Fall through to button polling instead of skipping the rest of the
        // loop — the user must still be able to press buttons even when the
        // camera stream has stalled.
        return false;
    }
    if (!nv12ToRgb565Scaled(frame.y(), frame.uv(),
                            frame.width, frame.height, frame.stride,
                            frame.yData.size(), frame.uvData.size(),
                            s.rgb565.data(), display.width(), display.height(),
                            s.rgb565.size())) {
        std::cerr << "Preview: nv12ToRgb565Scaled failed — skipping frame\n";
        return false;
    }

    applyBrightnessDimming(s.rgb565.data(), s.dispPixels,
                           s.overlay.settings.displayBrightness);

    // Check capture completion BEFORE building the overlay state, so
    // overlay.captureInProgress reflects the up-to-date value this frame.
    checkCaptureCompletion(s, display);

    // If capture completion switched us out of Viewfinder mode (e.g. to
    // Review), return without blitting — the main loop will render the
    // new mode on the next iteration. This avoids a one-frame viewfinder
    // flash before the review screen appears.
    if (s.mode != CameraMode::Viewfinder) return false;

    updateOverlayState(s, cam);
    handleSelfTimer(s, cam, pcfg, display);

    drawOverlay(s.rgb565.data(), display.width(), display.height(), s.overlay);

    // Live histogram overlay (like a real camera's live histogram)
    if (s.overlay.settings.showHistogram) {
        size_t ySize = frame.yData.size();
        drawHistogram(s.rgb565.data(), display.width(), display.height(),
                      s.rgb565.size(), frame.y(), frame.width, frame.height,
                      frame.stride, ySize);
    }

    if (!display.blit(s.rgb565.data())) {
        std::cerr << "Preview: VF blit failed (SPI error)\n";
    }
    return false;
}

// Render review screen and auto-return to viewfinder after timeout.
void renderReviewMode(PreviewState &s, St7735Display &display) {
    if (s.screenDirty || s.mode != s.lastRenderedMode) {
        drawReviewScreen(s.rgb565.data(), display.width(), display.height(),
                         s.rgb565.size(), s.lastCapturePath,
                         s.reviewPixels.empty() ? nullptr : s.reviewPixels.data(),
                         s.reviewPixels.size());
        if (!display.blit(s.rgb565.data())) {
            std::cerr << "Preview: review blit failed (SPI error)\n";
        }
        s.screenDirty = false;
        s.lastRenderedMode = s.mode;
    }
    // Auto-return to viewfinder after timeout
    if (std::chrono::steady_clock::now() - s.reviewStart >
        std::chrono::milliseconds(kReviewDurationMs)) {
        s.mode = CameraMode::Viewfinder;
        s.screenDirty = true;
    }
}

// Render playback browser (re-blit only when dirty).
void renderPlaybackMode(PreviewState &s, St7735Display &display) {
    if (s.screenDirty || s.mode != s.lastRenderedMode) {
        drawPlaybackBrowser(s.rgb565.data(), display.width(), display.height(),
                           s.playbackFiles, s.playbackIdx, s.playbackScroll);
        if (!display.blit(s.rgb565.data())) {
            std::cerr << "Preview: playback blit failed (SPI error)\n";
        }
        s.screenDirty = false;
        s.lastRenderedMode = s.mode;
    }
}

// Render full-screen image view; fall back to playback if decode is missing.
void renderImageViewMode(PreviewState &s, St7735Display &display) {
    // If the decoded image is missing, fall back to playback before rendering.
    if (s.imageViewPixels.empty()) {
        s.mode = CameraMode::Playback;
        s.screenDirty = true;
        return;
    }
    if (s.screenDirty || s.mode != s.lastRenderedMode) {
        drawImageView(s.rgb565.data(), display.width(), display.height(),
                      s.rgb565.size(),
                      s.imageViewPixels.data(), s.imageViewPixels.size(),
                      s.imageViewPath);
        if (!display.blit(s.rgb565.data())) {
            std::cerr << "Preview: image view blit failed (SPI error)\n";
        }
        s.screenDirty = false;
        s.lastRenderedMode = s.mode;
    }
}

// Render settings menu (re-blit only when dirty).
void renderSettingsMode(PreviewState &s, St7735Display &display) {
    if (s.screenDirty || s.mode != s.lastRenderedMode) {
        drawSettingsMenu(s.rgb565.data(), display.width(), display.height(),
                        s.overlay.settings, s.settingsTab, s.settingsIdx);
        if (!display.blit(s.rgb565.data())) {
            std::cerr << "Preview: settings blit failed (SPI error)\n";
        }
        s.screenDirty = false;
        s.lastRenderedMode = s.mode;
    }
}

// Check inactivity timeout and enter sleep if exceeded.
void checkSleepTimeout(PreviewState &s, St7735Display &display,
                       std::chrono::steady_clock::time_point now) {
    if (s.mode == CameraMode::Viewfinder &&
        now - s.lastActivity > std::chrono::seconds(kSleepTimeoutSec)) {
        s.sleeping = true;
        display.setBacklight(false);
        std::cout << "Preview: entering sleep mode (inactivity)\n";
    }
}

// Handle shutter release: quick tap = capture (or self-timer start),
// long hold = AE/AWB metering lock (half-press emulation).
void handleShutterRelease(PreviewState &s, DualStream &cam, const PreviewConfig &pcfg,
                          St7735Display &display, const ButtonEvent &evt) {
    if (evt.pressDurationMs >= 0 && evt.pressDurationMs < kShutterHoldMs) {
        // Re-entry guard: ignore the shutter while a capture is already in
        // flight (the worker thread is still saving the previous shot).
        if (s.captureActive) {
            // already capturing — ignore
        } else if (s.overlay.settings.timerDuration > 0 && !s.timerActive) {
            // Start self-timer countdown
            s.timerActive = true;
            s.timerEndTime = std::chrono::steady_clock::now() +
                std::chrono::seconds(s.overlay.settings.timerDuration);
            std::cout << "Preview: self-timer started ("
                      << s.overlay.settings.timerDuration << "s)\n";
        } else if (!s.timerActive) {
            // No timer — capture immediately
            std::cout << "Preview: shutter pressed ("
                      << evt.pressDurationMs << "ms) — capturing...\n";
            display.flash();
            {
                std::lock_guard<std::mutex> lk(s.captureMtx);
                s.captureErrorMsg.clear();
            }
            s.captureThread = captureStillAsync(cam, pcfg, s.overlay.settings,
                                              s.captureDone, s.captureSuccess,
                                              s.captureFilename, s.captureErrorMsg,
                                              s.captureMtx);
            s.captureActive = s.captureThread.joinable();
        }
        // If timer is already active, ignore additional presses
    } else if (evt.pressDurationMs >= kShutterHoldMs) {
        // Long hold — metering lock (AE/AWB lock)
        s.meteringLocked = !s.meteringLocked;
        std::cout << "Preview: metering "
                  << (s.meteringLocked ? "locked" : "unlocked")
                  << " (" << evt.pressDurationMs << "ms hold)\n";
        cam.setMeteringLock(s.meteringLocked);
    }
}

// Dispatch a button event in Viewfinder mode.
void handleViewfinderButton(PreviewState &s, DualStream &cam, const PreviewConfig &pcfg,
                            St7735Display &display, const ButtonEvent &evt) {
    if (evt.id == ButtonId::Shutter && !evt.pressed) {
        handleShutterRelease(s, cam, pcfg, display, evt);
    } else if (evt.pressed && evt.id == ButtonId::Key1) {
        // Enter playback mode
        s.playbackFiles = listCaptures(pcfg.captureDir);
        s.playbackIdx = 0;
        s.playbackScroll = 0;
        s.mode = CameraMode::Playback;
        s.screenDirty = true;
    } else if (evt.pressed && evt.id == ButtonId::Key2) {
        // Enter settings mode
        s.mode = CameraMode::Settings;
        s.settingsIdx = 0;
        s.screenDirty = true;
    } else if (evt.pressed && evt.id == ButtonId::Key3) {
        // Power off = enter sleep mode (display off, low power).
        // Any button press wakes the camera back up — like a real camera's
        // power button toggling on/off, rather than shutting down the OS
        // (which would require a reboot to power on again).
        s.sleeping = true;
        display.setBacklight(false);
        std::cout << "Preview: power-off (sleep mode)\n";
    }
}

// Any button in Review mode returns to viewfinder.
void handleReviewButton(PreviewState &s, const ButtonEvent &evt) {
    if (evt.pressed) {
        s.mode = CameraMode::Viewfinder;
        s.screenDirty = true;
    }
}

// Dispatch a button event in Playback browser mode.
void handlePlaybackButton(PreviewState &s, St7735Display &display, const ButtonEvent &evt) {
    if (evt.pressed && evt.id == ButtonId::Key1) {
        // Exit playback
        s.mode = CameraMode::Viewfinder;
        s.screenDirty = true;
    } else if (evt.pressed && evt.id == ButtonId::JoyUp) {
        if (s.playbackIdx > 0) {
            --s.playbackIdx;
            if (s.playbackIdx < s.playbackScroll) s.playbackScroll = s.playbackIdx;
            s.screenDirty = true;
        }
    } else if (evt.pressed && evt.id == ButtonId::JoyDown) {
        if (s.playbackIdx < static_cast<int>(s.playbackFiles.size()) - 1) {
            ++s.playbackIdx;
            // Auto-scroll if needed
            if (s.playbackIdx >= s.playbackScroll + kPlaybackVisible)
                s.playbackScroll = s.playbackIdx - kPlaybackVisible + 1;
            s.screenDirty = true;
        }
    } else if (evt.pressed && evt.id == ButtonId::Shutter) {
        // View the selected image full-screen.
        if (s.playbackFiles.empty()) return;
        const std::string &sel = s.playbackFiles[s.playbackIdx];
        s.imageViewPixels = decodeImageToRgb565(
            sel, display.width(), display.height());
        s.imageViewPath = sel;
        // Enter ImageView mode regardless of decode success — if the
        // image couldn't be decoded (e.g. DNG/RAW), drawImageView shows
        // a black screen with the filename so the user knows what file
        // they're looking at and can delete it if needed.
        s.mode = CameraMode::ImageView;
        s.screenDirty = true;
        if (s.imageViewPixels.empty()) {
            std::cerr << "Preview: cannot decode " << sel
                      << " (unsupported format or libjpeg missing)\n";
        }
    }
}

// Dispatch a button event in ImageView mode (delete on Key3, back otherwise).
// Delete requires two Key3 presses within 3 seconds to prevent accidental
// data loss from a mis-press — similar to many real cameras' trash flow.
void handleImageViewButton(PreviewState &s, const PreviewConfig &pcfg, const ButtonEvent &evt) {
    if (evt.pressed && evt.id == ButtonId::Key3) {
        if (!s.imageViewPath.empty()) {
            auto now = std::chrono::steady_clock::now();
            // Second press within the confirmation window → delete.
            if (s.deleteConfirmDeadline != std::chrono::steady_clock::time_point{} &&
                now < s.deleteConfirmDeadline) {
                s.deleteConfirmDeadline = {};
                // Canonicalize the delete target and verify it is inside the
                // (already canonicalized) capture directory. This prevents symlink
                // attacks where imageViewPath appears inside captureDir lexically
                // but resolves elsewhere. Delete the original directory entry (not
                // the canonicalized target) so symlinks are removed, not targets.
                std::string canonPath = canonicalizeDir(s.imageViewPath);
                if (canonPath.empty() ||
                    !isCanonicalPathInside(canonPath, pcfg.captureDir)) {
                    std::cerr << "Preview: refusing to delete file outside capture dir: "
                              << s.imageViewPath << "\n";
                    s.persistentError = "DELETE DENIED";
                    s.errorExpiry = now + std::chrono::seconds(3);
                    s.mode = CameraMode::Playback;
                    s.imageViewPixels.clear();
                    s.imageViewPath.clear();
                    s.screenDirty = true;
                    return;
                }
                std::error_code ec;
                if (std::filesystem::remove(s.imageViewPath, ec)) {
                    std::cout << "Preview: deleted " << s.imageViewPath << "\n";
                    // Refresh playback list
                    s.playbackFiles = listCaptures(pcfg.captureDir);
                    if (s.playbackIdx >= static_cast<int>(s.playbackFiles.size()))
                        s.playbackIdx = static_cast<int>(s.playbackFiles.size()) - 1;
                    if (s.playbackIdx < 0) s.playbackIdx = 0;
                    s.mode = CameraMode::Playback;
                    s.imageViewPixels.clear();
                    s.imageViewPath.clear();
                } else {
                    // Delete failed — keep the image on screen and show an error.
                    std::cerr << "Preview: failed to delete " << s.imageViewPath << "\n";
                    s.persistentError = "DELETE FAIL";
                    s.errorExpiry = now + std::chrono::seconds(3);
                }
            } else {
                // First press — arm the confirmation window.
                s.deleteConfirmDeadline = now + std::chrono::seconds(3);
                s.persistentError = "DELETE? KEY3 AGAIN";
                s.errorExpiry = s.deleteConfirmDeadline;
            }
        } else {
            s.deleteConfirmDeadline = {};
            s.mode = CameraMode::Playback;
            s.imageViewPixels.clear();
            s.imageViewPath.clear();
        }
        s.screenDirty = true;
    } else if (evt.pressed) {
        // Any other button cancels a pending delete confirmation and
        // returns to playback browser.
        s.deleteConfirmDeadline = {};
        s.mode = CameraMode::Playback;
        s.imageViewPixels.clear();
        s.imageViewPath.clear();
        s.screenDirty = true;
    }
}

// Dispatch a button event in Settings mode (navigate + adjust values, exit
// reconfigures the still stream if the capture format changed).
void handleSettingsButton(PreviewState &s, DualStream &cam, const PreviewConfig &pcfg,
                          StopFlag &stop, const ButtonEvent &evt) {
    if (evt.pressed && evt.id == ButtonId::Key2) {
        // Exit settings — reconfigure camera if capture format changed.
        if (s.overlay.settings.captureFormat != s.capFmt) {
            std::cout << "Preview: reconfiguring for format "
                      << extensionFor(s.overlay.settings.captureFormat) << "\n";
            // Join any in-flight capture worker before reconfiguring:
            // reconfigureStill() calls stop() (wakes the worker via stillCv_)
            // then start(). The worker must be finished before start()
            // re-installs the requestCompleted callback on the same `this`.
            if (s.captureThread.joinable()) {
                cam.stop();
                s.captureThread.join();
            }
            // Clear capture state so the completion handler doesn't fire with
            // stale results after the reconfigure.
            s.captureActive = false;
            s.captureDone.store(false, std::memory_order_release);
            if (!cam.reconfigureStill(pcfg.previewWidth, pcfg.previewHeight,
                                      pcfg.captureWidth, pcfg.captureHeight,
                                      s.overlay.settings.captureFormat)) {
                std::cerr << "Preview: reconfigure failed — exiting\n";
                stop.requestStop();
                return;
            }
            s.capFmt = s.overlay.settings.captureFormat;
        }
        s.mode = CameraMode::Viewfinder;
        s.screenDirty = true;
    } else if (evt.pressed && evt.id == ButtonId::JoyUp) {
        if (s.settingsIdx > 0) { --s.settingsIdx; s.screenDirty = true; }
    } else if (evt.pressed && evt.id == ButtonId::JoyDown) {
        if (s.settingsIdx < kSettingsCount - 1) { ++s.settingsIdx; s.screenDirty = true; }
    } else if (evt.pressed && evt.id == ButtonId::JoyLeft) {
        // Decrease / cycle left
        switch (s.settingsIdx) {
            case kSettingFormat: { // FORMAT: cycle backward JPEG -> RAW_NV12 -> PPM -> PNG -> DNG -> JPEG
                switch (s.overlay.settings.captureFormat) {
                    case OutputFormat::JPEG:     s.overlay.settings.captureFormat = OutputFormat::RAW_NV12; break;
                    case OutputFormat::RAW_NV12: s.overlay.settings.captureFormat = OutputFormat::PPM; break;
                    case OutputFormat::PPM:      s.overlay.settings.captureFormat = OutputFormat::PNG; break;
                    case OutputFormat::PNG:      s.overlay.settings.captureFormat = OutputFormat::DNG; break;
                    case OutputFormat::DNG:      s.overlay.settings.captureFormat = OutputFormat::JPEG; break;
                }
                s.screenDirty = true;
                break;
            }
            case kSettingJpegQuality: if (s.overlay.settings.jpegQuality > 10) { --s.overlay.settings.jpegQuality; s.screenDirty = true; } break;
            case kSettingGrid: s.overlay.settings.gridType = GridType::Off; s.screenDirty = true; break;
            case kSettingHistogram: s.overlay.settings.showHistogram = false; s.screenDirty = true; break;
            case kSettingBrightness: if (s.overlay.settings.displayBrightness > 10) { s.overlay.settings.displayBrightness -= 10; s.screenDirty = true; } break;
            case kSettingTimer: if (s.overlay.settings.timerDuration > 0) { --s.overlay.settings.timerDuration; s.screenDirty = true; } break;
            // kSettingExit: no JoyLeft action (exit via Key2)
        }
    } else if (evt.pressed && evt.id == ButtonId::JoyRight) {
        // Increase / cycle right
        switch (s.settingsIdx) {
            case kSettingFormat: { // FORMAT: cycle forward JPEG -> DNG -> PNG -> PPM -> RAW_NV12 -> JPEG
                switch (s.overlay.settings.captureFormat) {
                    case OutputFormat::JPEG:     s.overlay.settings.captureFormat = OutputFormat::DNG; break;
                    case OutputFormat::DNG:      s.overlay.settings.captureFormat = OutputFormat::PNG; break;
                    case OutputFormat::PNG:      s.overlay.settings.captureFormat = OutputFormat::PPM; break;
                    case OutputFormat::PPM:      s.overlay.settings.captureFormat = OutputFormat::RAW_NV12; break;
                    case OutputFormat::RAW_NV12: s.overlay.settings.captureFormat = OutputFormat::JPEG; break;
                }
                s.screenDirty = true;
                break;
            }
            case kSettingJpegQuality: if (s.overlay.settings.jpegQuality < 100) { ++s.overlay.settings.jpegQuality; s.screenDirty = true; } break;
            case kSettingGrid: s.overlay.settings.gridType = GridType::Thirds; s.screenDirty = true; break;
            case kSettingHistogram: s.overlay.settings.showHistogram = true; s.screenDirty = true; break;
            case kSettingBrightness: if (s.overlay.settings.displayBrightness < 100) { s.overlay.settings.displayBrightness += 10; s.screenDirty = true; } break;
            case kSettingTimer: if (s.overlay.settings.timerDuration < 10) { ++s.overlay.settings.timerDuration; s.screenDirty = true; } break;
            // kSettingExit: no JoyRight action (exit via Key2)
        }
    }
}

// Main preview loop — extracted from runPreview() for readability.
// Handles per-frame rendering, button events, and power management.
static bool runPreviewLoop(PreviewState &s, DualStream &cam,
                           St7735Display &display, ButtonInput &buttons,
                           BatteryMonitor &battery, StopFlag &stop,
                           const PreviewConfig &pcfg) {
    bool ok = true;
    try {
    while (!stop.stopRequested() && !cam.fatalError()) {
        auto frameStart = std::chrono::steady_clock::now();

        // Update battery reading periodically
        updateBatteryReading(s, battery);

        // While sleeping (backlight off, low-power): poll buttons with a
        // blocking timeout so we wake promptly on any press. We must NOT
        // skip this — otherwise the wake-from-sleep handler is unreachable
        // and the device can never wake up.
        if (handleSleepPoll(s, buttons, display)) continue;

        // Render based on current mode
        switch (s.mode) {
        case CameraMode::Viewfinder:
            if (renderViewfinder(s, cam, display, pcfg, stop)) continue;
            break;
        case CameraMode::Review:
            renderReviewMode(s, display);
            break;
        case CameraMode::Playback:
            renderPlaybackMode(s, display);
            break;
        case CameraMode::ImageView:
            renderImageViewMode(s, display);
            break;
        case CameraMode::Settings:
            renderSettingsMode(s, display);
            break;
        default:
            break;
        }

        ++s.frameCount;
        if (s.frameCount % kStatsIntervalFrames == 0) {
            std::cout << "Preview: " << s.frameCount << " frames, "
                      << s.captureCount << " captures\n";
        }

        // Handle button events based on mode
        ButtonEvent evt = buttons.poll(0);
        if (evt.id == ButtonId::None) {
            // Power management: check for sleep timeout (only when awake —
            // sleeping is handled by the early-continue branch above).
            auto now = std::chrono::steady_clock::now();
            checkSleepTimeout(s, display, now);
            auto elapsed = now - frameStart;
            if (elapsed < s.frameDelay) {
                std::this_thread::sleep_for(s.frameDelay - elapsed);
            }
            continue;
        }

        s.lastActivity = std::chrono::steady_clock::now();

        // Mode-specific button handling
        switch (s.mode) {
        case CameraMode::Viewfinder:
            handleViewfinderButton(s, cam, pcfg, display, evt);
            break;
        case CameraMode::Review:
            handleReviewButton(s, evt);
            break;
        case CameraMode::Playback:
            handlePlaybackButton(s, display, evt);
            break;
        case CameraMode::ImageView:
            handleImageViewButton(s, pcfg, evt);
            break;
        case CameraMode::Settings:
            handleSettingsButton(s, cam, pcfg, stop, evt);
            break;
        default:
            break;
        }

        auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < s.frameDelay) {
            std::this_thread::sleep_for(s.frameDelay - elapsed);
        }
    }
    } catch (const std::bad_alloc &) {
        std::cerr << "Preview: out of memory — shutting down\n";
        ok = false;
    } catch (const std::exception &e) {
        std::cerr << "Preview: unhandled exception in main loop: " << e.what() << "\n";
        ok = false;
    }
    // If the camera hit a fatal error, signal failure so the caller
    // (and systemd Restart=on-failure) can react.
    if (cam.fatalError()) ok = false;
    return ok;
}

#endif // HAVE_GPIOD

} // namespace

bool runPreview(PreviewConfig &pcfg) {
#ifdef HAVE_GPIOD
    // Canonicalize + verify/create the capture directory (symlink-safe).
    if (!prepareCaptureDir(pcfg)) return false;

    St7735Display display;
    if (!display.init(pcfg.displayCfg)) return false;

    ButtonInput buttons;
    if (!buttons.init()) {
        display.shutdown();
        return false;
    }

    // --- Splash screen ---
    PreviewState s;
    s.dispPixels = static_cast<size_t>(display.width()) * display.height();
    s.rgb565.resize(s.dispPixels * 2);
    drawSplash(s.rgb565.data(), display.width(), display.height());
    if (!display.blit(s.rgb565.data())) {
        std::cerr << "Preview: splash blit failed (SPI error)\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSplashDurationMs));

    // DualStream runs viewfinder + still capture simultaneously — no blackout.
    DualStream cam;
    if (!cam.init()) {
        buttons.shutdown();
        display.shutdown();
        return false;
    }
    s.capFmt = parseOutputFormat(pcfg.captureFormat).value_or(OutputFormat::JPEG);
    if (!cam.start(pcfg.previewWidth, pcfg.previewHeight,
                   pcfg.captureWidth, pcfg.captureHeight, s.capFmt)) {
        cam.shutdown();
        buttons.shutdown();
        display.shutdown();
        return false;
    }
    // Apply CLI exposure/gain/AWB settings to the still capture config.
    // Both the viewfinder and still captures share these settings via
    // applyControls() — manual exposure from the CLI affects both.
    cam.updateStillConfig(pcfg.cameraCfg);

    BatteryMonitor battery;
    if (pcfg.enableBattery) {
        s.batteryOk = battery.init(pcfg.batteryCfg);
        if (!s.batteryOk) {
            std::cerr << "Preview: battery monitor init failed"
                      << " — continuing without overlay\n";
        }
    }

    StopFlag stop;
    if (!stop.install()) {
        std::cerr << "Preview: failed to install signal handlers\n";
        if (s.batteryOk) battery.shutdown();
        buttons.shutdown();
        display.shutdown();
        return false;
    }

    s.frameDelay = std::chrono::microseconds(
        pcfg.maxFps > 0 ? 1000000 / pcfg.maxFps : 50000);
    s.overlay.settings.captureFormat = s.capFmt;

    // Initialize time-based state to "long ago" so the first iteration triggers
    // a battery read and so review/timer/error entries are not pre-expired.
    auto initNow = std::chrono::steady_clock::now();
    s.lastBatteryRead = initNow - std::chrono::hours(1);
    s.reviewStart = initNow - std::chrono::hours(1);
    s.timerEndTime = initNow - std::chrono::hours(1);
    s.errorExpiry = initNow - std::chrono::hours(1);
    s.lastActivity = initNow;

    std::cout << "Preview: streaming " << cam.vfWidth() << "x" << cam.vfHeight()
              << " -> display " << display.width() << "x" << display.height()
              << " (max " << pcfg.maxFps << " fps)\n";
    std::cout << "Preview: shutter=capture, Key1=playback, Key2=settings, Ctrl+C=exit\n";

    bool loopOk = runPreviewLoop(s, cam, display, buttons, battery, stop, pcfg);

    // Shutdown: stop the camera first (wakes any blocked waitCaptureDone()
    // via stillCv_.notify_all()), then join the capture worker thread so it
    // doesn't touch freed state, then release the camera handle.
    cam.stop();
    if (s.captureThread.joinable()) s.captureThread.join();

    cam.shutdown();
    buttons.shutdown();
    display.shutdown();
    if (s.batteryOk) battery.shutdown();

    std::cout << "Preview: stopped after " << s.frameCount << " frames, "
              << s.captureCount << " captures\n";
    return loopOk;
#else
    (void)pcfg;
    std::cerr << "Preview: libgpiod was not available at build time.\n"
              << "Rebuild on the Pi with libgpiod-dev installed.\n";
    return false;
#endif
}

PreviewConfig makePreviewConfig(const CliOptions &opts, const CameraConfig &cfg) {
    PreviewConfig pcfg;
    pcfg.displayCfg.spiDevice = opts.spiDevice;
    pcfg.displayCfg.rotation = opts.displayRotation;
    pcfg.previewWidth = opts.previewWidth;
    pcfg.previewHeight = opts.previewHeight;
    pcfg.maxFps = opts.previewFps;
    pcfg.captureWidth = opts.captureWidth;
    pcfg.captureHeight = opts.captureHeight;
    pcfg.captureFormat = opts.captureFormat.empty() ? "jpeg" : opts.captureFormat;
    pcfg.captureDir = opts.captureDir;
    pcfg.capturePrefix = opts.capturePrefix;
    pcfg.cameraCfg = cfg;
    pcfg.enableBattery = opts.enableBattery;
    pcfg.batteryCfg.i2cDevice = opts.batteryI2cDevice;
    pcfg.batteryCfg.i2cAddress = opts.batteryI2cAddress;
    // Use ±6.144V PGA for direct LiPo measurement (3.0-4.2V)
    pcfg.batteryCfg.pgaGain = 0x0000;
    return pcfg;
}

} // namespace picamera
