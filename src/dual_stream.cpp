#include "dual_stream.h"
#include "camera.h"
#include "controls.h"
#include "mapped_plane.h"
#include "safe_path.h"
#include "callback_guard.h"

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <unistd.h>
#include <sys/mman.h>

#include <libcamera/formats.h>
#include <libcamera/control_ids.h>

// Lock ordering (acquire in this order to avoid deadlock):
//   1. callbacksMtx_  — held during callback dispatch / stop() wait
//   2. stillMtx_ / vfMtx_  — per-stream state (still capture / viewfinder)
//   3. cfgMtx_  — still config (written by UI thread, read by callback)
// stop() acquires callbacksMtx_ then stillMtx_; applyControls() acquires
// cfgMtx_ only. Never hold a lower-numbered lock while acquiring a
// higher-numbered one.
namespace picamera {

// Targeted imports for the libcamera names used in this TU, instead of a
// file-scope `using namespace libcamera;` (SF.7).
using libcamera::CameraConfiguration;
using libcamera::FrameBufferAllocator;
using libcamera::Request;
using libcamera::StreamRole;
namespace controls = libcamera::controls;
namespace formats = libcamera::formats;

namespace {

constexpr uint32_t kVfBufferCount = 4;
constexpr uint32_t kStillBufferCount = 1;

} // namespace

DualStream::DualStream() = default;
DualStream::~DualStream() { shutdown(); }

bool DualStream::init() {
    return handle_.init("DualStream");
}

bool DualStream::start(uint32_t vfW, uint32_t vfH,
                       uint32_t capW, uint32_t capH,
                       OutputFormat capFmt) {
    if (!handle_.camera()) return false;

    // Validate dimensions to prevent zero-size or overflow-prone configurations.
    if (vfW == 0 || vfH == 0 || capW == 0 || capH == 0) return false;
    if (capW > kMaxSensorWidth || capH > kMaxSensorHeight) return false;

    // Reset per-start state. swJpegEncode_ is set to true below if the
    // pipeline handler rejects HW MJPEG; without this reset, a previous
    // fallback would persist across reconfigureStill() calls and cause
    // SwJpegWriter to be selected for an MJPEG stream (garbage output).
    swJpegEncode_ = false;

    stillFmt_ = capFmt;
    stillCfg_.width = capW;
    stillCfg_.height = capH;
    stillCfg_.format = capFmt;
    // Preserve AE/AWB enable flags across reconfigureStill() — the CLI may
    // have disabled them (--ae-disable/--awb-disable) via updateStillConfig(),
    // and resetting them here would re-enable AE/AWB in the viewfinder until
    // the next capture. CameraConfig defaults them to true, so the initial
    // start() still gets AE/AWB on unless the CLI explicitly disabled them.

    // Configure both Viewfinder + StillCapture roles on one camera.
    // The Pi ISP drives both from the same sensor, sharing AE/AWB.
    std::vector<StreamRole> roles;
    if (capFmt == OutputFormat::DNG) {
        // DNG needs raw — use Raw + Viewfinder.
        roles = {StreamRole::Viewfinder, StreamRole::Raw};
    } else {
        roles = {StreamRole::Viewfinder, StreamRole::StillCapture};
    }

    auto cfg = handle_.camera()->generateConfiguration(roles);
    if (!cfg || cfg->size() < 2) {
        std::cerr << "DualStream: generateConfiguration failed\n";
        return false;
    }

    // Viewfinder stream (slot 0): low-res NV12, continuous.
    auto &vfSc = cfg->at(0);
    vfSc.size.width = vfW;
    vfSc.size.height = vfH;
    vfSc.pixelFormat = formats::NV12;
    vfSc.bufferCount = kVfBufferCount;

    // Still stream (slot 1): full-res.
    auto &stillSc = cfg->at(1);
    stillSc.size.width = capW;
    stillSc.size.height = capH;
    if (capFmt == OutputFormat::JPEG) {
        stillSc.pixelFormat = formats::MJPEG;
    } else if (capFmt == OutputFormat::DNG) {
        stillSc.pixelFormat = formats::SRGGB10_CSI2P;
    } else {
        stillSc.pixelFormat = formats::NV12;
    }
    stillSc.bufferCount = kStillBufferCount;

    auto status = cfg->validate();
    if (status == CameraConfiguration::Invalid) {
        std::cerr << "DualStream: configuration invalid\n";
        return false;
    }

    // Detect HW MJPEG fallback (Pi VC4 may reject MJPEG at high res).
    if (capFmt == OutputFormat::JPEG && stillSc.pixelFormat != formats::MJPEG) {
#ifndef HAVE_JPEG
        std::cerr << "DualStream: HW MJPEG unavailable (got " << stillSc.pixelFormat
                  << ") and libjpeg was not compiled in — cannot encode JPEG\n";
        return false;
#else
        std::cerr << "DualStream: HW MJPEG unavailable (got " << stillSc.pixelFormat
                  << "), using software JPEG encode\n";
        stillSc.pixelFormat = formats::NV12;
        swJpegEncode_ = true;
        auto status2 = cfg->validate();
        if (status2 == CameraConfiguration::Invalid) {
            std::cerr << "DualStream: NV12 fallback invalid\n";
            return false;
        }
#endif
    }

    if (handle_.camera()->configure(cfg.get())) {
        std::cerr << "DualStream: cam->configure() failed\n";
        return false;
    }

    // Double-check MJPEG after configure (pipeline handler may change it).
    if (capFmt == OutputFormat::JPEG && !swJpegEncode_) {
        const auto &actualFmt = stillSc.stream()->configuration().pixelFormat;
        if (actualFmt != formats::MJPEG) {
#ifndef HAVE_JPEG
            std::cerr << "DualStream: HW MJPEG unavailable post-configure ("
                      << actualFmt << ") and libjpeg was not compiled in"
                      << " — cannot encode JPEG\n";
            return false;
#else
            std::cerr << "DualStream: HW MJPEG unavailable post-configure ("
                      << actualFmt << "), reconfiguring with NV12\n";
            stillSc.pixelFormat = formats::NV12;
            auto status2 = cfg->validate();
            if (status2 == libcamera::CameraConfiguration::Invalid) {
                std::cerr << "DualStream: NV12 validate() failed\n";
                return false;
            }
            if (handle_.camera()->configure(cfg.get())) {
                std::cerr << "DualStream: NV12 reconfigure failed\n";
                return false;
            }
            // Verify the fallback actually resulted in NV12 — the pipeline
            // handler could theoretically substitute yet another format.
            // Check the stream's configured pixel format (not stillSc, which
            // is the request object and may be stale after configure()).
            const auto &actualFmt2 = stillSc.stream()->configuration().pixelFormat;
            if (actualFmt2 != formats::NV12) {
                std::cerr << "DualStream: NV12 fallback rejected by pipeline (got "
                          << actualFmt2 << ")\n";
                return false;
            }
            swJpegEncode_ = true;
#endif
        }
    }

    vfStream_ = vfSc.stream();
    stillStream_ = stillSc.stream();
    vfWidth_ = vfSc.size.width;
    vfHeight_ = vfSc.size.height;
    vfStride_ = vfSc.stride;

    // Allocate buffers for both streams.
    allocator_ = std::make_unique<FrameBufferAllocator>(handle_.camera());
    if (allocator_->allocate(vfStream_) < 0) {
        std::cerr << "DualStream: VF buffer allocation failed\n";
        allocator_.reset();
        return false;
    }
    if (allocator_->allocate(stillStream_) < 0) {
        std::cerr << "DualStream: still buffer allocation failed\n";
        allocator_.reset();
        return false;
    }

    // Install the completion callback BEFORE starting the camera — the
    // safer libcamera pattern ensures no completion can fire before the
    // signal is connected (even though no buffers are queued until after).
    handle_.camera()->requestCompleted.connect(this, [this](Request *r) {
        // Track in-flight callbacks so stop() can wait for all
        // callbacks to exit before destroying Request objects.
        callbacksInFlight_.fetch_add(1, std::memory_order_acq_rel);
        CallbackGuard cbGuard{callbacksInFlight_, callbacksCv_, callbacksMtx_};

        // If shutdown is in progress, don't touch any state or re-queue.
        // The request will be cleaned up by stop().
        if (shuttingDown_.load(std::memory_order_acquire)) {
            r->reuse(Request::ReuseBuffers);
            return;
        }

        // Hold a shared_ptr snapshot of the camera for the whole callback
        // body so that shutdown() can't release it between the top-of-callback
        // check and any queueRequest below.
        auto cam = handle_.camera();
        if (!cam) return;

        const auto &buffers = r->buffers();
        if (buffers.empty()) {
            r->reuse(Request::ReuseBuffers);
            safeRequeue(cam.get(), r);
            return;
        }

        // Check if this is a still capture completion (still stream).
        bool isStill = false;
        for (const auto &[s, b] : buffers) {
            if (s == stillStream_) {
                isStill = true;
                break;
            }
        }

        if (isStill && stillInProgress_.load()) {
            handleStillFrame(r);
        } else if (r->status() != Request::RequestComplete) {
            handleVfError(r, cam.get());
        } else {
            handleVfFrame(r);
        }
    });

    // Start camera.
    if (handle_.camera()->start()) {
        std::cerr << "DualStream: cam->start() failed\n";
        handle_.camera()->requestCompleted.disconnect();
        allocator_.reset();
        return false;
    }
    started_.store(true, std::memory_order_release);

    // Queue all VF buffers.
    const auto &vfBuffers = allocator_->buffers(vfStream_);
    vfRequests_.reserve(vfBuffers.size());
    for (size_t i = 0; i < vfBuffers.size(); ++i) {
        auto req = handle_.camera()->createRequest();
        if (!req) {
            std::cerr << "DualStream: createRequest failed\n";
            failStart();
            return false;
        }
        if (req->addBuffer(vfStream_, vfBuffers[i].get())) {
            std::cerr << "DualStream: VF addBuffer failed\n";
            failStart();
            return false;
        }
        try {
            applyControls(req.get());
        } catch (const std::exception &e) {
            // applyControls failure means we can't set exposure/AWB controls
            // on this request. Queuing it with default/wrong controls would
            // stream bad frames — treat as fatal, clean up, and abort start.
            std::cerr << "DualStream: applyControls threw: " << e.what() << "\n";
            failStart();
            return false;
        }
        try {
            if (handle_.camera()->queueRequest(req.get())) {
                std::cerr << "DualStream: VF queueRequest failed\n";
                failStart();
                return false;
            }
        } catch (const std::exception &e) {
            std::cerr << "DualStream: VF queueRequest threw: " << e.what() << "\n";
            failStart();
            return false;
        }
        vfRequests_.push_back(std::move(req));
    }

    // Store still buffer reference for captureStill().
    const auto &stillBufs = allocator_->buffers(stillStream_);
    if (!stillBufs.empty()) {
        stillBuffer_ = stillBufs[0].get();
    }

    std::cout << "DualStream: VF " << vfWidth_ << "x" << vfHeight_
              << " + still " << capW << "x" << capH << " started\n";
    return true;
}

void DualStream::failStart() {
    stop();
    // stop() returns early when !started_ (e.g., if camera->start() failed),
    // so the requestCompleted signal may still be connected. Disconnect it
    // explicitly to avoid a dangling slot bound to this DualStream instance.
    if (handle_.camera()) {
        try {
            handle_.camera()->requestCompleted.disconnect();
        } catch (const std::exception &e) {
            std::cerr << "DualStream: failStart disconnect threw: " << e.what() << "\n";
        }
    }
    if (!fatalError_.load(std::memory_order_acquire)) allocator_.reset();
    vfStream_ = nullptr;
    stillStream_ = nullptr;
}

void DualStream::safeRequeue(libcamera::Camera *cam, Request *r) {
    if (shuttingDown_.load(std::memory_order_acquire)) return;
    if (!started_.load(std::memory_order_acquire)) return;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (shuttingDown_.load(std::memory_order_acquire)) return;
        if (!started_.load(std::memory_order_acquire)) return;
        try {
            if (cam->queueRequest(r) == 0) return;
            std::cerr << "DualStream: queueRequest returned error (attempt "
                      << attempt + 1 << ")\n";
        } catch (const std::exception &e) {
            std::cerr << "DualStream: queueRequest threw (attempt "
                      << attempt + 1 << "): " << e.what() << "\n";
        }
    }
    std::cerr << "DualStream: gave up re-queuing request after 3 attempts"
                 " — setting fatal error for clean restart\n";
    fatalError_.store(true, std::memory_order_release);
}

