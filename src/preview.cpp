#include "preview.h"
#include "bt_server.h"
#include "camera.h"
#include "camera_mode.h"
#include "cli.h"
#include "dual_stream.h"
#include "encoders.h"
#include "hardware_config.h"
#include "image.h"
#include "image_decode.h"
#include "image_effects.h"
#include "preview_helpers.h"
#include "safe_path.h"
#include "settings_menu.h"
#include "stop_flag.h"
#include "video_config.h"
#include "wifi_server.h"

#include "buttons.h"
#include "display.h"
#include "font.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace picamera {

namespace {

// Preview-mode constants.
constexpr int kFrameTimeoutMs = 2000;
constexpr int kBatteryReadIntervalSec = 3;
constexpr uint32_t kStatsIntervalFrames = 60;
constexpr int kSplashDurationMs = 1500;
constexpr int kReviewDurationMs = 2000;
// Below = capture (quick tap), at/above = metering lock.
constexpr int kShutterHoldMs = 500;
constexpr int kPlaybackVisible = 8;
// Pi Zero CPU limit for continuous shooting.
constexpr int kMaxBurstFrames = 3;
constexpr int kSettingsTabCount = 6;

// Forward declaration for DngJpeg+JPEG capture helper.
std::jthread captureDngJpegAsync(DualStream &cam, const PreviewConfig &pcfg,
                                 const CameraSettings &settings,
                                 std::atomic<bool> &captureDone,
                                 std::atomic<bool> &captureSuccess,
                                 std::string &captureFilename,
                                 std::string &errorMsg, std::mutex &captureMtx,
                                 uint64_t exposureOverride);

std::jthread captureStillAsync(
    DualStream &cam, const PreviewConfig &pcfg, const CameraSettings &settings,
    std::atomic<bool> &captureDone, std::atomic<bool> &captureSuccess,
    std::string &captureFilename, std::string &errorMsg, std::mutex &captureMtx,
    float evOverride, float wbRedOverride, float wbBlueOverride,
    uint64_t exposureOverride, float gainOverride) {
  captureDone.store(false, std::memory_order_release);
  captureSuccess.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(captureMtx);
    errorMsg.clear();
  }

  // Use the actual still stream format (not the UI setting) for the filename
  // extension — the file content is determined by stillFmt_, which only
  // changes on reconfigureStill().
  OutputFormat fmt = cam.stillFormat();

  // DngJpeg single shots use sequential DNG+JPEG capture; bracket/burst
  // (non-zero EV/WB overrides) fall through to JPEG-only.
  if (fmt == OutputFormat::DngJpeg && evOverride == 0.0f &&
      wbRedOverride == 0.0f) {
    return captureDngJpegAsync(cam, pcfg, settings, captureDone, captureSuccess,
                               captureFilename, errorMsg, captureMtx,
                               exposureOverride);
  }

  // Disk-space threshold by format: DNG/RAW/DngJpeg/RawJpeg need 64 MB,
  // JPEG/PNG need 50 MB.
  uint64_t minBytes =
      (fmt == OutputFormat::DNG || fmt == OutputFormat::RAW_NV12 ||
       fmt == OutputFormat::DngJpeg || fmt == OutputFormat::RawJpeg)
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

  // Start from the settings menu config (preserves exposure/gain/AWB),
  // merge CLI-only jpegQuality, and apply bulb-mode exposure override.
  CameraConfig stillCfg =
      settingsToCameraConfig(settings, pcfg.captureWidth, pcfg.captureHeight);
  stillCfg.jpegQuality = settings.jpegQuality;
  if (exposureOverride > 0) {
    stillCfg.exposureTime = exposureOverride;
    stillCfg.aeEnable = false;
  }
  cam.updateStillConfig(stillCfg);

  // Bracketing overrides (EV/WB/gain) applied by applyControls() for the
  // still request and cleared after the capture completes.
  cam.setStillEvOverride(evOverride);
  if (wbRedOverride > 0.0f) {
    cam.setStillWbOverride(wbRedOverride, wbBlueOverride);
  } else {
    cam.setStillWbOverride(0.0f, 0.0f);
  }
  cam.setStillGainOverride(gainOverride);

  // Build the capture filename (with optional date subfolder).
  std::string capDir =
      ensureDateSubfolder(pcfg.captureDir, settings.useDateSubfolders);
  std::string filename;
  if (settings.fileNamingMode == FileNamingMode::Sequential) {
    filename = makeSequentialFilename(capDir, pcfg.capturePrefix, fmt);
  } else {
    filename = makeCaptureFilename(capDir, pcfg.capturePrefix, fmt);
  }

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

  // Background thread waits for capture completion while the VF keeps
  // streaming. DualStream::stop() notifies stillCv_ so it unblocks promptly.
  // We ask waitCaptureDone for the actual saved path (may have a _2/_3
  // suffix on filename collision) so the review screen decodes the real file.
  return std::jthread([&cam, &captureDone, &captureSuccess, &captureFilename,
                       &errorMsg, &captureMtx, filename]() {
    constexpr int kStillTimeoutMs = 5000;
    std::string savedPath;
    bool ok = cam.waitCaptureDone(kStillTimeoutMs, &savedPath);
    if (ok) {
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        captureFilename = savedPath.empty() ? filename : savedPath;
      }
      std::cout << "Preview: saved "
                << (savedPath.empty() ? filename : savedPath) << "\n";
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

// Sequential DNG+JPEG capture (true mirrorless RAW+JPEG): captures NV12
// still→JPEG (VF keeps streaming), reconfigures to raw Bayer→DNG (brief VF
// blackout), then reconfigures back. Both files are valid.
std::jthread captureDngJpegAsync(DualStream &cam, const PreviewConfig &pcfg,
                                 const CameraSettings &settings,
                                 std::atomic<bool> &captureDone,
                                 std::atomic<bool> &captureSuccess,
                                 std::string &captureFilename,
                                 std::string &errorMsg, std::mutex &captureMtx,
                                 uint64_t exposureOverride) {
  captureDone.store(false, std::memory_order_release);
  captureSuccess.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(captureMtx);
    errorMsg.clear();
  }

  // Check disk space (need room for both DNG + JPEG).
  if (!hasDiskSpace(pcfg.captureDir, 64ull * 1024 * 1024)) {
    std::cerr << "Preview: insufficient disk space for DNG+JPEG capture\n";
    {
      std::lock_guard<std::mutex> lk(captureMtx);
      errorMsg = "CARD FULL";
    }
    captureDone.store(true, std::memory_order_release);
    return {};
  }

  CameraConfig stillCfg =
      settingsToCameraConfig(settings, pcfg.captureWidth, pcfg.captureHeight);
  stillCfg.jpegQuality = settings.jpegQuality;
  // Bulb mode: override the exposure time with the user-measured value.
  if (exposureOverride > 0) {
    stillCfg.exposureTime = exposureOverride;
    stillCfg.aeEnable = false;
  }
  cam.updateStillConfig(stillCfg);

  // Build the JPEG filename (primary; extensionFor(DngJpeg) = "jpg").
  std::string capDir =
      ensureDateSubfolder(pcfg.captureDir, settings.useDateSubfolders);
  std::string jpegFilename;
  if (settings.fileNamingMode == FileNamingMode::Sequential) {
    jpegFilename = makeSequentialFilename(capDir, pcfg.capturePrefix,
                                          OutputFormat::DngJpeg);
  } else {
    jpegFilename =
        makeCaptureFilename(capDir, pcfg.capturePrefix, OutputFormat::DngJpeg);
  }
  if (jpegFilename.empty()) {
    {
      std::lock_guard<std::mutex> lk(captureMtx);
      errorMsg = "BAD FILENAME";
    }
    captureDone.store(true, std::memory_order_release);
    return {};
  }

  // Derive the DNG filename inside the thread from the actual saved JPEG path
  // (may carry a _2/_3 suffix) so the companion DNG matches the written JPEG.
  uint32_t vfW = pcfg.previewWidth;
  uint32_t vfH = pcfg.previewHeight;
  uint32_t capW = pcfg.captureWidth;
  uint32_t capH = pcfg.captureHeight;

  return std::jthread([&cam, &captureDone, &captureSuccess, &captureFilename,
                       &errorMsg, &captureMtx, jpegFilename, vfW, vfH, capW,
                       capH, &settings, exposureOverride]() {
    constexpr int kStillTimeoutMs = 10000; // generous for full-res + DNG

    // Phase 1: Capture JPEG (NV12 still stream — VF keeps streaming).
    std::string jpegSavedPath;
    bool jpegOk = false;
    if (cam.captureStill(jpegFilename)) {
      jpegOk = cam.waitCaptureDone(kStillTimeoutMs, &jpegSavedPath);
    }
    if (!jpegOk) {
      std::cerr << "Preview: DNG+JPEG — JPEG phase failed\n";
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        errorMsg = "JPEG FAIL";
      }
      captureDone.store(true, std::memory_order_release);
      return;
    }
    std::cout << "Preview: DNG+JPEG — JPEG saved, capturing DNG...\n";

    const std::string &jpegPath =
        jpegSavedPath.empty() ? jpegFilename : jpegSavedPath;
    auto se = splitPathStemExt(jpegPath);
    std::string dngFilename = se.stem + ".dng";

    // Phase 2: Reconfigure to raw Bayer (DNG). VF pauses during this.
    if (!cam.reconfigureStill(vfW, vfH, capW, capH, OutputFormat::DNG)) {
      std::cerr << "Preview: DNG+JPEG — reconfigure to DNG failed\n";
      // JPEG was saved successfully, so report partial success.
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        captureFilename = jpegSavedPath.empty() ? jpegFilename : jpegSavedPath;
        errorMsg = "DNG RECONFIG FAIL";
      }
      captureSuccess.store(true, std::memory_order_release);
      captureDone.store(true, std::memory_order_release);
      return;
    }

    // Re-apply still config (reconfigure resets it).
    CameraConfig dngCfg = settingsToCameraConfig(settings, capW, capH);
    if (exposureOverride > 0) {
      dngCfg.exposureTime = exposureOverride;
      dngCfg.aeEnable = false;
    }
    cam.updateStillConfig(dngCfg);

    // Phase 3: Capture DNG (raw Bayer stream).
    std::string dngSavedPath;
    bool dngOk = false;
    if (cam.captureStill(dngFilename)) {
      dngOk = cam.waitCaptureDone(kStillTimeoutMs, &dngSavedPath);
    }

    // Phase 4: Reconfigure back to DngJpeg (NV12 still). VF resumes.
    // Even if the DNG capture failed, we must reconfigure back so the
    // camera is in a usable state for subsequent captures.
    if (!cam.reconfigureStill(vfW, vfH, capW, capH, OutputFormat::DngJpeg)) {
      std::cerr << "Preview: DNG+JPEG — reconfigure back failed — exiting\n";
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        errorMsg = "RECONFIG FAIL";
      }
      captureDone.store(true, std::memory_order_release);
      return;
    }
    // Re-apply still config (reconfigure resets it).
    CameraConfig backCfg = settingsToCameraConfig(settings, capW, capH);
    if (exposureOverride > 0) {
      backCfg.exposureTime = exposureOverride;
      backCfg.aeEnable = false;
    }
    cam.updateStillConfig(backCfg);

    if (dngOk) {
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        captureFilename = jpegSavedPath.empty() ? jpegFilename : jpegSavedPath;
      }
      std::cout << "Preview: saved DNG+JPEG: "
                << (dngSavedPath.empty() ? dngFilename : dngSavedPath) << " + "
                << (jpegSavedPath.empty() ? jpegFilename : jpegSavedPath)
                << "\n";
      captureSuccess.store(true, std::memory_order_release);
    } else {
      std::cerr << "Preview: DNG+JPEG — DNG phase failed (JPEG saved)\n";
      {
        std::lock_guard<std::mutex> lk(captureMtx);
        captureFilename = jpegSavedPath.empty() ? jpegFilename : jpegSavedPath;
        errorMsg = "DNG FAIL";
      }
      // JPEG was saved, so report partial success for the review screen.
      captureSuccess.store(true, std::memory_order_release);
    }
    captureDone.store(true, std::memory_order_release);
  });
}

