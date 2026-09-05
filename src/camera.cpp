#include "camera.h"
#include "callback_guard.h"
#include "controls.h"
#include "mapped_plane.h"
#include "output_writer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>

namespace picamera {

// Targeted imports for the libcamera names used in this TU, instead of a
// file-scope `using namespace libcamera;` (SF.7). Only the specific types
// and sub-namespaces referenced below are imported.
using libcamera::CameraConfiguration;
using libcamera::FrameBufferAllocator;
using libcamera::Request;
using libcamera::StreamRole;
namespace controls = libcamera::controls;
namespace formats = libcamera::formats;

namespace {

// --- Capture constants ---
constexpr uint32_t kDefaultBufferCount = 4;
constexpr uint32_t kHighResBufferCount = 1;
constexpr uint32_t kHighResPixelThreshold = 2000000; // >2MP → fewer buffers

} // namespace

CameraApp::CameraApp() = default;
CameraApp::~CameraApp() { shutdown(); }

bool CameraApp::init() { return handle_.init("Camera"); }

bool CameraApp::configure(const CameraConfig &cfg) {
  if (!handle_.camera()) {
    std::cerr << "Camera: not initialized — call init() first\n";
    return false;
  }
  config_ = cfg;
  // Reset software-JPEG flag so a previous fallback configure doesn't
  // leak into a new configuration that actually gets hardware MJPEG.
  swJpegEncode_ = false;

  // DNG capture needs a raw Bayer stream; everything else uses StillCapture.
  auto roles = (cfg.format == OutputFormat::DNG)
                   ? std::vector<StreamRole>{StreamRole::Raw}
                   : std::vector<StreamRole>{StreamRole::StillCapture};
  auto camCfg = handle_.camera()->generateConfiguration(roles);
  if (!camCfg) {
    std::cerr << "generateConfiguration failed\n";
    return false;
  }
  if (camCfg->empty()) {
    std::cerr << "Camera configuration produced no streams\n";
    return false;
  }

  auto &sc = camCfg->at(0);
  sc.size.width = cfg.width;
  sc.size.height = cfg.height;
  // JPEG output uses the Pi ISP's hardware MJPEG encoder — the buffer
  // contains a complete JPEG bitstream, no NV12->RGB conversion needed.
  // DNG output uses raw Bayer (SRGGB10_CSI2P on the Pi ISP).
  // Everything else uses NV12 and converts in software.
  if (cfg.format == OutputFormat::JPEG) {
    sc.pixelFormat = formats::MJPEG;
  } else if (cfg.format == OutputFormat::DNG) {
    sc.pixelFormat = formats::SRGGB10_CSI2P;
  } else {
    sc.pixelFormat = formats::NV12;
  }
  sc.bufferCount = kDefaultBufferCount;

  // For high-res captures (e.g. 4056x3040), the Pi may not have enough
  // memory for 4 buffers. Use 1 buffer for any high-resolution still
  // format (NV12, DNG raw, or MJPEG fallback). DNG raw at full res is
  // ~23 MB per buffer, so 4 buffers = ~90 MB on a 512 MB Pi Zero 2 W.
  // Use uint64_t to avoid overflow on large dimensions.
  if (static_cast<uint64_t>(cfg.width) * cfg.height > kHighResPixelThreshold) {
    sc.bufferCount = kHighResBufferCount;
  }

  auto status = camCfg->validate();
  if (status == CameraConfiguration::Invalid) {
    std::cerr << "Camera configuration invalid\n";
    return false;
  }

  // Check if MJPEG was rejected by the pipeline handler (Pi VC4 may fall
  // back to YUYV at high resolutions). If so, switch to NV12 and encode
  // JPEG in software via libjpeg.
  if (cfg.format == OutputFormat::JPEG && sc.pixelFormat != formats::MJPEG) {
#ifndef HAVE_JPEG
    std::cerr << "Camera: HW MJPEG unavailable (got " << sc.pixelFormat
              << ") and libjpeg was not compiled in — cannot encode JPEG\n";
    return false;
#else
    std::cerr << "Warning: HW MJPEG unavailable (got " << sc.pixelFormat
              << "), falling back to software JPEG encode\n";
    sc.pixelFormat = formats::NV12;
    // High-res NV12 needs fewer buffers on the Pi
    if (static_cast<uint64_t>(cfg.width) * cfg.height > kHighResPixelThreshold)
      sc.bufferCount = kHighResBufferCount;
    auto status2 = camCfg->validate();
    if (status2 == CameraConfiguration::Invalid) {
      std::cerr << "Camera configuration invalid (NV12 fallback)\n";
      return false;
    }
    swJpegEncode_ = true;
#endif
  }

  if (handle_.camera()->configure(camCfg.get())) {
    std::cerr << "cam->configure() failed\n";
    return false;
  }

  stream_ = sc.stream();

  // Double-check: configure() may silently change the pixel format even
  // if validate() accepted MJPEG. Check the actual stream configuration.
  if (cfg.format == OutputFormat::JPEG && !swJpegEncode_) {
    const auto &actualFmt = stream_->configuration().pixelFormat;
    if (actualFmt != formats::MJPEG) {
#ifndef HAVE_JPEG
      std::cerr << "Camera: HW MJPEG unavailable after configure (got "
                << actualFmt << ") and libjpeg was not compiled in"
                << " — cannot encode JPEG\n";
      return false;
#else
      std::cerr << "Warning: HW MJPEG unavailable after configure (got "
                << actualFmt << "), reconfiguring with NV12\n";
      // Reconfigure with NV12
      sc.pixelFormat = formats::NV12;
      if (static_cast<uint64_t>(cfg.width) * cfg.height >
          kHighResPixelThreshold)
        sc.bufferCount = kHighResBufferCount;
      auto status = camCfg->validate();
      if (status == CameraConfiguration::Invalid) {
        std::cerr << "NV12 fallback configuration invalid\n";
        return false;
      }
      if (handle_.camera()->configure(camCfg.get())) {
        std::cerr << "cam->configure() failed (NV12 fallback)\n";
        return false;
      }
      // Re-fetch stream_ — the second configure() may invalidate the
      // StreamConfiguration pointer captured before the fallback.
      stream_ = sc.stream();
      // Verify the fallback actually resulted in NV12 — the pipeline
      // handler could theoretically substitute yet another format.
      if (stream_->configuration().pixelFormat != formats::NV12) {
        std::cerr << "NV12 fallback rejected by pipeline handler (got "
                  << stream_->configuration().pixelFormat << ")\n";
        stream_ = nullptr;
        return false;
      }
      swJpegEncode_ = true;
#endif
    }
  }

  // Clear any pending capture requests before replacing the allocator.
  // Requests hold raw FrameBuffer* pointers into the allocator's pool;
  // destroying the allocator first would leave dangling pointers.
  capRequests_.clear();
  allocator_ = std::make_unique<FrameBufferAllocator>(handle_.camera());
  if (allocator_->allocate(stream_) < 0) {
    std::cerr << "Buffer allocation failed\n";
    allocator_.reset();
    stream_ = nullptr;
    return false;
  }

  std::cout << "Configured: " << sc.size.width << "x" << sc.size.height
            << " stride:" << sc.stride << "\n";
  return true;
}

bool CameraApp::capture(const std::string &filename, std::string *actualPath) {
  if (!handle_.camera() || !allocator_) {
    std::cerr << "Camera not initialized\n";
    return false;
  }

  const auto &buffers = allocator_->buffers(stream_);
  if (buffers.empty())
    return false;

  // RAII guard: ensures the signal is disconnected on all exit paths,
  // including exceptions, so the callback can't fire after capture()
  // returns or after the CameraApp is destroyed.
  class SignalDisconnectGuard {
  public:
    explicit SignalDisconnectGuard(libcamera::Camera *c) : cam_(c) {}
    ~SignalDisconnectGuard() noexcept {
      if (cam_) {
        try {
          cam_->requestCompleted.disconnect();
        } catch (const std::exception &e) {
          std::cerr << "Camera: disconnect threw in guard: " << e.what()
                    << "\n";
        }
      }
    }
    void release() { cam_ = nullptr; }

  private:
    libcamera::Camera *cam_;
  } signalGuard{handle_.camera().get()};

  // Connect the completion callback BEFORE starting the camera — the
  // safer libcamera pattern ensures no completion can fire before the
  // signal is connected (matches DualStream::start).
  const uint32_t warmup = config_.warmupFrames;
  capCompleted_.store(0, std::memory_order_relaxed);
  capDone_ = false;
  capSaved_ = false;

  handle_.camera()->requestCompleted.connect(
      this, [this, filename, warmup](Request *r) {
        onCaptureCompleted(r, filename, warmup);
      });

  if (handle_.camera()->start()) {
    std::cerr << "Failed to start camera\n";
    // Symmetric cleanup matching the other failure paths: release the
    // guard (so it won't double-disconnect), disconnect the signal, and
    // stop the camera (no-op here since start() failed, but keeps the
    // path consistent with the timeout/createRequest/queueRequest paths).
    signalGuard.release();
    try {
      handle_.camera()->requestCompleted.disconnect();
    } catch (const std::exception &e) {
      std::cerr << "Camera: disconnect threw: " << e.what() << "\n";
    }
    stopCamera();
    return false;
  }
  started_ = true;

  // Initial queue: one request per buffer, with controls applied to each.
  // Stored as members (capRequests_) so they outlive the capture() call
  // frame — libcamera may still invoke callbacks during stop().
  capRequests_.clear();
  capRequests_.reserve(buffers.size());
  for (size_t i = 0; i < buffers.size(); ++i) {
    auto req = handle_.camera()->createRequest();
    if (!req) {
      std::cerr << "Failed to create Request\n";
      signalGuard.release();
      handle_.camera()->requestCompleted.disconnect();
      stopCamera();
      return false;
    }
    if (req->addBuffer(stream_, buffers[i].get())) {
      std::cerr << "Failed to add buffer to Request\n";
      signalGuard.release();
      handle_.camera()->requestCompleted.disconnect();
      stopCamera();
      return false;
    }
    try {
      applyControls(req.get(), config_);
    } catch (const std::exception &e) {
      std::cerr << "Camera: applyControls threw: " << e.what() << "\n";
      signalGuard.release();
      handle_.camera()->requestCompleted.disconnect();
      stopCamera();
      return false;
    }
    try {
      if (handle_.camera()->queueRequest(req.get())) {
        std::cerr << "Failed to queue Request\n";
        signalGuard.release();
        handle_.camera()->requestCompleted.disconnect();
        stopCamera();
        return false;
      }
    } catch (const std::exception &e) {
      std::cerr << "Camera: queueRequest threw: " << e.what() << "\n";
      signalGuard.release();
      handle_.camera()->requestCompleted.disconnect();
      stopCamera();
      return false;
    }
    capRequests_.push_back(std::move(req));
  }

  // Compute timeout from exposure time plus a margin for warmup/processing.
  // A fixed 60s timeout breaks long exposures (e.g. --shutter 120000000).
  // Add 3s per warmup frame (exposure + readout + overhead) and a 90s base
  // margin for multi-MB DNG save on slow SD cards. Clamp the exposure
  // contribution to 1h — more than any real exposure, and avoids overflow
  // for huge programmatic exposureTime values.
  long long exposureSecs =
      config_.exposureTime > 0
          ? std::min<long long>(
                static_cast<long long>(config_.exposureTime / 1000000LL),
                3600LL)
          : 0;
  auto timeout = std::chrono::seconds(exposureSecs + 90 +
                                      static_cast<long long>(warmup) * 3);
  auto deadline = std::chrono::steady_clock::now() + timeout;
  bool saved = false;
  {
    std::unique_lock<std::mutex> lk(capMtx_);
    bool timedOut = false;
    try {
      timedOut = !capCv_.wait_until(lk, deadline, [this] { return capDone_; });
    } catch (const std::exception &e) {
      std::cerr << "Camera: capture wait threw: " << e.what() << "\n";
      timedOut = true;
    }
    if (timedOut) {
      std::cerr << "Capture timed out (completed "
                << capCompleted_.load(std::memory_order_relaxed) << "/"
                << (warmup + 1) << " frames)\n";
      // Unlock BEFORE disconnect/stop to avoid deadlock: the callback
      // needs capMtx_ to set capDone_, and stop() waits for callbacks.
      // Guard: wait_until may throw before re-locking, leaving lk
      // without ownership — calling unlock() then is UB.
      if (lk.owns_lock())
        lk.unlock();
      signalGuard.release(); // guard will not double-disconnect
      try {
        handle_.camera()->requestCompleted.disconnect();
      } catch (const std::exception &e) {
        std::cerr << "Camera: disconnect threw: " << e.what() << "\n";
      }
      stopCamera();
      return false;
    }
    saved = capSaved_; // read under mutex
    if (saved && actualPath)
      *actualPath = capActualPath_; // also under mutex
  }

  signalGuard.release(); // guard will not double-disconnect
  try {
    handle_.camera()->requestCompleted.disconnect();
  } catch (const std::exception &e) {
    std::cerr << "Camera: disconnect threw: " << e.what() << "\n";
  }
  stopCamera();

  if (fatalError_.load(std::memory_order_acquire))
    return false;
  if (!saved)
    std::cerr << "Failed to save frame\n";
  return saved;
}

bool CameraApp::captureBracket(const std::string &baseFilename) {
  // HDR bracketing: capture one frame per EV offset in cfg.bracketEv.
  // For each frame, we adjust the exposure time by 2^ev relative to the
  // base exposure. This requires manual exposure (--shutter set);
  // under AE the EV offset has no effect and all frames would be
  // identical, so reject the combination rather than silently producing
  // wrong output.
  //
  // Filenames: baseFilename with _evN suffix inserted before the extension.
  // e.g. "photo.png" -> "photo_ev-2.png", "photo_ev0.png", "photo_ev+2.png"
  if (config_.bracketEv.empty()) {
    return capture(baseFilename);
  }

  if (config_.exposureTime == 0) {
    std::cerr << "Bracketing requires manual exposure (--shutter <us>). "
              << "Exposure time is auto (0); EV offsets would have "
              << "no effect.\n";
    return false;
  }

  // Find extension to insert EV suffix.
  size_t dotPos = baseFilename.rfind('.');
  std::string base;
  std::string ext;
  if (dotPos != std::string::npos) {
    base = baseFilename.substr(0, dotPos);
    ext = baseFilename.substr(dotPos);
  } else {
    base = baseFilename;
    ext = "";
  }

  // Snapshot the original config before any bracket iteration — configure()
  // overwrites config_, so without this each bracket frame would compound
  // the exposure scaling on top of the previous frame's scaled value.
  const CameraConfig baseConfig = config_;
  const bool baseSwJpegEncode = swJpegEncode_;

  bool allOk = true;
  for (size_t i = 0; i < baseConfig.bracketEv.size(); ++i) {
    float ev = baseConfig.bracketEv[i];

    // Create a modified config for this bracket frame from the base.
    CameraConfig bracketCfg = baseConfig;
    // Scale manual exposure time by 2^ev (AE is rejected above).
    {
      double factor = std::pow(2.0, static_cast<double>(ev));
      // Guard against inf/NaN from extreme EV values —
      // static_cast<uint64_t>(inf) is undefined behavior.
      if (!std::isfinite(factor) || factor <= 0.0) {
        std::cerr << "Bracket EV" << ev << " produces invalid factor\n";
        allOk = false;
        continue;
      }
      double scaled = static_cast<double>(bracketCfg.exposureTime) * factor;
      // libcamera's controls::ExposureTime is int32_t, so the
      // scaled value must fit in INT32_MAX to avoid overflow when
      // applied via ctrls.set(controls::ExposureTime, ...).
      constexpr double kMaxExposure = 2147483647.0; // INT32_MAX
      if (!std::isfinite(scaled) || scaled < 1.0 || scaled > kMaxExposure) {
        std::cerr << "Bracket EV" << ev << " exposure out of range\n";
        allOk = false;
        continue;
      }
      bracketCfg.exposureTime = static_cast<uint64_t>(std::llround(scaled));
    }

    // Build filename with EV suffix.
    char evStr[16];
    std::snprintf(evStr, sizeof(evStr), "%+.1f", ev);
    std::string fname = base;
    fname += "_ev";
    fname += evStr;
    fname += ext;

    std::cout << "Bracket " << (i + 1) << "/" << baseConfig.bracketEv.size()
              << ": EV" << evStr << " -> " << fname << "\n";

    // Reconfigure with the bracket config and capture.
    if (!configure(bracketCfg)) {
      std::cerr << "Failed to configure for bracket EV" << evStr
                << " — aborting bracket sequence\n";
      allOk = false;
      break; // Camera is in an unknown state — don't continue.
    }
    if (!capture(fname)) {
      std::cerr << "Failed to capture bracket EV" << evStr << "\n";
      allOk = false;
      // Continue to next bracket frame — one bad frame shouldn't
      // prevent capturing the rest of the HDR set.
    }
  }
  // Restore the original config — configure() overwrites config_ each
  // iteration, so without this the last bracket exposure would persist.
  // Also restore swJpegEncode_ since configure() may flip it during
  // MJPEG→NV12 fallback; otherwise the next capture() would use the wrong
  // OutputWriter for the original format.
  // Reconfigure the live libcamera pipeline back to the base config so
  // subsequent capture() calls use the original exposure, not the last
  // bracket frame's. configure() also resets config_/swJpegEncode_.
  if (!configure(baseConfig)) {
    std::cerr << "Failed to restore base config after bracket sequence\n";
    allOk = false;
  }
  config_ = baseConfig;
  swJpegEncode_ = baseSwJpegEncode;
  return allOk;
}

void CameraApp::listControls() {
  if (!handle_.camera())
    return;

  const auto &controls = handle_.camera()->controls();
  std::cout << "=== Controls ===\n";
  for (const auto &[id, info] : controls) {
    std::cout << "  " << id->name() << ": " << info.toString() << "\n";
  }

  const auto &props = handle_.camera()->properties();
  std::cout << "\n=== Properties ===\n";
  const auto *propIdMap = props.idMap();
  for (const auto &[id, val] : props) {
    std::string name = std::to_string(id);
    if (propIdMap) {
      auto it = propIdMap->find(id);
      if (it != propIdMap->end() && it->second)
        name = it->second->name();
    }
    std::cout << "  prop:" << name << ": " << val.toString() << "\n";
  }
}

void CameraApp::requeueRequest(libcamera::Camera *cam, Request *r) {
  // Re-queue with controls, catching exceptions from applyControls
  // (ControlList::set throws if a control is not supported by the
  // pipeline handler, which would std::terminate inside this callback).
  // If applyControls throws, skip the requeue — streaming with partially
  // configured controls could produce wrong exposure/AWB for many frames.
  bool controlsOk = true;
  try {
    applyControls(r, config_);
  } catch (const std::exception &e) {
    std::cerr << "Camera: applyControls threw: " << e.what() << "\n";
    controlsOk = false;
  }
  if (!controlsOk) {
    // Request was already reused by the caller (onCaptureCompleted).
    // Skip the requeue — streaming with partially configured controls
    // could produce wrong exposure/AWB for many frames.
    return;
  }
  try {
    if (cam->queueRequest(r)) {
      std::cerr << "Camera: requeue failed\n";
    }
  } catch (const std::exception &e) {
    std::cerr << "Camera: queueRequest threw: " << e.what() << "\n";
  }
}

void CameraApp::onCaptureCompleted(Request *r, const std::string &filename,
                                   uint32_t warmup) {
  // Track in-flight callbacks so shutdown() can wait for all
  // callbacks to exit before destroying Request objects.
  callbacksInFlight_.fetch_add(1, std::memory_order_acq_rel);
  // Ensure decrement happens on every exit path (including early returns).
  CallbackGuard cbGuard{callbacksInFlight_, callbacksCv_, callbacksMtx_};

  // Hold a shared_ptr snapshot of the camera for the whole callback
  // body so that shutdown() can't release it during queueRequest.
  auto cam = handle_.camera();
  if (!cam)
    return;

  if (r->status() != Request::RequestComplete) {
    // Cancelled requests arrive during stopCamera() — don't requeue
    // them to an already-stopped camera (causes spurious errors).
    if (r->status() == Request::RequestCancelled) {
      r->reuse(Request::ReuseBuffers);
      return;
    }
    std::cerr << "Request status: " << r->status() << "\n";
    r->reuse(Request::ReuseBuffers);
    requeueRequest(cam.get(), r);
    return;
  }
  uint32_t completed =
      capCompleted_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (completed <= warmup) {
    // Warmup frame — discard and re-queue so AE/AWB keeps converging.
    r->reuse(Request::ReuseBuffers);
    requeueRequest(cam.get(), r);
    return;
  }
  // Only save the first converged frame; subsequent in-flight
  // requests are discarded to prevent multiple output files. Don't
  // re-queue — the capture is done (or being saved), and stopCamera()
  // will cancel any remaining in-flight requests. Requeuing would
  // just burn CPU on callbacks that hit this same branch again.
  if (completed > warmup + 1) {
    r->reuse(Request::ReuseBuffers);
    return;
  }
  // Converged frame — save it outside the mutex so the capture
  // timeout in the main thread can still fire during slow I/O.
  // Wrap in try/catch: saveFrame allocates large buffers (30+ MB
  // for full-res conversion) and std::bad_alloc from the allocator
  // would call std::terminate() inside this libcamera callback.
  bool saved = false;
  std::string actualPathLocal;
  try {
    saved = saveFrame(r, filename, &actualPathLocal);
  } catch (const std::bad_alloc &) {
    std::cerr << "Capture: out of memory saving frame\n";
  } catch (const std::exception &e) {
    std::cerr << "Capture: saveFrame threw: " << e.what() << "\n";
  }
  // Reuse the request's buffers so libcamera reclaims them promptly,
  // matching the pattern in every other path of this callback. Without
  // this, the saved request's V4L2 buffers stay held until libcamera
  // cleans them up during stop() — a latent buffer-retention issue.
  r->reuse(Request::ReuseBuffers);
  {
    std::lock_guard<std::mutex> lk(capMtx_);
    capSaved_ = saved;
    capActualPath_ = std::move(actualPathLocal);
    if (!saved)
      capActualPath_.clear();
    capDone_ = true;
    capCv_.notify_one();
  }
}

void CameraApp::stopCamera() noexcept {
  if (started_ && handle_.camera()) {
    try {
      handle_.camera()->stop();
    } catch (const std::exception &e) {
      std::cerr << "CameraApp: stop() threw: " << e.what() << "\n";
    }
    started_ = false;
  }
  // Wait for any in-flight callbacks to exit before returning —
  // stop() cancels requests and fires callbacks, but doesn't
  // guarantee they've returned. This prevents UAF if the caller
  // immediately destroys Request objects. Use a CV with a 60s
  // timeout (saveFrame can be slow for full-res JPEG/DNG writes
  // on slow SD cards). If callbacks haven't exited after 60s,
  // the system is broken — set fatalError_ so the caller exits
  // gracefully (systemd Restart=on-failure handles the restart).
  // Wrap in try/catch: mutex/CV ops can throw std::system_error,
  // which would std::terminate() inside this noexcept function.
  try {
    std::unique_lock<std::mutex> lk(callbacksMtx_);
    if (!callbacksCv_.wait_for(lk, std::chrono::seconds(60), [this] {
          return callbacksInFlight_.load(std::memory_order_acquire) == 0;
        })) {
      std::cerr << "CameraApp: FATAL: callbacks stuck after 60s — requesting "
                   "shutdown\n";
      fatalError_.store(true, std::memory_order_release);
    }
  } catch (const std::exception &e) {
    std::cerr << "CameraApp: callback wait threw: " << e.what() << "\n";
  }
}

void CameraApp::shutdown() noexcept {
  if (handle_.camera()) {
    try {
      handle_.camera()->requestCompleted.disconnect();
    } catch (const std::exception &e) {
      std::cerr << "CameraApp: disconnect threw: " << e.what() << "\n";
    }
  }
  stopCamera();
  // If stopCamera() timed out with stuck callbacks, fatalError_ is set.
  // Do NOT clear capRequests_ or free the allocator — a callback may
  // still be dereferencing them (use-after-free). Let the process exit
  // and systemd Restart=on-failure handle cleanup.
  if (fatalError_.load(std::memory_order_acquire))
    return;
  // Clear Requests BEFORE freeing the allocator — Requests reference
  // FrameBuffers owned by the allocator, so destroying them after
  // allocator_->reset() would be a use-after-free.
  capRequests_.clear();
  stream_ = nullptr;
  allocator_.reset();
  handle_.shutdown();
}

void CameraApp::applyControls(Request *req, const CameraConfig &cfg) {
  auto &ctrls = req->controls();
  // Clear any stale controls from a previous request lifecycle.
  ctrls.clear();

  // If the user asked for a manual shutter or gain, AE must be off or
  // libcamera will ignore ExposureTime/AnalogueGain. Auto-disable AE in
  // that case so --shutter / --iso do what the user expects without
  // requiring an explicit --ae-disable.
  const bool manualExposure =
      !cfg.aeEnable || cfg.exposureTime > 0 || cfg.analogueGain > 0.0f;
  if (manualExposure) {
    ctrls.set(controls::AeEnable, false);
    if (cfg.exposureTime > 0) {
      // libcamera's controls::ExposureTime is int32_t — clamp to
      // prevent overflow when the uint64_t config value is too large.
      ctrls.set(controls::ExposureTime, static_cast<int32_t>(std::min<uint64_t>(
                                            cfg.exposureTime, INT32_MAX)));
    }
    if (cfg.analogueGain > 0.0f) {
      ctrls.set(controls::AnalogueGain, cfg.analogueGain);
    }
  }
  if (cfg.digitalGain > 0.0f) {
    ctrls.set(controls::DigitalGain, cfg.digitalGain);
  }
  if (!cfg.awbEnable) {
    ctrls.set(controls::AwbEnable, false);
  } else {
    ctrls.set(controls::AwbEnable, true);
    if (auto mode = lookupAwb(cfg.awbMode)) {
      ctrls.set(controls::AwbMode, *mode);
    }
  }
  if (!manualExposure) {
    ctrls.set(controls::AeEnable, true);
  }
}

bool CameraApp::saveFrame(const Request *req, const std::string &filename,
                          std::string *actualPath) {
  const auto &buffers = req->buffers();
  if (buffers.empty())
    return false;

  const auto &[stream, buffer] = *buffers.begin();
  // Validate that this buffer belongs to our configured stream — a
  // mismatched buffer would produce garbage output.
  if (stream != stream_) {
    std::cerr << "Capture: buffer from unexpected stream\n";
    return false;
  }
  auto planes = buffer->planes();
  if (planes.empty()) {
    std::cerr << "No planes in buffer\n";
    return false;
  }

  const auto &sc = stream_->configuration();

  // Build a local config copy and override exposure/gain with the actual
  // converged values from the request metadata. When AE is enabled,
  // config_.exposureTime and config_.analogueGain are zero (auto), so
  // without this the DNG writer would omit ExposureTime and default
  // the ISO to 100 instead of recording the real values.
  CameraConfig writerCfg = config_;
  const auto &md = req->metadata();
  if (auto exp = md.get(controls::ExposureTime)) {
    writerCfg.exposureTime = static_cast<uint64_t>(*exp);
  }
  if (auto gain = md.get(controls::AnalogueGain)) {
    writerCfg.analogueGain = *gain;
  }

  auto writer = makeOutputWriter(writerCfg.format, writerCfg, swJpegEncode_);
  if (!writer) {
    std::cerr << "No output writer for format\n";
    return false;
  }

  MappedPlane p0(planes[0], "plane0");
  if (!p0.valid())
    return false;

  MappedPlane p1;
  if (planes.size() >= 2) {
    p1 = MappedPlane(planes[1], "plane1");
    if (!p1.valid())
      return false;
  }

  FrameView frame;
  frame.width = sc.size.width;
  frame.height = sc.size.height;
  frame.stride = sc.stride;
  frame.plane0 = p0.data();
  frame.plane0Size = p0.size();
  frame.plane1 = p1.data();
  frame.plane1Size = p1.valid() ? p1.size() : 0;

  return writer->write(frame, filename, actualPath);
}

} // namespace picamera