void DualStream::handleStillFrame(Request *r) {
    // Still capture completed — save it.
    // Copy the filename under the lock, then save without holding
    // it (saveFrame can be slow — don't block captureStill()).
    std::string filename;
    {
        std::lock_guard<std::mutex> lk(stillMtx_);
        filename = stillFilename_;
    }
    bool saved = false;
    if (r->status() == Request::RequestComplete) {
        // Wrap in try/catch: saveFrame allocates large buffers
        // (30+ MB for full-res conversion) and std::bad_alloc
        // would std::terminate() inside this libcamera callback.
        try {
            saved = saveFrame(r, filename);
        } catch (const std::bad_alloc &) {
            std::cerr << "DualStream: out of memory saving still\n";
        } catch (const std::exception &e) {
            std::cerr << "DualStream: saveFrame threw: " << e.what() << "\n";
        }
    }
    {
        std::lock_guard<std::mutex> lk(stillMtx_);
        stillSaved_ = saved;
        stillDone_ = true;
        stillCv_.notify_one();
    }
    // Release the buffer back to the pool by reusing the request.
    // Without this, the buffer stays attached to the completed
    // request and the next captureStill() can't addBuffer it.
    r->reuse(Request::ReuseBuffers);
    // Free the old request — a fresh one is created in the next
    // captureStill() call. This must happen after reuse() so the
    // buffer is detached before the Request is destroyed.
    stillRequest_.reset();
    // Clear stillInProgress_ AFTER the buffer is released, so a new
    // captureStill() can't addBuffer(stillBuffer_) while the previous
    // request still owns it.
    stillInProgress_.store(false);
}

