#pragma once

#include "camera_config.h"
#include "camera_handle.h"
#include "output_writer.h"
#include "stream.h"

#include <libcamera/libcamera.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace picamera {

// Dual-stream camera: runs a continuous low-res viewfinder (NV12) and an
// on-demand full-res still capture stream simultaneously, eliminating the
// 1s+ viewfinder blackout of the stop/restart capture flow.
//
// The Pi ISP supports multiple streams from one sensor: a Viewfinder role
// (low-res, continuous) and a StillCapture role (full-res, one-shot). Both
// share the same sensor mode and AE/AWB convergence, so the still is
// metered identically to what the user sees on the viewfinder.
//
// Usage:
//   DualStream cam;
//   cam.init();
//   cam.start(viewfinderW, viewfinderH, captureW, captureH, captureFmt);
//   loop {
//     StreamFrame vf = cam.grabFrame();   // viewfinder frame (NV12)
//     if (shutter_pressed) {
//       cam.captureStill(filename);        // non-blocking: returns immediately
//       cam.waitCaptureDone(timeout);      // wait for save to complete
//     }
//   }
//   cam.stop();
class DualStream {
public:
    DualStream();
    ~DualStream();

    bool init();
    bool start(uint32_t vfW, uint32_t vfH,
               uint32_t capW, uint32_t capH,
               OutputFormat capFmt);
    void stop() noexcept;
    void shutdown() noexcept;

    // Viewfinder access — returns the latest viewfinder frame.
    StreamFrame grabFrame(int timeoutMs = 2000);
    uint32_t vfWidth() const { return vfWidth_; }
    uint32_t vfHeight() const { return vfHeight_; }
    uint32_t vfStride() const { return vfStride_; }

    // Still capture: queue a one-shot capture request. Returns false if
    // a capture is already in progress or the still stream isn't ready.
    // The capture runs asynchronously — call waitCaptureDone() to block
    // until the file is saved (or the timeout expires).
    bool captureStill(const std::string &filename);

    // Wait for the in-progress capture to complete. Returns true if the
    // still was saved successfully, false on timeout or save failure.
    // On success, if `savedPath` is non-null it is set to the actual file
    // path written to (may differ from the requested filename if a
    // uniqueness suffix was needed).
    bool waitCaptureDone(int timeoutMs = 5000,
                         std::string *savedPath = nullptr);

    // True if a capture is currently in progress (request queued, not
    // yet completed). The viewfinder keeps running during captures.
    bool captureInProgress() const;

    // The active still-capture format. This is the format the still stream
    // was configured with at start()/reconfigureStill() time — it determines
    // the file content. Use this (not the UI settings) to build the filename
    // extension so the file matches its content.
    OutputFormat stillFormat() const { return stillFmt_; }

    // Update the still-capture config (jpegQuality, format, etc.) without
    // restarting the stream. Takes effect on the next captureStill() call.
    void updateStillConfig(const CameraConfig &cfg);

    // Reconfigure the still stream for a new capture format. Requires a
    // full stop/restart of the camera (the still stream's pixel format
    // can't change while streaming). The viewfinder resumes after.
    // Returns true on success, false on failure (camera left stopped).
    bool reconfigureStill(uint32_t vfW, uint32_t vfH,
                          uint32_t capW, uint32_t capH,
                          OutputFormat capFmt);

    // Lock or unlock AE/AWB metering. When locked, the exposure settings
    // are frozen at their current values (emulates half-press on a
    // mirrorless camera). Unlocking returns to continuous AE/AWB.
    void setMeteringLock(bool locked);

    // Latest exposure metadata from the viewfinder stream, for the on-screen
    // info display (shutter speed + ISO). Updated on every VF frame completion.
    // Returns 0 if no metadata has been received yet.
    uint32_t lastShutterMs() const { return lastShutterMs_.load(std::memory_order_acquire); }
    uint32_t lastIso() const { return lastIso_.load(std::memory_order_acquire); }
    bool fatalError() const { return fatalError_.load(std::memory_order_acquire); }

private:
    // Apply AE/AWB controls to a request (shared between VF and still).
    // Reads meteringLocked_ to decide whether to freeze or run AE/AWB.
    void applyControls(libcamera::Request *req) const;

