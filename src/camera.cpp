#include "camera.h"
#include "output_writer.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <csignal>
#include <stdexcept>
#include <array>
#include <optional>
#include <string_view>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unistd.h>
#include <sys/mman.h>

#include <libcamera/control_ids.h>
#include <libcamera/property_ids.h>
#include <libcamera/formats.h>

using namespace libcamera;
using namespace std::chrono_literals;

namespace picamera {

namespace {

// AWB mode name -> enum. A plain array of pairs avoids the heap allocation
// (and potential throwing) of a std::map with static storage duration; linear
// search over 7 entries is faster than a map lookup anyway.
struct AwbEntry {
    const char *name;
    controls::AwbModeEnum mode;
};
constexpr AwbEntry kAwbTable[] = {
    {"auto",         controls::AwbAuto},
    {"incandescent", controls::AwbIncandescent},
    {"tungsten",     controls::AwbTungsten},
    {"fluorescent",  controls::AwbFluorescent},
    {"indoor",       controls::AwbIndoor},
    {"daylight",     controls::AwbDaylight},
    {"cloudy",       controls::AwbCloudy},
};

std::optional<controls::AwbModeEnum> lookupAwb(std::string_view name) {
    for (const auto &e : kAwbTable) {
        if (name == e.name) return e.mode;
    }
    return std::nullopt;
}
} // namespace

CameraApp::CameraApp() = default;
CameraApp::~CameraApp() { shutdown(); }

bool CameraApp::init() {
    cm_ = std::make_shared<CameraManager>();
    if (cm_->start()) {
        std::cerr << "CameraManager::start() failed\n";
        return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
        std::cerr << "No cameras found\n";
        return false;
    }

    cam_ = cameras[0];
    std::cout << "Camera: " << cam_->id() << "\n";

    if (cam_->acquire()) {
        std::cerr << "Failed to acquire camera\n";
        return false;
    }

    return true;
}

bool CameraApp::configure(const CameraConfig &cfg) {
    config_ = cfg;

    // DNG capture needs a raw Bayer stream; everything else uses StillCapture.
    auto roles = (cfg.format == OutputFormat::DNG)
                 ? std::vector<StreamRole>{StreamRole::Raw}
                 : std::vector<StreamRole>{StreamRole::StillCapture};
    auto camCfg = cam_->generateConfiguration(roles);
    if (!camCfg) {
        std::cerr << "generateConfiguration failed\n";
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
    sc.bufferCount = 4;

    // For high-res NV12 captures (e.g. 4056x3040), the Pi may not have
    // enough memory for 4 buffers. Use 1 buffer for still capture.
    if (sc.pixelFormat == formats::NV12 && cfg.width * cfg.height > 2000000) {
        sc.bufferCount = 1;
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
        std::cerr << "Warning: HW MJPEG unavailable (got " << sc.pixelFormat
                  << "), falling back to software JPEG encode\n";
        sc.pixelFormat = formats::NV12;
        // High-res NV12 needs fewer buffers on the Pi
        if (cfg.width * cfg.height > 2000000)
            sc.bufferCount = 1;
        auto status2 = camCfg->validate();
        if (status2 == CameraConfiguration::Invalid) {
            std::cerr << "Camera configuration invalid (NV12 fallback)\n";
            return false;
        }
        swJpegEncode_ = true;
    }

    if (cam_->configure(camCfg.get())) {
        std::cerr << "cam->configure() failed\n";
        return false;
    }

    stream_ = sc.stream();

    // Double-check: configure() may silently change the pixel format even
    // if validate() accepted MJPEG. Check the actual stream configuration.
    if (cfg.format == OutputFormat::JPEG && !swJpegEncode_) {
        const auto &actualFmt = stream_->configuration().pixelFormat;
        if (actualFmt != formats::MJPEG) {
            std::cerr << "Warning: HW MJPEG unavailable after configure (got "
                      << actualFmt << "), reconfiguring with NV12\n";
            // Reconfigure with NV12
            sc.pixelFormat = formats::NV12;
            if (cfg.width * cfg.height > 2000000)
                sc.bufferCount = 1;
            camCfg->validate();
            if (cam_->configure(camCfg.get())) {
                std::cerr << "cam->configure() failed (NV12 fallback)\n";
                return false;
            }
            swJpegEncode_ = true;
        }
    }

    allocator_ = std::make_unique<FrameBufferAllocator>(cam_);
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

bool CameraApp::capture(const std::string &filename) {
    if (!cam_ || !allocator_) {
        std::cerr << "Camera not initialized\n";
        return false;
    }

    const auto &buffers = allocator_->buffers(stream_);
    if (buffers.empty()) return false;

    if (cam_->start()) {
        std::cerr << "Failed to start camera\n";
        return false;
    }
    started_ = true;

    // Queue all available buffers so AE/AWB can run continuously and converge.
    // We discard the first `warmupFrames` completions, re-queuing their buffers
    // (re-applying controls, since reuse(ReuseBuffers) clears per-request
    // controls), then save the next completed frame.
    const uint32_t warmup = config_.warmupFrames;
    uint32_t completed = 0;
    bool done = false;
    bool saved = false;

    std::mutex mtx;
    std::condition_variable cv;

    cam_->requestCompleted.connect(this, [&](Request *r) {
        if (r->status() != Request::RequestComplete) {
            std::cerr << "Request status: " << r->status() << "\n";
            r->reuse(Request::ReuseBuffers);
            applyControls(r, config_);
            cam_->queueRequest(r);
            return;
        }
        ++completed;
        if (completed <= warmup) {
            // Warmup frame — discard and re-queue so AE/AWB keeps converging.
            r->reuse(Request::ReuseBuffers);
            applyControls(r, config_);
            cam_->queueRequest(r);
            return;
        }
        // Converged frame — save it.
        {
            std::lock_guard<std::mutex> lk(mtx);
            saved = saveFrame(r, filename);
            done = true;
        }
        cv.notify_one();
    });

    // Initial queue: one request per buffer, with controls applied to each.
    std::vector<std::unique_ptr<Request>> reqs;
    reqs.reserve(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto req = cam_->createRequest();
        if (!req) {
            std::cerr << "Failed to create Request\n";
            cam_->requestCompleted.disconnect();
            stopCamera();
            return false;
        }
        req->addBuffer(stream_, buffers[i].get());
        applyControls(req.get(), config_);
        if (cam_->queueRequest(req.get())) {
            std::cerr << "Failed to queue Request\n";
            cam_->requestCompleted.disconnect();
            stopCamera();
            return false;
        }
        reqs.push_back(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + 60s;
    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_until(lk, deadline, [&] { return done; })) {
            std::cerr << "Capture timed out (completed " << completed
                      << "/" << (warmup + 1) << " frames)\n";
            cam_->requestCompleted.disconnect();
            stopCamera();
            return false;
        }
    }

    cam_->requestCompleted.disconnect();
    stopCamera();

    if (!saved) std::cerr << "Failed to save frame\n";
    return saved;
}

bool CameraApp::captureBracket(const std::string &baseFilename) {
    // HDR bracketing: capture one frame per EV offset in cfg.bracketEv.
    // For each frame, we adjust the exposure time by 2^ev relative to the
    // base exposure. If AE is enabled, the first warmup frames let it
    // converge; then we lock to manual for the bracketed captures.
    //
    // Filenames: baseFilename with _evN suffix inserted before the extension.
    // e.g. "photo.png" -> "photo_ev-2.png", "photo_ev0.png", "photo_ev+2.png"
    if (config_.bracketEv.empty()) {
        return capture(baseFilename);
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

    bool allOk = true;
    for (size_t i = 0; i < config_.bracketEv.size(); ++i) {
        float ev = config_.bracketEv[i];

        // Create a modified config for this bracket frame.
        CameraConfig bracketCfg = config_;
        // Adjust exposure time by 2^ev. If exposureTime is 0 (auto), we
        // need to let AE converge first, then lock. For simplicity, if
        // exposureTime is set, we scale it; if not, we use ExposureValue
        // control via the EV compensation.
        if (bracketCfg.exposureTime > 0) {
            // Manual exposure: scale by 2^ev
            double factor = std::pow(2.0, static_cast<double>(ev));
            bracketCfg.exposureTime = static_cast<uint64_t>(
                bracketCfg.exposureTime * factor);
        }
        // If AE is on, the EV offset is applied via ExposureValue control
        // in applyControls (we'd need to add it to CameraConfig). For now,
        // bracketing works best with manual exposure (--shutter + --iso).

        // Build filename with EV suffix.
        char evStr[16];
        std::snprintf(evStr, sizeof(evStr), "%+.1f", ev);
        std::string fname = base;
        fname += "_ev";
        fname += evStr;
        fname += ext;

        std::cout << "Bracket " << (i + 1) << "/" << config_.bracketEv.size()
                  << ": EV" << evStr << " -> " << fname << "\n";

        // Reconfigure with the bracket config and capture.
        if (!configure(bracketCfg)) {
            std::cerr << "Failed to configure for bracket EV" << evStr << "\n";
            allOk = false;
            continue;
        }
        if (!capture(fname)) {
            std::cerr << "Failed to capture bracket EV" << evStr << "\n";
            allOk = false;
        }
    }
    return allOk;
}

void CameraApp::listControls() {
    if (!cam_) return;

    const auto &controls = cam_->controls();
    std::cout << "=== Controls ===\n";
    for (const auto &[id, info] : controls) {
        std::cout << "  " << id->name() << ": " << info.toString() << "\n";
    }

    const auto &props = cam_->properties();
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

void CameraApp::stopCamera() {
    if (started_ && cam_) {
        cam_->stop();
        started_ = false;
    }
}

void CameraApp::shutdown() {
    stopCamera();
    if (cam_) {
        allocator_.reset();
        cam_->release();
        cam_.reset();
    }
    cm_.reset();
}

void CameraApp::applyControls(Request *req, const CameraConfig &cfg) {
    auto &ctrls = req->controls();

    // If the user asked for a manual shutter or gain, AE must be off or
    // libcamera will ignore ExposureTime/AnalogueGain. Auto-disable AE in
    // that case so --shutter / --iso do what the user expects without
    // requiring an explicit --ae-disable.
    const bool manualExposure = !cfg.aeEnable || cfg.exposureTime > 0 ||
                                cfg.analogueGain > 0.0f;
    if (manualExposure) {
        ctrls.set(controls::AeEnable, false);
        if (cfg.exposureTime > 0) {
            ctrls.set(controls::ExposureTime, cfg.exposureTime);
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
        if (auto mode = lookupAwb(cfg.awbMode)) {
            ctrls.set(controls::AwbMode, *mode);
        }
    }
}

bool CameraApp::saveFrame(const Request *req, const std::string &filename) {
    const auto &buffers = req->buffers();
    if (buffers.empty()) return false;

    const auto &[stream, buffer] = *buffers.begin();
    auto planes = buffer->planes();
    if (planes.empty()) {
        std::cerr << "No planes in buffer\n";
        return false;
    }

    const auto &sc = stream_->configuration();

    auto writer = makeOutputWriter(config_.format, config_, swJpegEncode_);
    if (!writer) {
        std::cerr << "No output writer for format\n";
        return false;
    }

    // Map each plane through its own dmabuf fd. libcamera backends may put
    // the two NV12 planes in the same dmabuf (Pi VC4) or in separate ones;
    // mapping per-fd is correct in both cases. Single-plane formats (DNG
    // raw, HW MJPEG) only map planes[0].
    struct MappedPlane {
        const uint8_t *data = nullptr;
        void *base = nullptr;
        size_t mapLen = 0;
    };
    auto mapPlane = [](const libcamera::FrameBuffer::Plane &pl, const char *what)
        -> MappedPlane {
        void *base = mmap(nullptr, pl.offset + pl.length, PROT_READ,
                          MAP_SHARED, pl.fd.get(), 0);
        if (base == MAP_FAILED) {
            std::cerr << "mmap " << what << " failed: " << strerror(errno) << "\n";
            return {nullptr, nullptr, 0};
        }
        return {static_cast<const uint8_t *>(base) + pl.offset, base,
                static_cast<size_t>(pl.offset + pl.length)};
    };

    MappedPlane p0 = mapPlane(planes[0], "plane0");
    if (!p0.data) return false;

    MappedPlane p1;
    if (planes.size() >= 2) {
        p1 = mapPlane(planes[1], "plane1");
        if (!p1.data) {
            munmap(p0.base, p0.mapLen);
            return false;
        }
    }

    FrameView frame;
    frame.width = sc.size.width;
    frame.height = sc.size.height;
    frame.stride = sc.stride;
    frame.plane0 = p0.data;
    frame.plane0Size = planes[0].length;
    frame.plane1 = p1.data;
    frame.plane1Size = planes.size() >= 2 ? planes[1].length : 0;

    bool ok = writer->write(frame, filename);

    if (p1.data) munmap(p1.base, p1.mapLen);
    munmap(p0.base, p0.mapLen);
    return ok;
}

}