void DualStream::handleVfError(Request *r, libcamera::Camera *cam) {
    // VF frame error — re-queue and continue. If applyControls
    // throws, skip the requeue to avoid streaming with wrong
    // exposure/AWB settings.
    r->reuse(Request::ReuseBuffers);
    bool controlsOk = true;
    try {
        applyControls(r);
    } catch (const std::exception &e) {
        std::cerr << "DualStream: applyControls threw: " << e.what() << "\n";
        controlsOk = false;
    }
    if (controlsOk) safeRequeue(cam, r);
}

void DualStream::handleVfFrame(Request *r) {
    // Read exposure metadata for the on-screen info display. ExposureTime
    // is int32 microseconds; AnalogueGain is float (1x = ISO100).
    // Done before the buffer copy so the UI thread sees consistent values
    // paired with the frame they describe.
    const auto &md = r->metadata();
    if (auto exp = md.get(controls::ExposureTime)) {
        // ExposureTime is in microseconds; convert to ms (round up to 1
        // so sub-millisecond exposures still display as "1/<n>").
        int32_t us = *exp;
        // Use int64_t for the division to avoid int32_t overflow when
        // us is close to INT32_MAX (~2.1s, us+999 would overflow).
        uint32_t ms = us > 0
            ? static_cast<uint32_t>((static_cast<int64_t>(us) + 999) / 1000)
            : 0;
        lastShutterMs_.store(ms, std::memory_order_release);
    }
    if (auto gain = md.get(controls::AnalogueGain)) {
        float g = std::max(0.0f, *gain);
        // gain*100 -> ISO equivalent (1x = ISO100, 4x = ISO400).
        // Clamp below UINT32_MAX before llround — float(UINT32_MAX) may
        // round to 2^32 which wraps to 0 in uint32_t. Saturate after
        // llround to guarantee a sane value for extreme gains.
        // Use llround (not lround) because long is 32-bit on some
        // platforms, and UINT32_MAX-1 exceeds LONG_MAX there.
        float isoF = std::min(g * 100.0f, static_cast<float>(UINT32_MAX - 1));
        long long iso = std::llround(isoF);
        lastIso_.store(static_cast<uint32_t>(std::min<long long>(iso, UINT32_MAX - 1)),
                       std::memory_order_release);
    }

    const auto &buffers = r->buffers();
    for (const auto &[s, b] : buffers) {
        if (s != vfStream_) continue;
        auto planes = b->planes();
        if (planes.size() < 2) continue;

        MappedPlane yPlane(planes[0], "vf-Y");
        MappedPlane uvPlane(planes[1], "vf-UV");
        if (!yPlane.valid() || !uvPlane.valid()) continue;

        size_t ySize = 0;
        size_t uvSize = 0;
        // NV12 UV plane has ceil(height/2) rows — use (height+1)/2
        // to avoid truncating the last UV row for odd heights.
        if (!checkedMul(static_cast<size_t>(vfStride_), vfHeight_, ySize) ||
            !checkedMul(static_cast<size_t>(vfStride_), (vfHeight_ + 1) / 2, uvSize)) {
            continue;
        }
        if (yPlane.size() < ySize || uvPlane.size() < uvSize) continue;

        // Wrap in try/catch: resize() can throw std::bad_alloc on the
        // 512MB Pi, which would std::terminate inside this callback.
        try {
            std::lock_guard<std::mutex> lk(vfMtx_);
            vfYData_.resize(ySize);
            vfUvData_.resize(uvSize);
            std::memcpy(vfYData_.data(), yPlane.data(), ySize);
            std::memcpy(vfUvData_.data(), uvPlane.data(), uvSize);
            vfFrameReady_ = true;
            vfCv_.notify_one();
        } catch (const std::exception &e) {
            std::cerr << "DualStream: VF frame copy failed: "
                      << e.what() << "\n";
            break;
        }
        break;
    }

    // Re-queue the VF request for continuous streaming.
    // Take a shared_ptr snapshot of the camera to avoid a race if
    // shutdown() resets the handle between the check and queueRequest.
    // If applyControls throws, skip the requeue to avoid streaming
    // with wrong exposure/AWB settings.
    r->reuse(Request::ReuseBuffers);
    bool controlsOk = true;
    try {
        applyControls(r);
    } catch (const std::exception &e) {
        std::cerr << "DualStream: applyControls threw: " << e.what() << "\n";
        controlsOk = false;
    }
    if (controlsOk && !shuttingDown_.load(std::memory_order_acquire)) {
        auto cam = handle_.camera();
        if (!cam) return;
        safeRequeue(cam.get(), r);
    }
}