    // Handle a viewfinder frame completion: copy data, re-queue request.
    void handleVfFrame(libcamera::Request *r);

    // Handle a completed still capture: save the frame and release the
    // still request back to the pool. Called from requestCompleted.
    void handleStillFrame(libcamera::Request *r);

    // Handle a viewfinder frame error (non-RequestComplete status):
    // re-queue the request after re-applying controls. Called from
    // requestCompleted.
    void handleVfError(libcamera::Request *r, libcamera::Camera *cam);

    // Safely re-queue a request with retry, checking shuttingDown_ before
    // each attempt. Used by handleVfError and the callback dispatcher.
    void safeRequeue(libcamera::Camera *cam, libcamera::Request *r);

    // Common cleanup for start() failure paths: stop the camera, reset the
    // allocator (unless a fatal error already did), and clear stream pointers.
    void failStart();

    // Save a completed still capture frame to disk.
    bool saveFrame(libcamera::Request *req, const std::string &filename);

    CameraHandle handle_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    libcamera::Stream *vfStream_ = nullptr;
    libcamera::Stream *stillStream_ = nullptr;

    // Viewfinder buffers + requests (continuously re-queued).
    std::vector<std::unique_ptr<libcamera::Request>> vfRequests_;

    // Still capture: one request at a time. Buffer lifetime is managed by
    // the libcamera FrameBufferAllocator; stillBuffer_ is a non-owning
    // pointer into its pool.
    std::unique_ptr<libcamera::Request> stillRequest_;
    libcamera::FrameBuffer *stillBuffer_ = nullptr;

    // Viewfinder frame data (copied from dmabuf on each completion).
    std::mutex vfMtx_;
    std::condition_variable vfCv_;
    bool vfFrameReady_ = false;
    std::vector<uint8_t> vfYData_;
    std::vector<uint8_t> vfUvData_;
    uint32_t vfWidth_ = 0, vfHeight_ = 0, vfStride_ = 0;

    // Still capture state.
    std::mutex stillMtx_;
    std::condition_variable stillCv_;
    std::atomic<bool> stillInProgress_{false};
    bool stillDone_ = false;
    bool stillSaved_ = false;
    bool stillInterrupted_ = false;  // set by stop() to wake waitCaptureDone()
    std::string stillFilename_;
    std::string stillSavedPath_;  // actual path written to (may have suffix)
    OutputFormat stillFmt_ = OutputFormat::JPEG;
    bool swJpegEncode_ = false;
    CameraConfig stillCfg_;
    // Mutex protecting stillCfg_ — written by updateStillConfig() on the
    // UI thread, read by applyControls() on the callback thread.
    mutable std::mutex cfgMtx_;
    // Atomic: written by setMeteringLock() from the UI thread, read by
    // applyControls() on libcamera's request-completion thread.
    // Uses acquire/release ordering so the callback thread sees the latest
    // value promptly (relaxed could delay the lock toggle by a frame).
    std::atomic<bool> meteringLocked_{false};

    // Latest exposure metadata from the VF stream (read by the UI thread
    // for the on-screen info display). ExposureTime is int32 microseconds;
    // we convert to ms on read. AnalogueGain is float; we report ISO as
    // gain*100 (so 1x = ISO100, 4x = ISO400, like a real camera).
    std::atomic<uint32_t> lastShutterMs_{0};
    std::atomic<uint32_t> lastIso_{0};

    std::atomic<bool> started_{false};
    std::atomic<bool> shuttingDown_{false};  // set in stop() before clearing state

    // In-flight callback counter: incremented on callback entry, decremented
    // on exit. stop() waits for this to reach 0 before clearing vfRequests_
    // and stillRequest_, ensuring no callback is dereferencing a Request
    // when its unique_ptr is destroyed.
    std::atomic<int> callbacksInFlight_{0};
    std::mutex callbacksMtx_;
    std::condition_variable callbacksCv_;
    // Set when callbacks are stuck after timeout — signals the caller to
    // exit gracefully instead of aborting (systemd Restart=on-failure).
    std::atomic<bool> fatalError_{false};
};

} // namespace picamera
