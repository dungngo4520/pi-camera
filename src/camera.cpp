#include "camera.h"
#include "image.h"
#include "output.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <csignal>
#include <stdexcept>
#include <algorithm>
#include <map>
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
} // namespace

static const std::map<std::string, controls::AwbModeEnum> awbMap = {
    {"auto",         controls::AwbAuto},
    {"incandescent", controls::AwbIncandescent},
    {"tungsten",     controls::AwbTungsten},
    {"fluorescent",  controls::AwbFluorescent},
    {"indoor",       controls::AwbIndoor},
    {"daylight",     controls::AwbDaylight},
    {"cloudy",       controls::AwbCloudy},
};

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

    auto roles = {StreamRole::StillCapture};
    auto camCfg = cam_->generateConfiguration(roles);
    if (!camCfg) {
        std::cerr << "generateConfiguration failed\n";
        return false;
    }

    auto &sc = camCfg->at(0);
    sc.size.width = cfg.width;
    sc.size.height = cfg.height;
    sc.pixelFormat = formats::NV12;
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

    auto &buffers = allocator_->buffers(stream_);
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

// Build a timelapse filename from a user-supplied pattern.
//
// If the pattern contains a printf-style integer conversion (e.g. "%04d"),
// the sequence index `i` is substituted. Otherwise the pattern is treated as
// a strftime template and expanded with the current local time.
//
// Security: the printf path passes the user pattern to snprintf, so it must
// contain ONLY integer conversions (`%d`/`%i`/`%u`/`%x`/`%X`/`%o`) and `%%`.
// Any other `%` specifier (e.g. `%s`, `%n`, `%p`, or a strftime-style `%Y`)
// mixed with an integer conversion is rejected — otherwise `%s`/`%n` would
// read garbage off the stack. A pure strftime pattern (no integer conversion)
// is safe because strftime only reads the struct tm we pass it.
std::string formatTimelapseName(const std::string &pattern, int i) {
    auto isPrintfInt = [](char conv) {
        return conv == 'd' || conv == 'i' || conv == 'u' ||
               conv == 'x' || conv == 'X' || conv == 'o';
    };

    bool hasIntConv = false;
    bool hasOtherConv = false;  // any %-specifier that isn't an int conv or %%

    for (size_t p = 0; p < pattern.size(); ++p) {
        if (pattern[p] != '%') continue;
        if (p + 1 >= pattern.size()) {
            throw std::invalid_argument("output pattern ends with stray '%'");
        }
        char next = pattern[p + 1];
        if (next == '%') { ++p; continue; }            // literal %
        // Skip printf flags/width/precision: [-+ 0#]*[0-9]*.?[0-9]*
        size_t q = p + 1;
        while (q < pattern.size() &&
               (pattern[q] == '-' || pattern[q] == '+' || pattern[q] == ' ' ||
                pattern[q] == '0' || pattern[q] == '#' || pattern[q] == '.' ||
                (pattern[q] >= '0' && pattern[q] <= '9'))) {
            ++q;
        }
        if (q >= pattern.size()) {
            throw std::invalid_argument("output pattern has incomplete '%...' specifier");
        }
        char conv = pattern[q];
        if (isPrintfInt(conv)) {
            hasIntConv = true;
        } else {
            hasOtherConv = true;
        }
        p = q;
    }

    if (hasIntConv && hasOtherConv) {
        throw std::invalid_argument(
            "output pattern mixes integer conversions with other '%...' specifiers; "
            "use either a printf %%d-style pattern or a strftime pattern, not both");
    }

    if (hasIntConv) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), pattern.c_str(), i);
        if (n < 0) throw std::runtime_error("snprintf failed on output pattern");
        return std::string(buf, std::min<int>(n, static_cast<int>(sizeof(buf) - 1)));
    }

    char buf[512];
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);
    strftime(buf, sizeof(buf), pattern.c_str(), &tm);
    return std::string(buf);
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

    auto &controls = cam_->controls();
    std::cout << "=== Controls ===\n";
    for (auto &[id, info] : controls) {
        std::cout << "  " << id->name() << ": " << info.toString() << "\n";
    }

    auto &props = cam_->properties();
    std::cout << "\n=== Properties ===\n";
    const auto *propIdMap = props.idMap();
    for (auto &[id, val] : props) {
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
        auto it = awbMap.find(cfg.awbMode);
        if (it != awbMap.end()) {
            ctrls.set(controls::AwbMode, it->second);
        }
    }
}

bool CameraApp::saveFrame(const Request *req, const std::string &filename) {
    auto &buffers = req->buffers();
    if (buffers.empty()) return false;

    auto &[stream, buffer] = *buffers.begin();
    auto planes = buffer->planes();
    if (planes.size() < 2) {
        std::cerr << "Expected >=2 planes (NV12), got " << planes.size() << "\n";
        return false;
    }

    auto &sc = stream_->configuration();
    auto &yPlane = planes[0];
    auto &uvPlane = planes[1];
    uint32_t w = sc.size.width;
    uint32_t h = sc.size.height;
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
            ok = writePng(filename.c_str(), rgb.data(), w, h);
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