bool DualStream::saveFrame(Request *req, const std::string &filename) {
    const auto &buffers = req->buffers();
    const auto it = buffers.find(stillStream_);
    if (it == buffers.end()) return false;

    auto *buffer = it->second;
    auto planes = buffer->planes();
    if (planes.empty()) return false;

    const auto &sc = stillStream_->configuration();

    // Override exposure/gain with the actual converged values from the
    // request metadata so DNG/EXIF tags are accurate even when AE is on.
    // Snapshot stillCfg_ under the mutex to avoid a data race with
    // updateStillConfig() which may run on the UI thread.
    CameraConfig writerCfg;
    {
        std::lock_guard<std::mutex> lk(cfgMtx_);
        writerCfg = stillCfg_;
    }
    const auto &md = req->metadata();
    if (auto exp = md.get(controls::ExposureTime)) {
        writerCfg.exposureTime = static_cast<uint64_t>(*exp);
    }
    if (auto gain = md.get(controls::AnalogueGain)) {
        writerCfg.analogueGain = *gain;
    }

    auto writer = makeOutputWriter(stillFmt_, writerCfg, swJpegEncode_);
    if (!writer) return false;

    MappedPlane p0(planes[0], "still-0");
    if (!p0.valid()) return false;

    MappedPlane p1;
    if (planes.size() >= 2) {
        p1 = MappedPlane(planes[1], "still-1");
    }

    FrameView frame;
    frame.width = sc.size.width;
    frame.height = sc.size.height;
    frame.stride = sc.stride;
    frame.plane0 = p0.data();
    frame.plane0Size = p0.size();
    frame.plane1 = p1.valid() ? p1.data() : nullptr;
    frame.plane1Size = p1.valid() ? p1.size() : 0;

    // Ask the writer for the actual saved path — it may differ from
    // `filename` if a uniqueness suffix (_2, _3, ...) was needed. The
    // caller (requestCompleted callback) stores this so waitCaptureDone()
    // can report the real path for the review screen to decode.
    std::string actualPath;
    bool ok = writer->write(frame, filename, &actualPath);
    if (ok) {
        std::lock_guard<std::mutex> lk(stillMtx_);
        stillSavedPath_ = actualPath.empty() ? filename : actualPath;
    }
    return ok;
}