// All mutable state from runPreview()'s loop, bundled so helpers take a
// single state reference instead of ~25 loose parameters.
struct PreviewState {
  // Framebuffer + timing
  size_t dispPixels = 0;
  uint32_t dispW = 0;
  uint32_t dispH = 0;
  std::vector<uint8_t> rgb565;
  std::chrono::microseconds frameDelay{50000};
  uint32_t frameCount = 0;
  uint32_t captureCount = 0;

  // Mode FSM
  CameraMode mode = CameraMode::Viewfinder;
  OverlayState overlay;
  OutputFormat capFmt =
      OutputFormat::JPEG; // active still format (post-reconfigure)
  SensorMode prevSensorMode = SensorMode::Auto;
  std::string lastCapturePath;
  std::chrono::steady_clock::time_point reviewStart;

  // Playback browser state
  std::vector<std::string> playbackFiles;
  int playbackIdx = 0;
  int playbackScroll = 0;
  // Image view zoom: 1=1x, 2=2x, 4=4x. Pan offset in source pixels.
  int imageViewZoom = 1;
  int imageViewPanX = 0;
  int imageViewPanY = 0;
  bool slideshowActive = false;
  std::chrono::steady_clock::time_point slideshowNextAdvance;

  std::vector<uint8_t> imageViewPixels;
  std::string imageViewPath;
  // Delete confirmation: first Key3 press arms a 3s window; a second
  // Key3 press within that window deletes.
  std::chrono::steady_clock::time_point deleteConfirmDeadline;

  // Review image state (decoded last capture for review screen)
  std::vector<uint8_t> reviewPixels;

  // Self-timer state
  bool timerActive = false;
  std::chrono::steady_clock::time_point timerEndTime;

  // Bulb mode state: first shutter press starts a counting-up timer;
  // second press captures with the elapsed time as the exposure.
  bool bulbActive = false;
  std::chrono::steady_clock::time_point bulbStartTime;

  // Timelapse state
  std::chrono::steady_clock::time_point timelapseNextCapture;
  int timelapseShotsTaken = 0;

  // Burst/bracket remaining frames
  int burstRemaining = 0;
  int bracketIndex = 0; // current bracket frame index
  BracketType bracketMode =
      BracketType::AE; // bracket type for current sequence

  // Metering lock state (AE/AWB lock via long shutter hold)
  bool meteringLocked = false;

  // Focus magnifier pan offset (viewfinder source pixels). Clamped in
  // renderViewfinder() so the crop region stays within the frame.
  int focusMagPanX = 0;
  int focusMagPanY = 0;

  // True when the currently displayed image is protected.
  bool fileProtected = false;

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
  SettingsTab settingsTab = SettingsTab::Capture;

  // Battery
  bool batteryOk = false;
  BatteryReading lastBattery;
  std::chrono::steady_clock::time_point lastBatteryRead;

  // Wi-Fi server
  WifiServer wifiServer;
  std::mutex wifiSettingsMtx;
  std::atomic<bool> wifiCaptureRequest{false};
  std::atomic<int> wifiBatteryPercent{0};
  std::atomic<uint32_t> wifiCaptureCount{0};

  // Bluetooth server (shares wifiSettingsMtx + overlay.settings)
  BtServer btServer;
  std::atomic<bool> btCaptureRequest{false};

  // Rating state (ImageView mode). Current rating of the displayed image.
  int fileRating = 0;

  // Format/reset confirmation state (Settings mode).
  std::chrono::steady_clock::time_point formatConfirmDeadline;
  std::chrono::steady_clock::time_point resetConfirmDeadline;

  // HDR bracket capture state: stores bracket frame paths for merging.
  std::vector<std::string> bracketCapturePaths;
  bool bracketMergePending = false;

  // Dark frame capture state for long-exposure NR.
  std::string darkFramePath;
  bool darkFramePending = false;

  // Copyright text entry state (Settings mode).
  bool copyrightEditing = false;
  std::string copyrightBuffer;
  int copyrightCursor = 0;

  // Quick/Fn overlay state (Viewfinder mode).
  bool quickFnActive = false;
  int quickFnIdx = 0;

  // Video recording state (Viewfinder mode, Video drive mode).
  bool videoRecording = false;
  bool videoReconfigured = false; // stream reconfigured to video res
  std::chrono::steady_clock::time_point videoStartTime;
  std::chrono::steady_clock::time_point videoLastFrameTime;
  std::string videoPath;
  int videoFrameCount = 0;
  uint32_t videoTargetW = 0;
  uint32_t videoTargetH = 0;
  int videoEffectiveFps = 30;
};

struct BracketOverride {
  float ev, wbRed, wbBlue, gain;
};
BracketOverride computeBracketOverride(const PreviewState &s, int index) {
  BracketOverride o{0.0f, 0.0f, 0.0f, 0.0f};
  float ev = s.overlay.settings.bracketEv[index];
  if (s.bracketMode == BracketType::WB) {
    o.wbRed = 1.0f + ev * 0.2f;
    o.wbBlue = 1.0f - ev * 0.2f;
  } else if (s.bracketMode == BracketType::ISO) {
    float baseGain = (s.overlay.settings.analogueGain > 0.0f)
                         ? s.overlay.settings.analogueGain
                         : 1.0f;
    o.gain = baseGain * std::pow(2.0f, ev);
  } else {
    o.ev = ev;
  }
  return o;
}

void launchCapture(PreviewState &s, DualStream &cam, const PreviewConfig &pcfg,
                   St7735Display &display, float evOv, float wbRed,
                   float wbBlue, uint64_t exposureOverride, float gainOv) {
  if (!s.overlay.settings.silentShutter)
    display.flash();
  {
    std::lock_guard<std::mutex> lk(s.captureMtx);
    s.captureErrorMsg.clear();
  }
  s.captureThread = captureStillAsync(
      cam, pcfg, s.overlay.settings, s.captureDone, s.captureSuccess,
      s.captureFilename, s.captureErrorMsg, s.captureMtx, evOv, wbRed, wbBlue,
      exposureOverride, gainOv);
  s.captureActive = s.captureThread.joinable();
}

void handleRemoteCaptureRequest(PreviewState &s, DualStream &cam,
                                const PreviewConfig &pcfg,
                                St7735Display &display,
                                std::atomic<bool> &request, const char *label) {
  if (!request.load(std::memory_order_acquire))
    return;
  request.store(false, std::memory_order_release);
  if (s.captureActive || s.overlay.timelapseRunning || s.timerActive ||
      s.overlay.settings.driveMode == DriveMode::Bulb)
    return;
  std::cout << "Preview: " << label << " capture request — capturing...\n";
  launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f, 0, 0.0f);
}

void exitImageView(PreviewState &s) {
  s.deleteConfirmDeadline = {};
  s.mode = CameraMode::Playback;
  s.imageViewPixels.clear();
  s.imageViewPath.clear();
  s.slideshowActive = false;
  s.imageViewZoom = 1;
  s.imageViewPanX = 0;
  s.imageViewPanY = 0;
}

// Canonicalize and verify/create the capture directory. Returns false on
// failure (after logging). Mutates pcfg.captureDir to the canonicalized path.
bool prepareCaptureDir(PreviewConfig &pcfg) {
  {
    std::string canonDir = canonicalizeDir(pcfg.captureDir);
    if (canonDir.empty()) {
      std::cerr << "Preview: invalid capture directory: " << pcfg.captureDir
                << "\n";
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
                  << " could not be created: " << pcfg.captureDir << " ("
                  << ec.message() << ")\n";
        return false;
      }
    }
  }
  return true;
}

// Periodically refresh the battery reading (I2C is slow — every 3s).
void updateBatteryReading(PreviewState &s, BatteryMonitor &battery) {
  if (!s.batteryOk)
    return;
  auto now = std::chrono::steady_clock::now();
  if (now - s.lastBatteryRead > std::chrono::seconds(kBatteryReadIntervalSec)) {
    s.lastBattery = battery.read();
    s.lastBatteryRead = now;
  }
}

