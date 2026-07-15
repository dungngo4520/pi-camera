#include "camera.h"
#include "image.h"
#include "output.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <map>
#include <unistd.h>
#include <sys/mman.h>

#include <libcamera/control_ids.h>
#include <libcamera/property_ids.h>
#include <libcamera/formats.h>

using namespace libcamera;
using namespace std::chrono_literals;

namespace picamera {

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

    // Queue all available buffers so AE/AWB can run continuously and converge.
    // We discard the first `warmupFrames` completions, re-queuing their buffers,
    // then save the next completed frame.
    const uint32_t warmup = config_.warmupFrames;
    uint32_t completed = 0;
    bool done = false;
    bool saved = false;

    cam_->requestCompleted.connect(this, [&](Request *r) {
        if (r->status() != Request::RequestComplete) {
            std::cerr << "Request status: " << r->status() << "\n";
            r->reuse(Request::ReuseBuffers);
            cam_->queueRequest(r);
            return;
        }
        ++completed;
        if (completed <= warmup) {
            // Warmup frame — discard and re-queue so AE/AWB keeps converging.
            r->reuse(Request::ReuseBuffers);
            cam_->queueRequest(r);
            return;
        }
        // Converged frame — save it.
        saved = saveFrame(r, filename);
        done = true;
    });

    // Initial queue: one request per buffer, with controls applied to the first.
    std::vector<std::unique_ptr<Request>> reqs;
    reqs.reserve(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto req = cam_->createRequest();
        if (!req) {
            std::cerr << "Failed to create Request\n";
            cam_->requestCompleted.disconnect();
            cam_->stop();
            return false;
        }
        req->addBuffer(stream_, buffers[i].get());
        if (i == 0) applyControls(req.get(), config_);
        if (cam_->queueRequest(req.get())) {
            std::cerr << "Failed to queue Request\n";
            cam_->requestCompleted.disconnect();
            cam_->stop();
            return false;
        }
        reqs.push_back(std::move(req));
    }

    auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!done) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "Capture timed out (completed " << completed
                      << "/" << (warmup + 1) << " frames)\n";
            cam_->requestCompleted.disconnect();
            cam_->stop();
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }

    cam_->requestCompleted.disconnect();
    cam_->stop();

    if (!saved) std::cerr << "Failed to save frame\n";
    return saved;
}

bool CameraApp::timelapse(int intervalSec, int count, const std::string &pattern) {
    bool infinite = (count == 0);

    for (int i = 0; infinite || i < count; ++i) {
        char buf[512];
        auto seqPos = pattern.find("%04d");
        if (seqPos != std::string::npos) {
            snprintf(buf, sizeof(buf), pattern.c_str(), i);
        } else {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm = *std::localtime(&t);
            strftime(buf, sizeof(buf), pattern.c_str(), &tm);
        }

        std::cout << "[" << (i + 1) << (infinite ? "/inf" : "/" + std::to_string(count))
                  << "] " << buf << "\n";

        if (!capture(buf)) {
            std::cerr << "Capture failed at shot " << i << "\n";
            return false;
        }

        if (infinite || i < count - 1) {
            std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
        }
    }
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
    for (auto &[id, val] : props) {
        std::cout << "  prop:" << id << ": " << val.toString() << "\n";
    }
}

void CameraApp::shutdown() {
    if (cam_) {
        cam_->stop();
        allocator_.reset();
        cam_->release();
        cam_.reset();
    }
    cm_.reset();
}

void CameraApp::applyControls(Request *req, const CameraConfig &cfg) {
    auto &ctrls = req->controls();

    if (!cfg.aeEnable) {
        ctrls.set(controls::AeEnable, false);
        ctrls.set(controls::ExposureTime, cfg.exposureTime);
    }
    if (cfg.analogueGain > 0.0f) {
        ctrls.set(controls::AnalogueGain, cfg.analogueGain);
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
    size_t bufLen = uvPlane.offset + uvPlane.length;
    uint32_t w = sc.size.width;
    uint32_t h = sc.size.height;
    uint32_t stride = sc.stride;

    auto *map = static_cast<uint8_t *>(
        mmap(nullptr, bufLen, PROT_READ, MAP_SHARED, yPlane.fd.get(), 0));
    if (map == MAP_FAILED) {
        std::cerr << "mmap failed: " << strerror(errno) << "\n";
        return false;
    }

    auto *yMap = map + yPlane.offset;
    auto *uvMap = map + uvPlane.offset;
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

    munmap(map, bufLen);
    return ok;
}

}