bool DualStream::captureStill(const std::string &filename) {
    // Atomically test-and-set stillInProgress_ to prevent a TOCTOU race
    // where two callers both see false and both proceed to queue.
    bool expected = false;
    if (!stillInProgress_.compare_exchange_strong(expected, true))
        return false;
    if (!started_.load(std::memory_order_acquire)) {
        stillInProgress_.store(false);
        return false;
    }
    if (shuttingDown_.load(std::memory_order_acquire)) {
        stillInProgress_.store(false);
        return false;
    }
    if (!stillBuffer_) {
        stillInProgress_.store(false);
        return false;
    }

    auto req = handle_.camera()->createRequest();
    if (!req) { stillInProgress_.store(false); return false; }

    if (req->addBuffer(stillStream_, stillBuffer_)) {
        std::cerr << "DualStream: still addBuffer failed\n";
        stillInProgress_.store(false);
        return false;
    }
    try {
        applyControls(req.get());
    } catch (const std::exception &e) {
        std::cerr << "DualStream: applyControls threw: " << e.what() << "\n";
        stillInProgress_.store(false);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stillMtx_);
        stillFilename_ = filename;
        stillDone_ = false;
        stillSaved_ = false;
        stillInterrupted_ = false;
        stillSavedPath_.clear();
    }

    // Store the request BEFORE queueing so the callback can safely access
    // stillRequest_ without racing with the assignment below. The callback
    // fires on the libcamera thread and resets stillRequest_ at completion;
    // if we moved after queueRequest, the callback could fire between the
    // queue and the move, creating a data race on the unique_ptr.
    stillRequest_ = std::move(req);

    try {
        if (handle_.camera()->queueRequest(stillRequest_.get())) {
            stillInProgress_.store(false);
            stillRequest_.reset();
            return false;
        }
    } catch (const std::exception &e) {
        std::cerr << "DualStream: captureStill queueRequest threw: " << e.what() << "\n";
        stillInProgress_.store(false);
        stillRequest_.reset();
        return false;
    }
    return true;
}