// While sleeping (backlight off): poll buttons with a blocking timeout so we
// wake promptly on any press. Returns true to skip render + normal handling.
bool handleSleepPoll(PreviewState &s, ButtonInput &buttons,
                     St7735Display &display) {
  if (!s.sleeping)
    return false;
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
void applyBrightnessDimming(uint8_t *rgb565, size_t dispPixels,
                            int displayBrightness) {
  // Clamp to valid range [0, 100] — values >= 100 mean no dimming.
  int scale = std::clamp(displayBrightness, 0, 100);
  if (scale >= 100)
    return;
  if (dispPixels > SIZE_MAX / 2)
    return; // guard overflow on huge displays
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

// Merge bracket frame JPEGs into a single HDR JPEG: decode each to RGB24,
// extract Y-planes, merge with hdrMergeY, reconstruct RGB from merged Y +
// middle-frame chroma, save as _HDR.jpg.
void performHdrMerge(PreviewState &s, const PreviewConfig &pcfg) {
  auto &paths = s.bracketCapturePaths;
  if (paths.size() < 2)
    return;
  // Decode all bracket frames to RGB24 and extract Y-planes.
  std::vector<std::vector<uint8_t>> rgbFrames;
  std::vector<std::vector<uint8_t>> yPlanes;
  std::vector<const uint8_t *> yPtrs;
  uint32_t w = 0;
  uint32_t h = 0;
  for (const auto &p : paths) {
    uint32_t fw = 0;
    uint32_t fh = 0;
    auto rgb = decodeJpegFileToRgb24(p, fw, fh);
    if (rgb.empty()) {
      std::cerr << "Preview: HDR merge — failed to decode " << p << "\n";
      return;
    }
    if (w == 0) {
      w = fw;
      h = fh;
    } else if (fw != w || fh != h) {
      std::cerr << "Preview: HDR merge — dimension mismatch\n";
      return;
    }
    // Align width to even for NV12 chroma.
    uint32_t alignedW = (w + 1) & ~1u;
    auto yPlane = rgb24ToY(rgb.data(), w, h, alignedW);
    if (yPlane.empty()) {
      std::cerr << "Preview: HDR merge — Y extraction failed\n";
      return;
    }
    rgbFrames.push_back(std::move(rgb));
    yPlanes.push_back(std::move(yPlane));
    yPtrs.push_back(yPlanes.back().data());
  }
  uint32_t alignedW = (w + 1) & ~1u;
  auto mergedY = hdrMergeY(yPtrs, alignedW, h, alignedW);
  if (mergedY.empty()) {
    std::cerr << "Preview: HDR merge — merge failed\n";
    return;
  }
  // Reconstruct RGB from merged Y using the middle bracket frame's chroma
  // so the HDR result retains color instead of being grayscale.
  size_t midIdx = rgbFrames.size() / 2;
  auto midUv = rgb24ToUv(rgbFrames[midIdx].data(), w, h);
  if (midUv.empty()) {
    std::cerr << "Preview: HDR merge — chroma extraction failed\n";
    return;
  }
  auto mergedRgb = yuvToRgb24(mergedY.data(), midUv.data(), w, h, alignedW, w);
  if (mergedRgb.empty()) {
    std::cerr << "Preview: HDR merge — RGB reconstruction failed\n";
    return;
  }
  // Save as additional HDR JPEG alongside the bracket frames.
  std::string capDir =
      ensureDateSubfolder(pcfg.captureDir, s.overlay.settings.useDateSubfolders);
  std::string hdrName;
  if (s.overlay.settings.fileNamingMode == FileNamingMode::Sequential) {
    hdrName = makeSequentialFilename(capDir, pcfg.capturePrefix,
                                     OutputFormat::JPEG);
  } else {
    hdrName = makeCaptureFilename(capDir, pcfg.capturePrefix, OutputFormat::JPEG);
  }
  // Insert _HDR before the extension.
  auto dot = hdrName.rfind('.');
  if (dot != std::string::npos)
    hdrName.insert(dot, "_HDR");
  else
    hdrName += "_HDR";
  CameraConfig cfg =
      settingsToCameraConfig(s.overlay.settings, pcfg.captureWidth,
                             pcfg.captureHeight);
  std::string savedPath;
  if (writeJpegRgb(mergedRgb.data(), w, h, hdrName, cfg.jpegQuality,
                   &savedPath)) {
    std::cout << "Preview: HDR merge saved " << savedPath << "\n";
    s.lastCapturePath = savedPath;
  } else {
    std::cerr << "Preview: HDR merge — JPEG encode failed\n";
  }
}

// Long-exposure noise reduction: estimate the sensor black level from the
// optical-black border pixels and subtract it as a uniform offset, removing
// fixed-pattern dark current without a separate dark-frame capture (the Pi
// has no mechanical shutter). Re-encodes and overwrites the original JPEG.
void performLenr(PreviewState &s, const std::string &mainPath,
                 const PreviewConfig &pcfg) {
  uint32_t mw = 0;
  uint32_t mh = 0;
  auto mainRgb = decodeJpegFileToRgb24(mainPath, mw, mh);
  if (mainRgb.empty()) {
    std::cerr << "Preview: LENR — decode failed\n";
    return;
  }
  uint32_t alignedW = (mw + 1) & ~1u;
  auto mainY = rgb24ToY(mainRgb.data(), mw, mh, alignedW);
  if (mainY.empty()) {
    std::cerr << "Preview: LENR — Y extraction failed\n";
    return;
  }
  // Estimate black level from a 4-pixel border (optical-black region).
  constexpr uint32_t kBorderWidth = 4;
  uint8_t blackLevel =
      estimateBlackLevel(mainY.data(), mw, mh, alignedW, kBorderWidth);
  if (blackLevel == 0) {
    std::cerr << "Preview: LENR — black level estimation failed\n";
    return;
  }
  auto correctedY =
      subtractBlackLevel(mainY.data(), mw, mh, alignedW, blackLevel);
  if (correctedY.empty()) {
    std::cerr << "Preview: LENR — subtraction failed\n";
    return;
  }
  // Reconstruct RGB from corrected Y + the main frame's chroma.
  auto mainUv = rgb24ToUv(mainRgb.data(), mw, mh);
  if (mainUv.empty()) {
    std::cerr << "Preview: LENR — chroma extraction failed\n";
    return;
  }
  auto correctedRgb =
      yuvToRgb24(correctedY.data(), mainUv.data(), mw, mh, alignedW, mw);
  if (correctedRgb.empty()) {
    std::cerr << "Preview: LENR — RGB reconstruction failed\n";
    return;
  }
  // Overwrite the main JPEG with the corrected version.
  CameraConfig cfg =
      settingsToCameraConfig(s.overlay.settings, pcfg.captureWidth,
                             pcfg.captureHeight);
  std::string newPath;
  if (writeJpegRgb(correctedRgb.data(), mw, mh, mainPath, cfg.jpegQuality,
                   &newPath)) {
    std::cout << "Preview: LENR — corrected " << newPath
              << " (black=" << static_cast<int>(blackLevel) << ")\n";
  } else {
    std::cerr << "Preview: LENR — re-encode failed\n";
  }
}

// Join a completed capture worker and transition to review/error state.
void checkCaptureCompletion(PreviewState &s, DualStream &cam,
                           St7735Display &display,
                           const PreviewConfig &pcfg) {
  if (!s.captureDone.load(std::memory_order_acquire))
    return;
  if (s.captureThread.joinable())
    s.captureThread.join();
  if (s.captureActive)
    s.captureActive = false;
  std::string savedPath;
  std::string errMsg;
  {
    std::lock_guard<std::mutex> lk(s.captureMtx);
    savedPath = s.captureFilename;
    errMsg = s.captureErrorMsg;
  }
  if (s.captureSuccess.load(std::memory_order_acquire)) {
    s.lastCapturePath = savedPath;
    ++s.captureCount; // increment only on successful capture
    // Collect bracket frame paths for HDR merge.
    if (s.bracketMergePending &&
        s.overlay.settings.driveMode == DriveMode::Bracket)
      s.bracketCapturePaths.push_back(savedPath);
    // Decode for review screen (like a real camera)
    s.reviewPixels =
        decodeImageToRgb565(savedPath, display.width(), display.height());
    // HDR merge: when all bracket frames are captured and merge is pending.
    if (s.bracketMergePending && s.burstRemaining <= 0 &&
        !s.overlay.settings.bracketEv.empty() &&
        s.bracketCapturePaths.size() ==
            s.overlay.settings.bracketEv.size()) {
      performHdrMerge(s, pcfg);
      s.bracketMergePending = false;
      s.bracketCapturePaths.clear();
    }
    // Long-exposure NR: for single captures (not bracket/burst) with LENR
    // enabled and shutter > 1s, estimate black level and subtract.
    if (s.overlay.settings.longExposureNr && s.burstRemaining <= 0 &&
        !s.overlay.timelapseRunning &&
        s.overlay.settings.driveMode != DriveMode::Bracket) {
      uint64_t shutterUs = s.overlay.settings.shutterUs;
      if (shutterUs == 0)
        shutterUs = static_cast<uint64_t>(cam.lastShutterMs()) * 1000;
      if (shutterUs > 1000000) {
        std::cout << "Preview: LENR — estimating black level\n";
        performLenr(s, savedPath, pcfg);
      }
    }
    // Only switch to Review if we're in Viewfinder and not in a burst/timelapse
    if (s.mode == CameraMode::Viewfinder && s.burstRemaining <= 0 &&
        !s.overlay.timelapseRunning) {
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
    // On error, cancel any remaining burst/timelapse
    s.burstRemaining = 0;
    s.overlay.timelapseRunning = false;
    s.bracketMergePending = false;
    s.bracketCapturePaths.clear();
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

  // Sync Wi-Fi server atomics with current state
  s.wifiBatteryPercent.store(
      s.batteryOk && s.lastBattery.valid ? s.lastBattery.percent : 0,
      std::memory_order_release);
  s.wifiCaptureCount.store(s.captureCount, std::memory_order_release);
  s.overlay.wifiActive = s.wifiServer.isRunning();
  s.overlay.btActive = s.btServer.isRunning();
}

// Advance the self-timer: draw countdown, capture when expired. Defers the
// shot if a previous capture is still in flight (rare edge case).
void handleSelfTimer(PreviewState &s, DualStream &cam,
                     const PreviewConfig &pcfg, St7735Display &display) {
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
      launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f, 0, 0.0f);
    }
  } else {
    auto remaining =
        std::chrono::duration_cast<std::chrono::seconds>(s.timerEndTime - now)
            .count() +
        1;
    s.overlay.timerRemaining = static_cast<uint32_t>(remaining);
  }
}

// Handle timelapse capture: fires at each interval until count is reached
// or the user presses the shutter to stop.
void handleTimelapse(PreviewState &s, DualStream &cam,
                     const PreviewConfig &pcfg, St7735Display &display) {
  if (!s.overlay.timelapseRunning)
    return;
  if (s.captureActive)
    return; // wait for previous capture to finish

  auto now = std::chrono::steady_clock::now();
  if (now < s.timelapseNextCapture)
    return;

  // Check if we've reached the target count (0 = unlimited)
  if (s.overlay.settings.timelapseCount > 0 &&
      s.timelapseShotsTaken >= s.overlay.settings.timelapseCount) {
    s.overlay.timelapseRunning = false;
    std::cout << "Preview: timelapse complete (" << s.timelapseShotsTaken
              << " shots)\n";
    s.screenDirty = true;
    return;
  }

  std::cout << "Preview: timelapse shot " << (s.timelapseShotsTaken + 1)
            << "\n";
  launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f, 0, 0.0f);
  s.timelapseShotsTaken++;
  s.timelapseNextCapture =
      now + std::chrono::seconds(s.overlay.settings.timelapseInterval);
}

// Render one viewfinder frame: grab, convert, dim, overlay, histogram, blit.
// Returns true to `continue` (stop requested mid-grab).
// Forward declarations for functions defined after renderViewfinder.
void adjustQuickFnItem(PreviewState &s, int idx, int direction);
void buildQuickFnItems(const CameraSettings &s,
                       std::vector<std::string> &labels,
                       std::vector<std::string> &values);

