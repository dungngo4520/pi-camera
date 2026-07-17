#include "camera.h"
#include "image.h"
#include "output.h"
#include "timelapse.h"
#include "dng.h"

#include <iostream>
#include <fstream>
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

// SIGINT/SIGTERM flag for graceful timelapse interruption.
namespace {
std::atomic<bool> g_stopRequested{false};
void stopSignalHandler(int) { g_stopRequested.store(true); }
void installStopHandler() {
    g_stopRequested.store(false);
    std::signal(SIGINT, stopSignalHandler);
    std::signal(SIGTERM, stopSignalHandler);
}
void restoreStopHandler() {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
}

// AWB mode name -> enum. A plain array of pairs avoids the heap allocation
// (and potential throwing) of a std::map with static storage duration; linear
// search over 7 entries is faster than a map lookup anyway.
struct AwbEntry {
    const char *name;
    controls::AwbModeEnum mode;
};
constexpr AwbEntry awbTable[] = {
    {"auto",         controls::AwbAuto},
    {"incandescent", controls::AwbIncandescent},
    {"tungsten",     controls::AwbTungsten},
    {"fluorescent",  controls::AwbFluorescent},
    {"indoor",       controls::AwbIndoor},
    {"daylight",     controls::AwbDaylight},
    {"cloudy",       controls::AwbCloudy},
};

std::optional<controls::AwbModeEnum> lookupAwb(std::string_view name) {
    for (const auto &e : awbTable) {
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

    auto status = camCfg->validate();
    if (status == CameraConfiguration::Invalid) {
        std::cerr << "Camera configuration invalid\n";
        return false;
    }

    if (cam_->configure(camCfg.get())) {
        std::cerr << "cam->configure() failed\n";
        return false;
    }

    stream_ = sc.stream();

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

bool CameraApp::timelapse(int intervalSec, int count, const std::string &pattern) {
    bool infinite = (count == 0);

    installStopHandler();

    for (int i = 0; infinite || i < count; ++i) {
        if (g_stopRequested.load()) {
            std::cerr << "\nTimelapse interrupted by signal after " << i
                      << " shots\n";
            break;
        }

        std::string filename;
        try {
            filename = formatTimelapseName(pattern, i);
        } catch (const std::exception &e) {
            std::cerr << "Bad --output pattern: " << e.what() << "\n";
            restoreStopHandler();
            return false;
        }
        std::cout << "[" << (i + 1) << (infinite ? "/inf" : "/" + std::to_string(count))
                  << "] " << filename << "\n";

        if (!capture(filename)) {
            std::cerr << "Capture failed at shot " << i << "\n";
            restoreStopHandler();
            return false;
        }

        if ((infinite || i < count - 1) && !g_stopRequested.load()) {
            // Sleep in small increments so a signal is noticed promptly.
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSec);
            while (std::chrono::steady_clock::now() < end) {
                if (g_stopRequested.load()) break;
                std::this_thread::sleep_for(200ms);
            }
        }
    }

    restoreStopHandler();
    return true;
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
    uint32_t w = sc.size.width;
    uint32_t h = sc.size.height;

    // --- DNG path: raw Bayer data, unpack to 16-bit, write DNG container. ---
    if (config_.format == OutputFormat::DNG) {
        const auto &plane = planes[0];
        void *base = mmap(nullptr, plane.offset + plane.length, PROT_READ,
                          MAP_SHARED, plane.fd.get(), 0);
        if (base == MAP_FAILED) {
            std::cerr << "mmap raw failed: " << strerror(errno) << "\n";
            return false;
        }
        auto *rawData = static_cast<uint8_t *>(base) + plane.offset;

        // SRGGB10_CSI2P packs 4 10-bit pixels into 5 bytes (MIPI CSI-2
        // packed format). Unpack to 16-bit samples for the DNG writer.
        // Each 5-byte group: [b0 b1 b2 b3 | b4]
        //   pixel0 = b0 | ((b4 & 0x03) << 8)
        //   pixel1 = b1 | ((b4 & 0x0C) << 6)
        //   pixel2 = b2 | ((b4 & 0x30) << 4)
        //   pixel3 = b3 | ((b4 & 0xC0) << 2)
        size_t numPixels = static_cast<size_t>(w) * h;
        std::vector<uint8_t> unpacked(numPixels * 2);
        size_t packedSize = (numPixels / 4) * 5;
        for (size_t i = 0, p = 0; p < packedSize; i += 4, p += 5) {
            uint16_t p0 = rawData[p]     | ((rawData[p + 4] & 0x03) << 8);
            uint16_t p1 = rawData[p + 1] | ((rawData[p + 4] & 0x0C) << 6);
            uint16_t p2 = rawData[p + 2] | ((rawData[p + 4] & 0x30) << 4);
            uint16_t p3 = rawData[p + 3] | ((rawData[p + 4] & 0xC0) << 2);
            unpacked[i * 2]     = p0 & 0xFF;
            unpacked[i * 2 + 1] = (p0 >> 8) & 0xFF;
            unpacked[(i+1) * 2]     = p1 & 0xFF;
            unpacked[(i+1) * 2 + 1] = (p1 >> 8) & 0xFF;
            unpacked[(i+2) * 2]     = p2 & 0xFF;
            unpacked[(i+2) * 2 + 1] = (p2 >> 8) & 0xFF;
            unpacked[(i+3) * 2]     = p3 & 0xFF;
            unpacked[(i+3) * 2 + 1] = (p3 >> 8) & 0xFF;
        }

        DngMetadata dngMeta;
        dngMeta.width = w;
        dngMeta.height = h;
        dngMeta.bitsPerPixel = 10;
        dngMeta.blackLevel = 64;    // typical IMX477 black level
        dngMeta.whiteLevel = 1023;  // 10-bit max
        dngMeta.activeTop = 0;
        dngMeta.activeLeft = 0;
        dngMeta.activeBottom = h;
        dngMeta.activeRight = w;
        dngMeta.exposureTimeUs = config_.exposureTime;
        dngMeta.analogueGain = config_.analogueGain;
        dngMeta.timestampSec = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        bool ok = writeDng(filename.c_str(), unpacked.data(), unpacked.size(),
                           dngMeta);
        munmap(base, plane.offset + plane.length);
        if (ok) {
            std::cout << "Saved DNG: " << filename << " (" << w << "x" << h
                      << ") " << unpacked.size() << " bytes\n";
        } else {
            std::cerr << "Failed to write DNG: " << filename << "\n";
        }
        return ok;
    }

    // --- JPEG path: single-plane MJPEG bitstream, write directly. ---
    // The Pi ISP hardware-encodes the JPEG; the buffer is a complete JPEG
    // file. No NV12->RGB conversion, no software encode — just mmap + write.
    if (config_.format == OutputFormat::JPEG) {
        const auto &plane = planes[0];
        void *base = mmap(nullptr, plane.offset + plane.length, PROT_READ,
                          MAP_SHARED, plane.fd.get(), 0);
        if (base == MAP_FAILED) {
            std::cerr << "mmap JPEG failed: " << strerror(errno) << "\n";
            return false;
        }
        auto *data = static_cast<uint8_t *>(base) + plane.offset;
        // MJPEG buffers may be padded; use the JPEG end marker (FFD9) to
        // find the real end if the plane length exceeds it.
        size_t writeLen = plane.length;
        for (size_t i = 0; i + 1 < plane.length; ++i) {
            if (data[i] == 0xFF && data[i + 1] == 0xD9) {
                writeLen = i + 2;
                break;
            }
        }
        bool ok = writeJpeg(data, writeLen, filename);
        munmap(base, plane.offset + plane.length);
        if (ok) {
            std::cout << "Saved JPEG: " << filename << " (" << w << "x" << h
                      << ") " << writeLen << " bytes\n";
        } else {
            std::cerr << "Failed to write JPEG: " << filename << "\n";
        }
        return ok;
    }

    // --- NV12 path: 2 planes, convert to RGB then encode. ---
    if (planes.size() < 2) {
        std::cerr << "Expected >=2 planes (NV12), got " << planes.size() << "\n";
        return false;
    }

    const auto &yPlane = planes[0];
    const auto &uvPlane = planes[1];
    uint32_t stride = sc.stride;

    // Map each plane through its own dmabuf fd. libcamera backends may put
    // the two NV12 planes in the same dmabuf (Pi VC4) or in separate ones;
    // mapping per-fd is correct in both cases.
    auto mapPlane = [](const libcamera::FrameBuffer::Plane &pl, const char *what)
        -> std::pair<uint8_t *, void *> {
        void *base = mmap(nullptr, pl.offset + pl.length, PROT_READ,
                          MAP_SHARED, pl.fd.get(), 0);
        if (base == MAP_FAILED) {
            std::cerr << "mmap " << what << " failed: " << strerror(errno) << "\n";
            return {nullptr, nullptr};
        }
        return {static_cast<uint8_t *>(base) + pl.offset, base};
    };

    auto [yMap, yBase] = mapPlane(yPlane, "Y");
    if (!yMap) return false;
    auto [uvMap, uvBase] = mapPlane(uvPlane, "UV");
    if (!uvMap) {
        munmap(yBase, yPlane.offset + yPlane.length);
        return false;
    }

    bool ok = true;
    if (config_.format == OutputFormat::RAW_NV12) {
        size_t ySize = static_cast<size_t>(stride) * h;
        size_t uvSize = static_cast<size_t>(stride) * (h / 2);
        ok = writeRaw(yMap, ySize, uvMap, uvSize, filename);
        if (ok) {
            std::cout << "Saved RAW: " << filename
                      << " (" << w << "x" << h << ")\n";
        }
    } else {
        auto rgb = nv12ToRgb(yMap, uvMap, w, h, stride);
        if (rgb.empty()) {
            ok = false;
        } else if (config_.format == OutputFormat::PNG) {
            ok = writePng(filename.c_str(), rgb.data(), w, h, config_.pngLevel);
            if (ok)
                std::cout << "Saved PNG: " << filename
                          << " (" << w << "x" << h << ")\n";
            else
                std::cerr << "Failed to write PNG: " << filename << "\n";
        } else {
            ok = writePpm(rgb.data(), rgb.size(), w, h, filename);
            if (ok)
                std::cout << "Saved PPM: " << filename
                          << " (" << w << "x" << h << ")"
                          << " " << rgb.size() << " bytes\n";
            else
                std::cerr << "Failed to write PPM: " << filename << "\n";
        }
    }

    munmap(uvBase, uvPlane.offset + uvPlane.length);
    munmap(yBase, yPlane.offset + yPlane.length);
    return ok;
}

}