bool DualStream::waitCaptureDone(int timeoutMs, std::string *savedPath) {
    std::unique_lock<std::mutex> lk(stillMtx_);
    bool ok = false;
    try {
        ok = stillCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                               [this] { return stillDone_ || stillInterrupted_; });
    } catch (const std::exception &e) {
        std::cerr << "DualStream: waitCaptureDone threw: " << e.what() << "\n";
        return false;
    }
    if (!ok) return false; // timeout
    if (stillInterrupted_) return false;  // stopped via stop()
    if (stillSaved_ && savedPath) *savedPath = stillSavedPath_;
    return stillSaved_;
}

bool DualStream::captureInProgress() const {
    return stillInProgress_.load();
}

void DualStream::updateStillConfig(const CameraConfig &cfg) {
    // Only update the encoder-relevant fields (quality, format). The stream
    // geometry (width/height/pixelFormat) is fixed at start() time and can't
    // change without a full stop/reconfigure/restart.
    std::lock_guard<std::mutex> lk(cfgMtx_);
    stillCfg_.jpegQuality = cfg.jpegQuality;
    stillCfg_.pngLevel = cfg.pngLevel;
    // Also propagate exposure/gain/AWB settings so manual shutter/ISO
    // from the settings menu or CLI are applied to still captures.
    stillCfg_.aeEnable = cfg.aeEnable;
    stillCfg_.exposureTime = cfg.exposureTime;
    stillCfg_.analogueGain = cfg.analogueGain;
    stillCfg_.digitalGain = cfg.digitalGain;
    stillCfg_.awbEnable = cfg.awbEnable;
    stillCfg_.awbMode = cfg.awbMode;
}