bool renderViewfinder(PreviewState &s, DualStream &cam, St7735Display &display,
                      const PreviewConfig &pcfg, const StopFlag &stop) {
  auto frame = cam.grabFrame(kFrameTimeoutMs);
  if (!frame.y()) {
    if (stop.stopRequested())
      return true; // -> continue
    std::cerr << "Preview: frame timeout\n";
    // Fall through to button polling instead of skipping the rest of the
    // loop — the user must still be able to press buttons even when the
    // camera stream has stalled.
    return false;
  }
  if (s.overlay.settings.focusMagnify > 0) {
    // Focus magnifier: crop a region of the viewfinder and scale up.
    // 2x: crop center half (srcW/2 x srcH/2), 4x: crop center quarter.
    uint32_t mag = static_cast<uint32_t>(s.overlay.settings.focusMagnify);
    uint32_t cropW = (frame.width / mag) & ~1u;
    uint32_t cropH = (frame.height / mag) & ~1u;
    // Base crop position is center; apply pan offset.
    uint32_t cropX = ((frame.width - cropW) / 2) & ~1u;
    uint32_t cropY = ((frame.height - cropH) / 2) & ~1u;
    // Clamp pan so the crop region stays within the frame.
    // Pan range: ±(frame.width - cropW)/2 X, ±(frame.height - cropH)/2 Y.
    int maxPanX = static_cast<int>((frame.width - cropW) / 2);
    int maxPanY = static_cast<int>((frame.height - cropH) / 2);
    s.focusMagPanX = std::clamp(s.focusMagPanX, -maxPanX, maxPanX);
    s.focusMagPanY = std::clamp(s.focusMagPanY, -maxPanY, maxPanY);
    cropX = static_cast<uint32_t>(
                std::max(0, static_cast<int>(cropX) + s.focusMagPanX)) &
            ~1u;
    cropY = static_cast<uint32_t>(
                std::max(0, static_cast<int>(cropY) + s.focusMagPanY)) &
            ~1u;
    if (!nv12ToRgb565CroppedScaled(
            frame.y(), frame.uv(), frame.width, frame.height, frame.stride,
            frame.yData.size(), frame.uvData.size(), cropX, cropY, cropW, cropH,
            s.rgb565.data(), display.width(), display.height(),
            s.rgb565.size())) {
      std::cerr
          << "Preview: nv12ToRgb565CroppedScaled failed — skipping frame\n";
      return false;
    }
    s.overlay.focusMagnify = s.overlay.settings.focusMagnify;
  } else {
    if (!nv12ToRgb565Scaled(
            frame.y(), frame.uv(), frame.width, frame.height, frame.stride,
            frame.yData.size(), frame.uvData.size(), s.rgb565.data(),
            display.width(), display.height(), s.rgb565.size())) {
      std::cerr << "Preview: nv12ToRgb565Scaled failed — skipping frame\n";
      return false;
    }
    s.overlay.focusMagnify = 0;
    s.focusMagPanX = 0;
    s.focusMagPanY = 0;
  }

  applyBrightnessDimming(s.rgb565.data(), s.dispPixels,
                         s.overlay.settings.displayBrightness);

  // Night mode: boost viewfinder brightness by scaling RGB565 pixel values.
  // This helps visibility in low-light conditions on the ST7735S display.
  if (s.overlay.settings.nightMode) {
    for (size_t i = 0; i < s.dispPixels * 2; i += 2) {
      uint16_t px = (static_cast<uint16_t>(s.rgb565[i]) << 8) | s.rgb565[i + 1];
      uint8_t r5 = (px >> 11) & 0x1F;
      uint8_t g6 = (px >> 5) & 0x3F;
      uint8_t b5 = px & 0x1F;
      r5 = static_cast<uint8_t>(std::min(31, r5 * 3 / 2));
      g6 = static_cast<uint8_t>(std::min(63, g6 * 3 / 2));
      b5 = static_cast<uint8_t>(std::min(31, b5 * 3 / 2));
      uint16_t boosted = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
      s.rgb565[i] = static_cast<uint8_t>(boosted >> 8);
      s.rgb565[i + 1] = static_cast<uint8_t>(boosted & 0xFF);
    }
  }

  if (s.overlay.settings.aspectRatio != AspectRatio::Native) {
    drawAspectRatioMask(s.rgb565.data(), display.width(), display.height(),
                        s.overlay.settings.aspectRatio);
  }

  // Check capture completion before building overlay state so
  // overlay.captureInProgress is current this frame.
  checkCaptureCompletion(s, cam, display, pcfg);

  // If capture completion switched us out of Viewfinder (e.g. to Review),
  // return without blitting — avoids a one-frame VF flash before review.
  if (s.mode != CameraMode::Viewfinder)
    return false;

  updateOverlayState(s, cam);
  handleSelfTimer(s, cam, pcfg, display);
  handleTimelapse(s, cam, pcfg, display);

  handleRemoteCaptureRequest(s, cam, pcfg, display, s.wifiCaptureRequest,
                             "Wi-Fi");
  handleRemoteCaptureRequest(s, cam, pcfg, display, s.btCaptureRequest, "BT");

  // Bulb mode: update the counting-up timer overlay while the exposure
  // is "open" (between the first and second shutter press).
  if (s.bulbActive) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - s.bulbStartTime)
                       .count();
    s.overlay.bulbSeconds = static_cast<uint32_t>(elapsed);
  } else {
    s.overlay.bulbSeconds = 0;
  }

  // One-touch custom WB: measure the live frame's average chroma and update
  // manual R/B gains (armed by the WBSET settings action). Switches AWB off.
  if (s.overlay.settings.wbMeasurePending) {
    float red = 1.0f;
    float blue = 1.0f;
    if (computeWbGainsFromNv12(frame.uv(), frame.width, frame.height,
                               frame.uvData.size(), red, blue)) {
      s.overlay.settings.wbRedGain = red;
      s.overlay.settings.wbBlueGain = blue;
      s.overlay.settings.awbEnable = false;
      std::cout << "Preview: custom WB set R=" << red << " B=" << blue << "\n";
      s.persistentError = "WB SET";
      s.errorExpiry =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
    } else {
      std::cerr << "Preview: custom WB measure failed\n";
      s.persistentError = "WB FAIL";
      s.errorExpiry =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    s.overlay.settings.wbMeasurePending = false;
    s.screenDirty = true;
  }

  // Fire next burst/bracket frame if remaining and previous is done
  if (s.burstRemaining > 0 && !s.captureActive && !s.overlay.timelapseRunning) {
    float evOv = 0.0f;
    float wbRed = 0.0f;
    float wbBlue = 0.0f;
    float gainOv = 0.0f;
    if (s.overlay.settings.driveMode == DriveMode::Bracket &&
        !s.overlay.settings.bracketEv.empty() &&
        s.bracketIndex <
            static_cast<int>(s.overlay.settings.bracketEv.size())) {
      auto bo = computeBracketOverride(s, s.bracketIndex);
      evOv = bo.ev;
      wbRed = bo.wbRed;
      wbBlue = bo.wbBlue;
      gainOv = bo.gain;
      s.bracketIndex++;
    }
    std::cout << "Preview: burst frame (" << s.burstRemaining
              << " remaining)\n";
    launchCapture(s, cam, pcfg, display, evOv, wbRed, wbBlue, 0, gainOv);
    s.burstRemaining--;
  }

  drawOverlay(s.rgb565.data(), display.width(), display.height(), s.overlay);

  if (s.overlay.settings.showHistogram) {
    size_t ySize = frame.yData.size();
    drawHistogram(s.rgb565.data(), display.width(), display.height(),
                  s.rgb565.size(), frame.y(), frame.width, frame.height,
                  frame.stride, ySize);
  }

  // Zebra stripes (overexposure blinkies) — drawn from the Y plane.
  if (s.overlay.settings.zebraMode != ZebraMode::Off) {
    drawZebra(s.rgb565.data(), display.width(), display.height(),
              s.rgb565.size(), frame.y(), frame.width, frame.height,
              frame.stride, frame.yData.size(),
              zebraThreshold(s.overlay.settings.zebraMode));
  }

  if (s.overlay.settings.focusPeaking) {
    drawFocusPeaking(s.rgb565.data(), display.width(), display.height(),
                     s.rgb565.size(), frame.y(), frame.width, frame.height,
                     frame.stride, frame.yData.size());
  }

  // Quick/Fn overlay (drawn on top of everything else when active).
  if (s.quickFnActive) {
    std::vector<std::string> labels;
    std::vector<std::string> values;
    buildQuickFnItems(s.overlay.settings, labels, values);
    drawQuickFnOverlay(s.rgb565.data(), display.width(), display.height(),
                       labels, values, s.quickFnIdx);
  }

  // Video recording: throttle to the selected FPS, scale each frame to the
  // output resolution, and encode. MJPEG/H264 write JPEG frames to .mjpeg;
  // YUV writes raw NV12 planes to .yuv.
  if (s.videoRecording && !s.videoPath.empty()) {
    auto now = std::chrono::steady_clock::now();
    auto interval = videoFrameInterval(s.videoEffectiveFps);
    if (now - s.videoLastFrameTime >= interval) {
      s.videoLastFrameTime = now;
      uint32_t vw = frame.width;
      uint32_t vh = frame.height;
      uint32_t vStride = frame.stride;
      if (vw > 0 && vh > 0 && frame.y() && frame.uv()) {
        std::vector<uint8_t> yCopy(
            frame.y(),
            frame.y() + static_cast<size_t>(vStride) * vh);
        std::vector<uint8_t> uvCopy(
            frame.uv(),
            frame.uv() + static_cast<size_t>(vStride) * (vh / 2));
        uint32_t targetW = s.videoTargetW;
        uint32_t targetH = s.videoTargetH;
        if (targetW == 0 || targetH == 0) {
          targetW = vw;
          targetH = vh;
        }
        if (s.overlay.settings.videoCodec == VideoCodec::YUV) {
          // Raw NV12: crop the Y and UV planes to the target resolution
          // (the stream may be larger due to ISP alignment). Bitrate is
          // ignored for raw YUV.
          uint32_t cropW = std::min(targetW, vw);
          uint32_t cropH = std::min(targetH, vh);
          std::vector<uint8_t> yOut(static_cast<size_t>(cropW) * cropH);
          std::vector<uint8_t> uvOut(
              static_cast<size_t>(cropW) * (cropH / 2));
          for (uint32_t r = 0; r < cropH; ++r)
            std::memcpy(yOut.data() + static_cast<size_t>(r) * cropW,
                        yCopy.data() + static_cast<size_t>(r) * vStride,
                        cropW);
          for (uint32_t r = 0; r < cropH / 2; ++r)
            std::memcpy(uvOut.data() + static_cast<size_t>(r) * cropW,
                        uvCopy.data() + static_cast<size_t>(r) * vStride,
                        cropW);
          std::ofstream ofs(s.videoPath,
                            std::ios::binary | std::ios::app);
          if (ofs) {
            ofs.write(reinterpret_cast<const char *>(yOut.data()),
                      static_cast<std::streamsize>(yOut.size()));
            ofs.write(reinterpret_cast<const char *>(uvOut.data()),
                      static_cast<std::streamsize>(uvOut.size()));
            ++s.videoFrameCount;
          }
        } else if (videoCodecUsesJpeg(s.overlay.settings.videoCodec)) {
          // MJPEG (and H264 fallback): convert to RGB24, scale to the
          // target resolution, then JPEG-encode at the bitrate-mapped
          // quality and append to the .mjpeg container.
          auto rgb = yuvToRgb24(yCopy.data(), uvCopy.data(), vw, vh,
                                vStride, vStride);
          if (!rgb.empty()) {
            if (rgb.size() !=
                static_cast<size_t>(targetW) * targetH * 3) {
              auto scaled = scaleRgb24Bilinear(rgb.data(), vw, vh,
                                                targetW, targetH);
              if (!scaled.empty())
                rgb = std::move(scaled);
            }
            int quality = videoBitrateToJpegQuality(
                s.overlay.settings.videoBitrate);
            std::string tmpPath = s.videoPath + ".tmp.jpg";
            if (writeJpegRgb(rgb.data(), targetW, targetH, tmpPath,
                             quality, nullptr)) {
              std::ifstream jf(tmpPath, std::ios::binary);
              if (jf) {
                std::ofstream ofs(s.videoPath,
                                  std::ios::binary | std::ios::app);
                ofs << jf.rdbuf();
                ++s.videoFrameCount;
              }
              std::error_code ec;
              std::filesystem::remove(tmpPath, ec);
            }
          }
        }
      }
    }
  }

  // Video recording indicator.
  if (s.videoRecording) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - s.videoStartTime)
                       .count();
    drawRecIndicator(s.rgb565.data(), display.width(), display.height(),
                     static_cast<uint32_t>(elapsed));
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
  // Slideshow auto-advance: check if it's time to advance to the next image.
  if (s.slideshowActive) {
    auto now = std::chrono::steady_clock::now();
    if (now >= s.slideshowNextAdvance) {
      // Advance to next image
      if (!s.playbackFiles.empty()) {
        s.playbackIdx =
            (s.playbackIdx + 1) % static_cast<int>(s.playbackFiles.size());
        const std::string &sel = s.playbackFiles[s.playbackIdx];
        s.imageViewPixels =
            decodeImageToRgb565(sel, display.width(), display.height());
        s.imageViewPath = sel;
        s.imageViewZoom = 1;
        s.imageViewPanX = 0;
        s.imageViewPanY = 0;
        s.slideshowNextAdvance = now + std::chrono::seconds(3);
        s.screenDirty = true;
      }
    } else {
      // Redraw periodically to update the countdown timer
      s.screenDirty = true;
    }
  }
  if (s.screenDirty || s.mode != s.lastRenderedMode) {
    drawImageViewZoomed(s.rgb565.data(), display.width(), display.height(),
                        s.rgb565.size(), s.imageViewPixels.data(),
                        s.imageViewPixels.size(), display.width(),
                        display.height(), s.imageViewZoom, s.imageViewPanX,
                        s.imageViewPanY, s.imageViewPath,
                        s.overlay.settings.rotateTall, s.fileRating);
    // Show protection indicator if the file is protected.
    if (s.fileProtected) {
      drawProtectionIndicator(s.rgb565.data(), display.width(),
                              display.height());
    }
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
    if (s.copyrightEditing) {
      drawCopyrightEditOverlay(s.rgb565.data(), display.width(),
                               display.height(), s.copyrightBuffer,
                               s.copyrightCursor);
    }
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
  int timeout = s.overlay.settings.powerSaveTimeout;
  if (timeout <= 0)
    return; // 0 = never sleep
  if (s.mode == CameraMode::Viewfinder &&
      now - s.lastActivity > std::chrono::seconds(timeout)) {
    s.sleeping = true;
    display.setBacklight(false);
    std::cout << "Preview: entering sleep mode (inactivity)\n";
  }
}

// Handle shutter release: quick tap = capture (or self-timer start),
// long hold = AE/AWB metering lock. Dispatches by drive mode.
void handleShutterRelease(PreviewState &s, DualStream &cam,
                          const PreviewConfig &pcfg, St7735Display &display,
                          const ButtonEvent &evt) {
  // Video mode: shutter toggles recording.
  if (s.overlay.settings.driveMode == DriveMode::Video) {
    if (s.videoRecording) {
      s.videoRecording = false;
      std::cout << "Preview: video recording stopped (" << s.videoFrameCount
                << " frames, " << s.videoPath << ")\n";
      s.persistentError = "REC STOP";
      s.errorExpiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(2);
      // Restore the viewfinder stream to the original resolution.
      if (s.videoReconfigured) {
        s.videoReconfigured = false;
        uint32_t capW = pcfg.captureWidth;
        uint32_t capH = pcfg.captureHeight;
        auto smDims = sensorModeDims(s.overlay.settings.sensorMode);
        if (smDims.width > 0 && smDims.height > 0) {
          capW = smDims.width;
          capH = smDims.height;
        }
        if (s.captureThread.joinable()) {
          cam.stop();
          s.captureThread.join();
        }
        s.captureActive = false;
        s.captureDone.store(false, std::memory_order_release);
        if (!cam.reconfigureStill(pcfg.previewWidth, pcfg.previewHeight,
                                  capW, capH, s.capFmt)) {
          std::cerr << "Preview: video restore reconfigure failed\n";
        }
      }
      // Restore the main loop frame rate to the viewfinder default.
      s.frameDelay = std::chrono::microseconds(
          pcfg.maxFps > 0 ? 1000000 / pcfg.maxFps : 50000);
    } else {
      // Start recording — create video file in capture directory.
      std::string capDir = ensureDateSubfolder(
          pcfg.captureDir, s.overlay.settings.useDateSubfolders);
      std::string vidName;
      if (s.overlay.settings.fileNamingMode == FileNamingMode::Sequential) {
        vidName = makeSequentialFilename(capDir, pcfg.capturePrefix,
                                         OutputFormat::JPEG);
      } else {
        vidName = makeCaptureFilename(capDir, pcfg.capturePrefix,
                                      OutputFormat::JPEG);
      }
      // Replace .jpg extension with codec-appropriate extension. H264
      // falls back to MJPEG encoding (no HW H264 encoder via libcamera on
      // Pi Zero 2 W), so it uses the .mjpeg container — never write JPEG
      // frames to a .h264 file (invalid H.264 bitstream).
      const char *ext =
          videoCodecExtension(s.overlay.settings.videoCodec);
      auto dot = vidName.rfind('.');
      if (dot != std::string::npos)
        vidName = vidName.substr(0, dot) + ext;
      else
        vidName += ext;
      // Resolve the target output resolution and clamp the frame rate to
      // what the closest sensor mode supports.
      auto dims =
          videoResolutionDims(s.overlay.settings.videoResolution);
      s.videoTargetW = dims.width;
      s.videoTargetH = dims.height;
      SensorMode sm = videoResolutionToSensorMode(
          s.overlay.settings.videoResolution);
      s.videoEffectiveFps = clampFpsToSensorMode(
          s.overlay.settings.videoFps, sm);
      // Reconfigure the viewfinder stream to the video target resolution
      // so source frames come from the sensor at (or close to) the
      // selected resolution instead of the 320x240 viewfinder. The still
      // stream config is preserved.
      uint32_t capW = pcfg.captureWidth;
      uint32_t capH = pcfg.captureHeight;
      auto smDims = sensorModeDims(s.overlay.settings.sensorMode);
      if (smDims.width > 0 && smDims.height > 0) {
        capW = smDims.width;
        capH = smDims.height;
      }
      if (s.captureThread.joinable()) {
        cam.stop();
        s.captureThread.join();
      }
      s.captureActive = false;
      s.captureDone.store(false, std::memory_order_release);
      if (cam.reconfigureStill(s.videoTargetW, s.videoTargetH, capW, capH,
                               s.capFmt)) {
        s.videoReconfigured = true;
      } else {
        std::cerr << "Preview: video reconfigure failed — using viewfinder"
                  << " stream at " << pcfg.previewWidth << "x"
                  << pcfg.previewHeight << "\n";
      }
      // Increase the main loop frame rate to match the video FPS so the
      // recording loop can capture at the selected rate (the default
      // viewfinder frameDelay limits the loop to 20fps).
      s.frameDelay = videoFrameInterval(s.videoEffectiveFps);
      s.videoPath = vidName;
      s.videoFrameCount = 0;
      s.videoRecording = true;
      s.videoStartTime = std::chrono::steady_clock::now();
      s.videoLastFrameTime = s.videoStartTime;
      std::cout << "Preview: video recording started (" << s.videoPath
                << ", " << s.videoTargetW << "x" << s.videoTargetH << "@"
                << s.videoEffectiveFps << "fps, "
                << videoCodecLabel(s.overlay.settings.videoCodec)
                << ", sensor " << sensorModeLabel(sm) << ")\n";
    }
    s.screenDirty = true;
    return;
  }
  if (evt.pressDurationMs >= 0 && evt.pressDurationMs < kShutterHoldMs) {
    // Re-entry guard: ignore the shutter while a capture is already in
    // flight (the worker thread is still saving the previous shot).
    if (s.captureActive) {
    } else if (s.overlay.timelapseRunning) {
      // Shutter press stops a running timelapse
      s.overlay.timelapseRunning = false;
      s.timelapseNextCapture = {};
      std::cout << "Preview: timelapse stopped by shutter press\n";
      s.screenDirty = true;
    } else if (s.overlay.settings.driveMode == DriveMode::Timelapse) {
      s.overlay.timelapseRunning = true;
      s.timelapseNextCapture = std::chrono::steady_clock::now();
      s.timelapseShotsTaken = 0;
      std::cout << "Preview: timelapse started (interval="
                << s.overlay.settings.timelapseInterval
                << "s, count=" << s.overlay.settings.timelapseCount << ")\n";
      s.screenDirty = true;
    } else if (s.overlay.settings.driveMode == DriveMode::Continuous) {
      // Burst capture: fire kMaxBurstFrames shots in rapid succession
      std::cout << "Preview: burst capture (" << kMaxBurstFrames
                << " frames)\n";
      launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f, 0, 0.0f);
      s.burstRemaining = kMaxBurstFrames - 1;
    } else if (s.overlay.settings.driveMode == DriveMode::Bracket) {
      // Bracket capture: fire one shot per bracketEv entry.
      // AE bracket varies EV per frame; WB bracket varies WB gains;
      // ISO bracket varies analogue gain per frame.
      if (!s.overlay.settings.bracketEv.empty()) {
        std::cout << "Preview: bracket capture ("
                  << s.overlay.settings.bracketEv.size() << " frames)\n";
        s.bracketIndex = 0;
        s.bracketMode = s.overlay.settings.bracketType;
        // Track bracket paths for HDR merge (AE bracket only).
        s.bracketCapturePaths.clear();
        s.bracketMergePending = s.overlay.settings.hdrMerge &&
                                s.bracketMode == BracketType::AE;
        auto bo = computeBracketOverride(s, 0);
        s.bracketIndex = 1; // next frame uses index 1
        launchCapture(s, cam, pcfg, display, bo.ev, bo.wbRed, bo.wbBlue, 0,
                      bo.gain);
        s.burstRemaining =
            static_cast<int>(s.overlay.settings.bracketEv.size()) - 1;
      }
    } else if (s.overlay.settings.driveMode == DriveMode::Bulb) {
      // Bulb mode: two-press long exposure. First press starts a counting-up
      // timer; second press captures with the elapsed time as the exposure.
      if (s.bulbActive) {
        // Second press — capture with the measured exposure time.
        auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - s.bulbStartTime)
                             .count();
        s.bulbActive = false;
        s.overlay.bulbSeconds = 0;
        if (elapsedUs <= 0)
          elapsedUs = 1; // guard: min exposure
        std::cout << "Preview: bulb capture (" << elapsedUs << "us)\n";
        launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f,
                      static_cast<uint64_t>(elapsedUs), 0.0f);
      } else {
        // First press — start the counting-up timer.
        s.bulbActive = true;
        s.bulbStartTime = std::chrono::steady_clock::now();
        std::cout << "Preview: bulb timer started — press again to capture\n";
      }
    } else if (s.overlay.settings.timerDuration > 0 && !s.timerActive) {
      s.timerActive = true;
      s.timerEndTime = std::chrono::steady_clock::now() +
                       std::chrono::seconds(s.overlay.settings.timerDuration);
      std::cout << "Preview: self-timer started ("
                << s.overlay.settings.timerDuration << "s)\n";
    } else if (!s.timerActive) {
      // No timer — capture immediately (Single mode)
      std::cout << "Preview: shutter pressed (" << evt.pressDurationMs
                << "ms) — capturing...\n";
      launchCapture(s, cam, pcfg, display, 0.0f, 0.0f, 0.0f, 0, 0.0f);
    }
    // If timer is already active, ignore additional presses
  } else if (evt.pressDurationMs >= kShutterHoldMs) {
    // Long hold — metering lock (AE/AWB lock)
    s.meteringLocked = !s.meteringLocked;
    std::cout << "Preview: metering "
              << (s.meteringLocked ? "locked" : "unlocked") << " ("
              << evt.pressDurationMs << "ms hold)\n";
    cam.setMeteringLock(s.meteringLocked);
  }
}

// Adjust a Quick/Fn overlay item by direction (+1 or -1).
// Items: 0=ISO, 1=WB, 2=EV, 3=Drive, 4=Format, 5=PictureStyle.
void adjustQuickFnItem(PreviewState &s, int idx, int direction) {
  switch (idx) {
  case 0: // ISO → Exposure tab idx 2
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Exposure, 2, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Exposure, 2, s.overlay.settings);
    break;
  case 1: // WB → Color tab idx 0
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Color, 0, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Color, 0, s.overlay.settings);
    break;
  case 2: // EV → Exposure tab idx 6
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Exposure, 6, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Exposure, 6, s.overlay.settings);
    break;
  case 3: // Drive → Capture tab idx 6
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Capture, 6, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Capture, 6, s.overlay.settings);
    break;
  case 4: // Format → Capture tab idx 0
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Capture, 0, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Capture, 0, s.overlay.settings);
    break;
  case 5: // PictureStyle → Color tab idx 11
    if (direction > 0)
      settingsItemAdjustRight(SettingsTab::Color, 11, s.overlay.settings);
    else
      settingsItemAdjustLeft(SettingsTab::Color, 11, s.overlay.settings);
    break;
  default:
    break;
  }
}