bool DualStream::reconfigureStill(uint32_t vfW, uint32_t vfH,
                                   uint32_t capW, uint32_t capH,
                                   OutputFormat capFmt) {
    // Full stop + restart with the new format. The camera must be released
    // before reconfiguring.
    stop();
    // If stop() timed out with stuck callbacks, do NOT free the allocator
    // or streams — a callback may still be using them (use-after-free).
    // Return false so the caller exits and systemd restarts the process.
    if (fatalError_.load(std::memory_order_acquire)) return false;
    allocator_.reset();
    vfStream_ = nullptr;
    stillStream_ = nullptr;
    return start(vfW, vfH, capW, capH, capFmt);
}

void DualStream::setMeteringLock(bool locked) {
    meteringLocked_.store(locked, std::memory_order_release);
}

void DualStream::applyControls(Request *req) const {
    auto &ctrls = req->controls();
    // Clear any stale controls from a previous request lifecycle.
    // libcamera may reuse Request objects, and without clearing, settings
    // from a prior frame (e.g. AeEnable=false) could persist unexpectedly.
    ctrls.clear();
    // Snapshot stillCfg_ under the lock to avoid a data race with
    // updateStillConfig() which writes from the UI thread.
    CameraConfig cfg;
    {
        std::lock_guard<std::mutex> lk(cfgMtx_);
        cfg = stillCfg_;
    }
    if (meteringLocked_.load(std::memory_order_acquire)) {
        // Freeze AE/AWB at current values — emulates half-press metering
        // lock on a mirrorless camera.
        //   AeEnable=false  -> AE switches to manual, holding the last
        //                      converged exposure time + analogue gain.
        //   AwbEnable=true + AwbLocked=true -> AWB keeps running but is
        //                      told to hold its current white-balance gains
        //                      (the idiomatic lock; AwbEnable=false would
        //                      drop to manual mode and ignore AwbLocked).
        ctrls.set(controls::AeEnable, false);
        ctrls.set(controls::AwbEnable, true);
        ctrls.set(controls::AwbLocked, true);
        // Still honor --digital-gain and --awb mode while locked, matching
        // CameraApp::applyControls where these are set independently of AE.
        if (cfg.digitalGain > 0.0f) {
            ctrls.set(controls::DigitalGain, cfg.digitalGain);
        }
        if (auto mode = lookupAwb(cfg.awbMode)) {
            ctrls.set(controls::AwbMode, *mode);
        }
        return;
    }
    if (cfg.exposureTime > 0 || cfg.analogueGain > 0.0f || !cfg.aeEnable) {
        // Manual exposure/gain from settings — mirror CameraApp::applyControls.
        ctrls.set(controls::AeEnable, false);
        if (cfg.exposureTime > 0) {
            ctrls.set(controls::ExposureTime,
                      static_cast<int32_t>(std::min<uint64_t>(
                          cfg.exposureTime, INT32_MAX)));
        }
        if (cfg.analogueGain > 0.0f) {
            ctrls.set(controls::AnalogueGain, cfg.analogueGain);
        }
    } else {
        // Continuous AE for accurate metering.
        ctrls.set(controls::AeEnable, true);
    }
    // Digital gain is a post-processing gain, not an exposure control —
    // set it independently of AE mode so --digital-gain doesn't disable AE.
    if (cfg.digitalGain > 0.0f) {
        ctrls.set(controls::DigitalGain, cfg.digitalGain);
    }
    // AWB handling — mirror CameraApp::applyControls so --awb works in preview.
    if (!cfg.awbEnable) {
        ctrls.set(controls::AwbEnable, false);
    } else {
        ctrls.set(controls::AwbEnable, true);
        ctrls.set(controls::AwbLocked, false);
        if (auto mode = lookupAwb(cfg.awbMode)) {
            ctrls.set(controls::AwbMode, *mode);
        }
    }
}