// Build the Quick/Fn overlay labels and values.
void buildQuickFnItems(const CameraSettings &s,
                       std::vector<std::string> &labels,
                       std::vector<std::string> &values) {
  labels = {"ISO", "WB", "EV", "DRIVE", "FMT", "PSTYLE"};
  // ISO
  if (s.analogueGain <= 0)
    values.push_back("AUTO");
  else
    values.push_back(std::to_string(static_cast<int>(s.analogueGain * 100)));
  // WB
  values.push_back(s.awbMode == "auto" ? "AUTO" : s.awbMode.substr(0, 4));
  // EV
  {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%+.1f", s.exposureValue);
    values.push_back(buf);
  }
  // Drive
  switch (s.driveMode) {
  case DriveMode::Single: values.push_back("SINGLE"); break;
  case DriveMode::SelfTimer: values.push_back("TIMER"); break;
  case DriveMode::Bracket: values.push_back("BRACKET"); break;
  case DriveMode::Timelapse: values.push_back("INTERVAL"); break;
  case DriveMode::Continuous: values.push_back("CONT"); break;
  case DriveMode::Bulb: values.push_back("BULB"); break;
  case DriveMode::Video: values.push_back("VIDEO"); break;
  }
  // Format
  values.push_back(std::string(extensionFor(s.captureFormat)));
  // PictureStyle
  switch (s.pictureStyle) {
  case PictureStyle::Standard: values.push_back("STD"); break;
  case PictureStyle::Vivid: values.push_back("VIVID"); break;
  case PictureStyle::Natural: values.push_back("NAT"); break;
  case PictureStyle::Monochrome: values.push_back("MONO"); break;
  case PictureStyle::Portrait: values.push_back("PORTRAIT"); break;
  case PictureStyle::Landscape: values.push_back("LAND"); break;
  case PictureStyle::Sepia: values.push_back("SEPIA"); break;
  case PictureStyle::Cool: values.push_back("COOL"); break;
  case PictureStyle::Warm: values.push_back("WARM"); break;
  case PictureStyle::Film: values.push_back("FILM"); break;
  case PictureStyle::HDR: values.push_back("HDR"); break;
  }
}

void handleViewfinderButton(PreviewState &s, DualStream &cam,
                            const PreviewConfig &pcfg, St7735Display &display,
                            const ButtonEvent &evt) {
  if (evt.id == ButtonId::Shutter && !evt.pressed) {
    handleShutterRelease(s, cam, pcfg, display, evt);
  } else if (evt.pressed && evt.id == ButtonId::Key1) {
    s.bulbActive = false; // cancel any pending bulb exposure
    s.playbackFiles = listCaptures(pcfg.captureDir);
    s.playbackIdx = 0;
    s.playbackScroll = 0;
    s.mode = CameraMode::Playback;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Key2) {
    s.bulbActive = false; // cancel any pending bulb exposure
    s.mode = CameraMode::Settings;
    s.settingsIdx = 0;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Key3) {
    // Power off = enter sleep mode (display off, low power).
    // Any button press wakes the camera back up — like a real camera's
    // power button toggling on/off, rather than shutting down the OS
    // (which would require a reboot to power on again).
    s.bulbActive = false; // cancel any pending bulb exposure
    s.sleeping = true;
    display.setBacklight(false);
    std::cout << "Preview: power-off (sleep mode)\n";
  } else if (!evt.pressed && evt.id == ButtonId::Key3 &&
             evt.pressDurationMs >= 1000) {
    // Long Key3 press: toggle Quick/Fn overlay.
    s.quickFnActive = !s.quickFnActive;
    s.quickFnIdx = 0;
    s.screenDirty = true;
  } else if (evt.pressed && s.quickFnActive &&
             (evt.id == ButtonId::JoyUp || evt.id == ButtonId::JoyDown ||
              evt.id == ButtonId::JoyLeft || evt.id == ButtonId::JoyRight)) {
    // Quick/Fn overlay navigation: Up/Down select item, Left/Right adjust.
    constexpr int kQuickFnCount = 6;
    if (evt.id == ButtonId::JoyUp) {
      if (s.quickFnIdx > 0)
        --s.quickFnIdx;
    } else if (evt.id == ButtonId::JoyDown) {
      if (s.quickFnIdx < kQuickFnCount - 1)
        ++s.quickFnIdx;
    } else if (evt.id == ButtonId::JoyLeft) {
      adjustQuickFnItem(s, s.quickFnIdx, -1);
    } else if (evt.id == ButtonId::JoyRight) {
      adjustQuickFnItem(s, s.quickFnIdx, 1);
    }
    s.screenDirty = true;
  } else if (evt.pressed && s.overlay.settings.focusMagnify > 0 &&
             (evt.id == ButtonId::JoyUp || evt.id == ButtonId::JoyDown ||
              evt.id == ButtonId::JoyLeft || evt.id == ButtonId::JoyRight)) {
    // Focus magnifier pan: joystick moves the crop region. Pan step is 16
    // source pixels; clamping is applied in renderViewfinder().
    constexpr int kFocusPanStep = 16;
    if (evt.id == ButtonId::JoyUp)
      s.focusMagPanY -= kFocusPanStep;
    if (evt.id == ButtonId::JoyDown)
      s.focusMagPanY += kFocusPanStep;
    if (evt.id == ButtonId::JoyLeft)
      s.focusMagPanX -= kFocusPanStep;
    if (evt.id == ButtonId::JoyRight)
      s.focusMagPanX += kFocusPanStep;
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
void handlePlaybackButton(PreviewState &s, const PreviewConfig &pcfg,
                          St7735Display &display, const ButtonEvent &evt) {
  if (evt.pressed && evt.id == ButtonId::Key1) {
    s.mode = CameraMode::Viewfinder;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::JoyUp) {
    if (s.playbackIdx > 0) {
      --s.playbackIdx;
      s.playbackScroll = std::min(s.playbackIdx, s.playbackScroll);
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
    if (s.playbackFiles.empty())
      return;
    const std::string &sel = s.playbackFiles[s.playbackIdx];
    s.imageViewPixels =
        decodeImageToRgb565(sel, display.width(), display.height());
    s.imageViewPath = sel;
    // Reset zoom/slideshow state when entering image view.
    s.imageViewZoom = 1;
    s.imageViewPanX = 0;
    s.imageViewPanY = 0;
    s.slideshowActive = false;
    // Check if the file is protected.
    s.fileProtected = isFileProtected(pcfg.captureDir, sel);
    // Load the file's rating (0 if no .rating sidecar exists).
    s.fileRating = readFileRating(pcfg.captureDir, sel);
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

// ImageView button dispatch. Delete requires two Key3 presses within 3s
// (prevents accidental data loss). Key1 toggles protection, Key2 cycles
// zoom (1x/2x/4x), joystick pans when zoomed, shutter toggles slideshow.
void handleImageViewButton(PreviewState &s, const PreviewConfig &pcfg,
                           const ButtonEvent &evt) {
  if (evt.pressed && evt.id == ButtonId::Key1) {
    if (!s.imageViewPath.empty()) {
      bool nowProtected =
          toggleFileProtection(pcfg.captureDir, s.imageViewPath);
      s.fileProtected = nowProtected;
      s.persistentError = nowProtected ? "PROTECTED" : "UNPROTECTED";
      s.errorExpiry =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      std::cout << "Preview: " << (nowProtected ? "protected " : "unprotected ")
                << s.imageViewPath << "\n";
      s.screenDirty = true;
    }
  } else if (evt.pressed && evt.id == ButtonId::Key3) {
    if (!s.imageViewPath.empty()) {
      auto now = std::chrono::steady_clock::now();
      // Second press within the confirmation window → delete.
      if (s.deleteConfirmDeadline != std::chrono::steady_clock::time_point{} &&
          now < s.deleteConfirmDeadline) {
        s.deleteConfirmDeadline = {};
        // Check if the file is protected — refuse to delete.
        if (isFileProtected(pcfg.captureDir, s.imageViewPath)) {
          std::cerr << "Preview: refusing to delete protected file: "
                    << s.imageViewPath << "\n";
          s.persistentError = "FILE PROTECTED";
          s.errorExpiry = now + std::chrono::seconds(3);
          s.screenDirty = true;
          return;
        }
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
          exitImageView(s);
          s.screenDirty = true;
          return;
        }
        std::error_code ec;
        if (std::filesystem::remove(s.imageViewPath, ec)) {
          std::cout << "Preview: deleted " << s.imageViewPath << "\n";
          s.playbackFiles = listCaptures(pcfg.captureDir);
          if (s.playbackIdx >= static_cast<int>(s.playbackFiles.size()))
            s.playbackIdx = static_cast<int>(s.playbackFiles.size()) - 1;
          s.playbackIdx = std::max(s.playbackIdx, 0);
          exitImageView(s);
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
      exitImageView(s);
    }
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Key2) {
    // Cycle zoom: 1x -> 2x -> 4x -> 1x
    s.imageViewZoom = (s.imageViewZoom == 1)   ? 2
                      : (s.imageViewZoom == 2) ? 4
                                               : 1;
    s.imageViewPanX = 0;
    s.imageViewPanY = 0;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Shutter) {
    s.slideshowActive = !s.slideshowActive;
    if (s.slideshowActive) {
      s.slideshowNextAdvance =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
    }
    s.screenDirty = true;
  } else if (evt.pressed &&
             (evt.id == ButtonId::JoyUp || evt.id == ButtonId::JoyDown ||
              evt.id == ButtonId::JoyLeft || evt.id == ButtonId::JoyRight)) {
    if (s.imageViewZoom > 1) {
      int panStep = 8; // pixels per joystick press
      if (evt.id == ButtonId::JoyUp)
        s.imageViewPanY -= panStep;
      if (evt.id == ButtonId::JoyDown)
        s.imageViewPanY += panStep;
      if (evt.id == ButtonId::JoyLeft)
        s.imageViewPanX -= panStep;
      if (evt.id == ButtonId::JoyRight)
        s.imageViewPanX += panStep;
      s.screenDirty = true;
    } else if (s.slideshowActive) {
      // In slideshow mode, joystick navigates manually (stops slideshow)
      s.slideshowActive = false;
      if (evt.id == ButtonId::JoyUp || evt.id == ButtonId::JoyLeft) {
        if (s.playbackIdx > 0)
          --s.playbackIdx;
      } else {
        if (s.playbackIdx < static_cast<int>(s.playbackFiles.size()) - 1)
          ++s.playbackIdx;
      }
      if (!s.playbackFiles.empty()) {
        const std::string &sel = s.playbackFiles[s.playbackIdx];
        s.imageViewPixels = decodeImageToRgb565(sel, s.dispW, s.dispH);
        s.imageViewPath = sel;
        s.fileRating = readFileRating(pcfg.captureDir, sel);
      }
      s.screenDirty = true;
    } else {
      // Not zoomed, not slideshow: joystick adjusts rating (0-5 stars).
      // Up/Right increases, Down/Left decreases.
      if (evt.id == ButtonId::JoyUp || evt.id == ButtonId::JoyRight) {
        if (s.fileRating < 5)
          ++s.fileRating;
      } else {
        if (s.fileRating > 0)
          --s.fileRating;
      }
      if (!s.imageViewPath.empty()) {
        writeFileRating(pcfg.captureDir, s.imageViewPath, s.fileRating);
      }
      s.screenDirty = true;
    }
  } else if (evt.pressed) {
    // Any other button cancels a pending delete confirmation and
    // returns to playback browser.
    exitImageView(s);
    s.screenDirty = true;
  }
}

// Settings button dispatch (navigate + adjust). Exit reconfigures the still
// stream if the capture format/sensor mode changed. Basic mode: flat list;
// Advanced mode: Key1 cycles tabs. JoyUp/Down navigate, JoyLeft/Right adjust.
void handleSettingsButton(PreviewState &s, DualStream &cam,
                          const PreviewConfig &pcfg, StopFlag &stop,
                          const ButtonEvent &evt) {
  // Copyright text entry mode: intercept all buttons while editing.
  if (s.copyrightEditing) {
    if (!evt.pressed)
      return;
    if (evt.id == ButtonId::Key2 || evt.id == ButtonId::Shutter) {
      // Confirm: save the buffer (trim trailing spaces) and exit edit mode.
      std::string trimmed = s.copyrightBuffer;
      while (!trimmed.empty() && trimmed.back() == ' ')
        trimmed.pop_back();
      s.overlay.settings.copyright = trimmed;
      s.copyrightEditing = false;
      saveSettings(s.overlay.settings, defaultSettingsPath());
      std::cout << "Preview: copyright set to '" << trimmed << "'\n";
    } else if (evt.id == ButtonId::Key1) {
      // Cancel: discard changes and exit edit mode without saving.
      s.copyrightEditing = false;
      std::cout << "Preview: copyright edit cancelled\n";
    } else if (evt.id == ButtonId::JoyUp) {
      copyrightCycleChar(s.copyrightBuffer, s.copyrightCursor, 1);
    } else if (evt.id == ButtonId::JoyDown) {
      copyrightCycleChar(s.copyrightBuffer, s.copyrightCursor, -1);
    } else if (evt.id == ButtonId::JoyLeft) {
      if (s.copyrightCursor > 0)
        --s.copyrightCursor;
    } else if (evt.id == ButtonId::JoyRight) {
      if (s.copyrightCursor < 19)
        ++s.copyrightCursor;
    }
    s.screenDirty = true;
    return;
  }
  if (evt.pressed && evt.id == ButtonId::Key2) {
    // Exit settings — reconfigure camera if capture format or sensor mode
    // changed.
    uint32_t capW = pcfg.captureWidth;
    uint32_t capH = pcfg.captureHeight;
    auto smDims = sensorModeDims(s.overlay.settings.sensorMode);
    if (smDims.width > 0 && smDims.height > 0) {
      capW = smDims.width;
      capH = smDims.height;
    }
    if (s.overlay.settings.captureFormat != s.capFmt ||
        s.overlay.settings.sensorMode != s.prevSensorMode) {
      std::cout << "Preview: reconfiguring for format "
                << extensionFor(s.overlay.settings.captureFormat)
                << " sensor "
                << sensorModeLabel(s.overlay.settings.sensorMode) << "\n";
      // Join any in-flight capture worker before reconfiguring: stop() wakes
      // the worker via stillCv_, and it must finish before start() re-installs
      // the requestCompleted callback on the same `this`.
      if (s.captureThread.joinable()) {
        cam.stop();
        s.captureThread.join();
      }
      // Clear capture state so the completion handler doesn't fire with
      // stale results after the reconfigure.
      s.captureActive = false;
      s.captureDone.store(false, std::memory_order_release);
      if (!cam.reconfigureStill(pcfg.previewWidth, pcfg.previewHeight,
                                capW, capH,
                                s.overlay.settings.captureFormat)) {
        std::cerr << "Preview: reconfigure failed — exiting\n";
        stop.requestStop();
        return;
      }
      s.capFmt = s.overlay.settings.captureFormat;
      s.prevSensorMode = s.overlay.settings.sensorMode;
    }
    // Save settings on exit
    saveSettings(s.overlay.settings, defaultSettingsPath());
    // If a custom mode slot is selected (C1/C2/C3), also save to that slot.
    if (s.overlay.settings.customMode != CustomMode::Auto)
      saveCustomMode(s.overlay.settings,
                     static_cast<int>(s.overlay.settings.customMode));
    // Apply updated controls to the still stream
    CameraConfig stillCfg = settingsToCameraConfig(
        s.overlay.settings, capW, capH);
    cam.updateStillConfig(stillCfg);
    s.mode = CameraMode::Viewfinder;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Key3) {
    // Key3: toggle Basic ↔ Advanced mode.
    s.overlay.settings.menuMode =
        (s.overlay.settings.menuMode == MenuMode::Basic) ? MenuMode::Advanced
                                                         : MenuMode::Basic;
    s.settingsIdx = 0;
    if (s.overlay.settings.menuMode == MenuMode::Advanced)
      s.settingsTab = SettingsTab::Capture;
    s.screenDirty = true;
  } else if (evt.pressed && evt.id == ButtonId::Key1) {
    // Key1: cycle tabs in Advanced mode (no-op in Basic mode).
    if (s.overlay.settings.menuMode == MenuMode::Advanced) {
      s.settingsTab = static_cast<SettingsTab>(
          (static_cast<int>(s.settingsTab) + 1) % kSettingsTabCount);
      s.settingsIdx = 0;
      s.screenDirty = true;
    }
  } else if (evt.pressed && evt.id == ButtonId::JoyUp) {
    if (s.settingsIdx > 0) {
      --s.settingsIdx;
      s.screenDirty = true;
    }
  } else if (evt.pressed && evt.id == ButtonId::JoyDown) {
    int count = (s.overlay.settings.menuMode == MenuMode::Basic)
                    ? basicMenuItemCount()
                    : settingsTabItemCount(s.settingsTab);
    if (s.settingsIdx < count - 1) {
      ++s.settingsIdx;
      s.screenDirty = true;
    }
  } else if (evt.pressed &&
             (evt.id == ButtonId::JoyLeft || evt.id == ButtonId::JoyRight)) {
    // --- Basic mode item handling ---
    if (s.overlay.settings.menuMode == MenuMode::Basic) {
      // ADVANCED toggle (last item): JoyRight switches to Advanced.
      if (basicMenuItemIsAdvancedToggle(s.settingsIdx)) {
        if (evt.id == ButtonId::JoyRight) {
          s.overlay.settings.menuMode = MenuMode::Advanced;
          s.settingsTab = SettingsTab::Capture;
          s.settingsIdx = 0;
          s.screenDirty = true;
        }
        return;
      }
      // FORMAT CARD: double-press JoyRight to confirm.
      if (basicMenuItemIsFormatCard(s.settingsIdx)) {
        if (evt.id == ButtonId::JoyRight) {
          auto now = std::chrono::steady_clock::now();
          if (s.formatConfirmDeadline !=
                  std::chrono::steady_clock::time_point{} &&
              now < s.formatConfirmDeadline) {
            s.formatConfirmDeadline = {};
            int deleted = 0;
            try {
              namespace fs = std::filesystem;
              for (const auto &f : listCaptures(pcfg.captureDir)) {
                if (!isFileProtected(pcfg.captureDir, f)) {
                  std::error_code ec;
                  fs::remove(f, ec);
                  if (!ec)
                    ++deleted;
                }
              }
            } catch (const std::exception &e) {
              std::cerr << "Preview: format error: " << e.what() << "\n";
            }
            s.persistentError = "FORMATTED";
            s.errorExpiry = now + std::chrono::seconds(3);
            std::cout << "Preview: formatted card, deleted " << deleted
                      << " files\n";
          } else {
            s.formatConfirmDeadline = now + std::chrono::seconds(3);
            s.persistentError = "FORMAT? JOY-R";
            s.errorExpiry = s.formatConfirmDeadline;
          }
          s.screenDirty = true;
        }
        return;
      }
      // Normal adjustable item.
      if (evt.id == ButtonId::JoyLeft)
        basicMenuItemAdjustLeft(s.settingsIdx, s.overlay.settings);
      else
        basicMenuItemAdjustRight(s.settingsIdx, s.overlay.settings);
      s.screenDirty = true;
      return;
    }

    // --- Advanced mode item handling ---
    // EXIT item (System tab, last item) is not adjustable.
    if (advancedMenuItemIsExit(s.settingsTab, s.settingsIdx))
      return;
    // BASIC toggle (System tab idx 10): JoyRight switches to Basic.
    if (advancedMenuItemIsBasicToggle(s.settingsTab, s.settingsIdx)) {
      if (evt.id == ButtonId::JoyRight) {
        s.overlay.settings.menuMode = MenuMode::Basic;
        s.settingsIdx = 0;
        s.screenDirty = true;
      }
      return;
    }
    // FORMAT (System tab idx 2): double-press JoyRight to confirm.
    if (advancedMenuItemIsFormatCard(s.settingsTab, s.settingsIdx)) {
      if (evt.id == ButtonId::JoyRight) {
        auto now = std::chrono::steady_clock::now();
        if (s.formatConfirmDeadline != std::chrono::steady_clock::time_point{} &&
            now < s.formatConfirmDeadline) {
          s.formatConfirmDeadline = {};
          // Format: delete all non-protected captures.
          int deleted = 0;
          try {
            namespace fs = std::filesystem;
            for (const auto &f : listCaptures(pcfg.captureDir)) {
              if (!isFileProtected(pcfg.captureDir, f)) {
                std::error_code ec;
                fs::remove(f, ec);
                if (!ec)
                  ++deleted;
              }
            }
          } catch (const std::exception &e) {
            std::cerr << "Preview: format error: " << e.what() << "\n";
          }
          s.persistentError = "FORMATTED";
          s.errorExpiry = now + std::chrono::seconds(3);
          std::cout << "Preview: formatted card, deleted " << deleted
                    << " files\n";
        } else {
          s.formatConfirmDeadline = now + std::chrono::seconds(3);
          s.persistentError = "FORMAT? JOY-R";
          s.errorExpiry = s.formatConfirmDeadline;
        }
        s.screenDirty = true;
      }
      return;
    }
    // RESET (System tab idx 3): double-press JoyRight to confirm.
    if (advancedMenuItemIsReset(s.settingsTab, s.settingsIdx)) {
      if (evt.id == ButtonId::JoyRight) {
        auto now = std::chrono::steady_clock::now();
        if (s.resetConfirmDeadline !=
                std::chrono::steady_clock::time_point{} &&
            now < s.resetConfirmDeadline) {
          s.resetConfirmDeadline = {};
          // Reset: restore defaults and delete settings file.
          s.overlay.settings = CameraSettings();
          s.overlay.settings.captureFormat = s.capFmt;
          std::error_code ec;
          std::filesystem::remove(defaultSettingsPath(), ec);
          s.persistentError = "RESET DONE";
          s.errorExpiry = now + std::chrono::seconds(3);
          std::cout << "Preview: settings reset to defaults\n";
        } else {
          s.resetConfirmDeadline = now + std::chrono::seconds(3);
          s.persistentError = "RESET? JOY-R";
          s.errorExpiry = s.resetConfirmDeadline;
        }
        s.screenDirty = true;
      }
      return;
    }
    // DATE (System tab idx 5): show current date/time (read-only on Pi Zero
    // without RTC — setting system time requires root privileges).
    if (advancedMenuItemIsDate(s.settingsTab, s.settingsIdx)) {
      auto now = std::chrono::system_clock::now();
      std::time_t t = std::chrono::system_clock::to_time_t(now);
      std::tm tm;
      tm = *std::localtime(&t);
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                    tm.tm_min);
      s.persistentError = buf;
      s.errorExpiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(5);
      s.screenDirty = true;
      return;
    }
    // VIDEO (Video tab idx 0): toggle Video drive mode.
    if (advancedMenuItemIsVideo(s.settingsTab, s.settingsIdx)) {
      if (s.overlay.settings.driveMode == DriveMode::Video) {
        s.overlay.settings.driveMode = DriveMode::Single;
        s.persistentError = "VIDEO OFF";
      } else {
        s.overlay.settings.driveMode = DriveMode::Video;
        s.persistentError = "VIDEO ON";
      }
      s.errorExpiry = std::chrono::steady_clock::now() +
                      std::chrono::seconds(2);
      s.screenDirty = true;
      return;
    }
    // COPYRIGHT (System tab idx 8): enter text entry mode on JoyRight.
    if (advancedMenuItemIsCopyright(s.settingsTab, s.settingsIdx)) {
      if (evt.id == ButtonId::JoyRight) {
        s.copyrightEditing = true;
        s.copyrightBuffer = s.overlay.settings.copyright;
        // Pad to 20 chars with spaces for editing.
        while (s.copyrightBuffer.size() < 20)
          s.copyrightBuffer += ' ';
        if (s.copyrightBuffer.size() > 20)
          s.copyrightBuffer.resize(20);
        s.copyrightCursor = 0;
        std::cout << "Preview: entering copyright edit mode\n";
      }
      s.screenDirty = true;
      return;
    }
    // Normal adjustable item.
    if (evt.id == ButtonId::JoyLeft)
      settingsItemAdjustLeft(s.settingsTab, s.settingsIdx,
                             s.overlay.settings);
    else
      settingsItemAdjustRight(s.settingsTab, s.settingsIdx,
                              s.overlay.settings);
    // Handle airplane mode toggle: start/stop servers at runtime.
    if (advancedMenuItemIsAirplane(s.settingsTab, s.settingsIdx)) {
      if (s.overlay.settings.airplaneMode) {
        s.wifiServer.stop();
        s.btServer.stop();
        std::cout << "Preview: airplane mode ON — servers stopped\n";
      } else {
        constexpr int kWifiPort = 8080;
        s.wifiServer.start(kWifiPort, pcfg.captureDir, s.overlay.settings,
                           s.wifiSettingsMtx, s.wifiCaptureRequest,
                           s.wifiBatteryPercent, s.wifiCaptureCount);
        constexpr int kBtChannel = 1;
        s.btServer.start(kBtChannel, pcfg.captureDir, s.overlay.settings,
                         s.wifiSettingsMtx, s.btCaptureRequest,
                         s.wifiBatteryPercent, s.wifiCaptureCount);
        std::cout << "Preview: airplane mode OFF — servers started\n";
      }
    }
    // Handle custom mode selection: load saved settings for C1/C2/C3.
    if (advancedMenuItemIsCustomMode(s.settingsTab, s.settingsIdx) &&
        s.overlay.settings.customMode != CustomMode::Auto) {
      CameraSettings loaded;
      if (loadCustomMode(loaded,
                         static_cast<int>(s.overlay.settings.customMode))) {
        // Preserve the customMode selection itself and capture format.
        OutputFormat savedFmt = s.overlay.settings.captureFormat;
        loaded.customMode = s.overlay.settings.customMode;
        loaded.captureFormat = savedFmt;
        s.overlay.settings = loaded;
        std::cout << "Preview: loaded custom mode C"
                  << static_cast<int>(s.overlay.settings.customMode) << "\n";
      } else {
        std::cout << "Preview: no saved settings for C"
                  << static_cast<int>(s.overlay.settings.customMode)
                  << " — using current\n";
      }
    }
    s.screenDirty = true;
  }
}

// Main preview loop — extracted from runPreview() for readability.
// Handles per-frame rendering, button events, and power management.
bool runPreviewLoop(PreviewState &s, DualStream &cam,
                           St7735Display &display, ButtonInput &buttons,
                           BatteryMonitor &battery, StopFlag &stop,
                           const PreviewConfig &pcfg) {
  bool ok = true;
  try {
    while (!stop.stopRequested() && !cam.fatalError()) {
      auto frameStart = std::chrono::steady_clock::now();

      updateBatteryReading(s, battery);

      // While sleeping: poll buttons with a blocking timeout to wake on press.
      if (handleSleepPoll(s, buttons, display))
        continue;

      switch (s.mode) {
      case CameraMode::Viewfinder:
        if (renderViewfinder(s, cam, display, pcfg, stop))
          continue;
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

      switch (s.mode) {
      case CameraMode::Viewfinder:
        handleViewfinderButton(s, cam, pcfg, display, evt);
        break;
      case CameraMode::Review:
        handleReviewButton(s, evt);
        break;
      case CameraMode::Playback:
        handlePlaybackButton(s, pcfg, display, evt);
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
    std::cerr << "Preview: unhandled exception in main loop: " << e.what()
              << "\n";
    ok = false;
  }
  // If the camera hit a fatal error, signal failure so the caller
  // (and systemd Restart=on-failure) can react.
  if (cam.fatalError())
    ok = false;
  return ok;
}

} // namespace

bool runPreview(PreviewConfig &pcfg) {
  // Canonicalize + verify/create the capture directory (symlink-safe).
  if (!prepareCaptureDir(pcfg))
    return false;

  St7735Display display;
  if (!display.init(pcfg.displayCfg))
    return false;

  ButtonInput buttons;
  if (!buttons.init()) {
    display.shutdown();
    return false;
  }

  // --- Splash screen ---
  PreviewState s;
  s.dispPixels = static_cast<size_t>(display.width()) * display.height();
  s.dispW = display.width();
  s.dispH = display.height();
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
  if (!cam.start(pcfg.previewWidth, pcfg.previewHeight, pcfg.captureWidth,
                 pcfg.captureHeight, s.capFmt)) {
    cam.shutdown();
    buttons.shutdown();
    display.shutdown();
    return false;
  }
  // Apply CLI exposure/gain/AWB to the still config (shared with VF via
  // applyControls()).
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
    if (s.batteryOk)
      battery.shutdown();
    buttons.shutdown();
    display.shutdown();
    return false;
  }

  s.frameDelay = std::chrono::microseconds(
      pcfg.maxFps > 0 ? 1000000 / pcfg.maxFps : 50000);
  s.overlay.settings.captureFormat = s.capFmt;

  // Load saved settings from ~/.config/picamera/settings.conf
  // (falls back to defaults if the file doesn't exist or is corrupt)
  {
    std::string settingsPath = defaultSettingsPath();
    if (loadSettings(s.overlay.settings, settingsPath)) {
      std::cout << "Preview: loaded settings from " << settingsPath << "\n";
      // Override capture format with CLI value if it was explicitly set
      s.overlay.settings.captureFormat = s.capFmt;
      CameraConfig stillCfg = settingsToCameraConfig(
          s.overlay.settings, pcfg.captureWidth, pcfg.captureHeight);
      cam.updateStillConfig(stillCfg);
    }
  }

  // Load hardware config from /etc/picamera.conf (if present)
  {
    HardwareConfig hwCfg = loadHardwareConfig("/etc/picamera.conf");
    if (hwCfg.loaded) {
      std::cout << "Preview: loaded hardware config from /etc/picamera.conf\n";
      if (hwCfg.previewWidth > 0)
        pcfg.previewWidth = hwCfg.previewWidth;
      if (hwCfg.previewHeight > 0)
        pcfg.previewHeight = hwCfg.previewHeight;
      if (hwCfg.captureWidth > 0)
        pcfg.captureWidth = hwCfg.captureWidth;
      if (hwCfg.captureHeight > 0)
        pcfg.captureHeight = hwCfg.captureHeight;
      if (hwCfg.maxFps > 0)
        pcfg.maxFps = hwCfg.maxFps;
      if (!hwCfg.captureDir.empty())
        pcfg.captureDir = hwCfg.captureDir;
      if (!hwCfg.capturePrefix.empty())
        pcfg.capturePrefix = hwCfg.capturePrefix;
      if (hwCfg.wifiEnabled)
        pcfg.wifiEnabled = true;
      if (hwCfg.btEnabled)
        pcfg.btEnabled = true;
    }
  }

  // Initialize time-based state to "long ago" so the first iteration triggers
  // a battery read and no entries are pre-expired.
  auto initNow = std::chrono::steady_clock::now();
  s.lastBatteryRead = initNow - std::chrono::hours(1);
  s.reviewStart = initNow - std::chrono::hours(1);
  s.timerEndTime = initNow - std::chrono::hours(1);
  s.errorExpiry = initNow - std::chrono::hours(1);
  s.lastActivity = initNow;

  std::cout << "Preview: streaming " << cam.vfWidth() << "x" << cam.vfHeight()
            << " -> display " << display.width() << "x" << display.height()
            << " (max " << pcfg.maxFps << " fps)\n";
  std::cout << "Preview: shutter=capture, Key1=playback, Key2=settings, "
               "Ctrl+C=exit\n";

  // Start Wi-Fi server if enabled and airplane mode is not active.
  if (pcfg.wifiEnabled && !s.overlay.settings.airplaneMode) {
    constexpr int kWifiPort = 8080;
    if (!s.wifiServer.start(kWifiPort, pcfg.captureDir, s.overlay.settings,
                            s.wifiSettingsMtx, s.wifiCaptureRequest,
                            s.wifiBatteryPercent, s.wifiCaptureCount)) {
      std::cerr << "Preview: Wi-Fi server failed to start on port " << kWifiPort
                << " — continuing without remote control\n";
    }
  }

  // Start Bluetooth server if enabled (shares settings mutex + CameraSettings
  // with the Wi-Fi server).
  if (pcfg.btEnabled && !s.overlay.settings.airplaneMode) {
    constexpr int kBtChannel = 1;
    if (!s.btServer.start(kBtChannel, pcfg.captureDir, s.overlay.settings,
                          s.wifiSettingsMtx, s.btCaptureRequest,
                          s.wifiBatteryPercent, s.wifiCaptureCount)) {
      std::cerr << "Preview: BT server failed to start on channel "
                << kBtChannel << " — continuing without BT remote control\n";
    }
  }

  bool loopOk = runPreviewLoop(s, cam, display, buttons, battery, stop, pcfg);

  s.wifiServer.stop();
  s.btServer.stop();

  // Shutdown: stop the camera first (wakes blocked waitCaptureDone() via
  // stillCv_), join the capture worker, then release the camera handle.
  cam.stop();
  if (s.captureThread.joinable())
    s.captureThread.join();

  cam.shutdown();
  buttons.shutdown();
  display.shutdown();
  if (s.batteryOk)
    battery.shutdown();

  saveSettings(s.overlay.settings, defaultSettingsPath());

  std::cout << "Preview: stopped after " << s.frameCount << " frames, "
            << s.captureCount << " captures\n";
  return loopOk;
}

PreviewConfig makePreviewConfig(const CliOptions &opts,
                                const CameraConfig &cfg) {
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
  pcfg.wifiEnabled = opts.wifiEnabled;
  pcfg.btEnabled = opts.btEnabled;
  // Use ±6.144V PGA for direct LiPo measurement (3.0-4.2V)
  pcfg.batteryCfg.pgaGain = 0x0000;
  return pcfg;
}

} // namespace picamera