StreamFrame DualStream::grabFrame(int timeoutMs) {
    StreamFrame frame;
    if (!started_.load(std::memory_order_acquire)) return frame;

    std::unique_lock<std::mutex> lk(vfMtx_);
    if (!vfCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                        [this] { return vfFrameReady_; })) {
        return frame;
    }
    vfFrameReady_ = false;

    // Copy the frame data while holding the lock so the callback can't
    // overwrite it while the caller is using the returned frame.
    frame.yData = vfYData_;
    frame.uvData = vfUvData_;
    frame.width = vfWidth_;
    frame.height = vfHeight_;
    frame.stride = vfStride_;
    return frame;
}

void DualStream::stop() noexcept {
    if (!started_.load(std::memory_order_acquire) || !handle_.camera()) return;

    // Set shuttingDown_ before disconnect so any in-flight callback sees
    // it and returns without touching vfRequests_/stillRequest_ state.
    shuttingDown_.store(true, std::memory_order_release);

    try {
        handle_.camera()->requestCompleted.disconnect();
        handle_.camera()->stop();
    } catch (const std::exception &e) {
        std::cerr << "DualStream: stop() threw: " << e.what() << "\n";
    }
    started_.store(false, std::memory_order_release);

    // Wait for any in-flight callbacks to exit before destroying Request
    // objects. camera->stop() cancels pending requests and fires their
    // callbacks, but doesn't guarantee they've returned. Use a 60s timeout
    // (saveFrame can be slow for full-res JPEG/DNG writes on slow SD cards).
    // If callbacks haven't exited, set fatalError_ so the caller exits
    // gracefully (systemd Restart=on-failure handles the restart).
    // Wrap in try/catch: mutex/CV ops can throw std::system_error,
    // which would std::terminate() inside this noexcept function.
    try {
        std::unique_lock<std::mutex> lk(callbacksMtx_);
        if (!callbacksCv_.wait_for(lk, std::chrono::seconds(60),
                [this] { return callbacksInFlight_.load(std::memory_order_acquire) == 0; })) {
            std::cerr << "DualStream: FATAL: callbacks stuck after 60s — requesting shutdown\n";
            fatalError_.store(true, std::memory_order_release);
            // Do NOT clear vfRequests_/stillRequest_ — a callback may still
            // be dereferencing them. Leak until process death (systemd
            // Restart=on-failure handles cleanup). Freeing under a live
            // callback would be use-after-free.
            // Wake any thread blocked in waitCaptureDone() so it returns
            // promptly instead of waiting for the full 5s timeout.
            {
                std::lock_guard<std::mutex> lk(stillMtx_);
                stillInterrupted_ = true;
            }
            stillInProgress_.store(false);
            stillCv_.notify_all();
            shuttingDown_.store(false, std::memory_order_release);
            return;
        }
    } catch (const std::exception &e) {
        std::cerr << "DualStream: callback wait threw: " << e.what() << "\n";
    }

    // Wake any thread blocked in waitCaptureDone() so it returns promptly
    // instead of waiting for the full timeout. Use stillInterrupted_ rather
    // than overwriting stillDone_/stillSaved_ — the callback may still be
    // in saveFrame() and should remain the sole writer of stillSaved_.
    {
        std::lock_guard<std::mutex> lk(stillMtx_);
        stillInterrupted_ = true;
        stillCv_.notify_all();
    }
    stillInProgress_.store(false);

    vfRequests_.clear();
    stillRequest_.reset();
    stillBuffer_ = nullptr;

    shuttingDown_.store(false, std::memory_order_release);
}

void DualStream::shutdown() noexcept {
    stop();
    // If stop() timed out with stuck callbacks, fatalError_ is set and
    // vfRequests_/stillRequest_ were intentionally leaked (a callback may
    // still be dereferencing them). Do NOT free the allocator or clear
    // streams — that would be use-after-free. Let the process exit and
    // systemd Restart=on-failure handle cleanup.
    if (fatalError_.load(std::memory_order_acquire)) return;
    allocator_.reset();
    handle_.shutdown();
    vfStream_ = nullptr;
    stillStream_ = nullptr;
}

} // namespace picamera
