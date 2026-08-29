#include "stream.h"

#include <iostream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <sys/mman.h>

#include <libcamera/formats.h>

using namespace libcamera;
using namespace std::chrono_literals;

namespace picamera {

CameraStream::CameraStream() = default;

CameraStream::~CameraStream() { shutdown(); }

bool CameraStream::init() {
    cm_ = std::make_shared<CameraManager>();
    if (cm_->start()) {
        std::cerr << "Stream: CameraManager::start() failed\n";
        cm_.reset();
        return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
        std::cerr << "Stream: No cameras found\n";
        cm_->stop();
        cm_.reset();
        return false;
    }

    cam_ = cameras[0];
    if (cam_->acquire()) {
        std::cerr << "Stream: Failed to acquire camera\n";
        cam_.reset();
        cm_->stop();
        cm_.reset();
        return false;
    }

    std::cout << "Stream: camera " << cam_->id() << " acquired\n";
    return true;
}

bool CameraStream::start(uint32_t width, uint32_t height) {
    if (!cam_) return false;

    // Configure with Viewfinder role for continuous streaming
    auto cfg = cam_->generateConfiguration({StreamRole::Viewfinder});
    if (!cfg) {
        std::cerr << "Stream: generateConfiguration failed\n";
        return false;
    }

    auto &sc = cfg->at(0);
    sc.size.width = width;
    sc.size.height = height;
    sc.pixelFormat = formats::NV12;
    sc.bufferCount = 4;

    auto status = cfg->validate();
    if (status == CameraConfiguration::Invalid) {
        std::cerr << "Stream: configuration invalid\n";
        return false;
    }

    if (cam_->configure(cfg.get())) {
        std::cerr << "Stream: cam->configure() failed\n";
        return false;
    }

    stream_ = sc.stream();
    width_ = sc.size.width;
    height_ = sc.size.height;
    stride_ = sc.stride;

    allocator_ = std::make_unique<FrameBufferAllocator>(cam_);
    if (allocator_->allocate(stream_) < 0) {
        std::cerr << "Stream: buffer allocation failed\n";
        allocator_.reset();
        stream_ = nullptr;
        return false;
    }

    std::cout << "Stream: configured " << width_ << "x" << height_
              << " stride:" << stride_ << "\n";

    // Start camera and queue all buffers
    if (cam_->start()) {
        std::cerr << "Stream: cam->start() failed\n";
        allocator_.reset();
        stream_ = nullptr;
        return false;
    }
    started_ = true;

    // Install request completion callback
    cam_->requestCompleted.connect(this, [this](Request *r) {
        if (r->status() != Request::RequestComplete) {
            r->reuse(Request::ReuseBuffers);
            cam_->queueRequest(r);
            return;
        }

        // Copy frame data from the completed buffer
        const auto &buffers = r->buffers();
        if (buffers.empty()) {
            r->reuse(Request::ReuseBuffers);
            cam_->queueRequest(r);
            return;
        }

        const auto &[stream, buffer] = *buffers.begin();
        auto planes = buffer->planes();

        if (planes.size() >= 2) {
            const auto &yPlane = planes[0];
            const auto &uvPlane = planes[1];

            // Map Y plane
            void *yBase = mmap(nullptr, yPlane.offset + yPlane.length,
                               PROT_READ, MAP_SHARED, yPlane.fd.get(), 0);
            void *uvBase = nullptr;
            if (yBase != MAP_FAILED) {
                uvBase = mmap(nullptr, uvPlane.offset + uvPlane.length,
                              PROT_READ, MAP_SHARED, uvPlane.fd.get(), 0);
            }

            if (yBase != MAP_FAILED && uvBase != MAP_FAILED) {
                size_t ySize = static_cast<size_t>(stride_) * height_;
                size_t uvSize = static_cast<size_t>(stride_) * (height_ / 2);
                yData_.resize(ySize);
                uvData_.resize(uvSize);
                std::memcpy(yData_.data(),
                            static_cast<uint8_t *>(yBase) + yPlane.offset, ySize);
                std::memcpy(uvData_.data(),
                            static_cast<uint8_t *>(uvBase) + uvPlane.offset, uvSize);
            }

            if (yBase != MAP_FAILED)
                munmap(yBase, yPlane.offset + yPlane.length);
            if (uvBase != MAP_FAILED && uvBase != nullptr)
                munmap(uvBase, uvPlane.offset + uvPlane.length);
        }

        // Re-queue the buffer for continuous capture
        r->reuse(Request::ReuseBuffers);
        cam_->queueRequest(r);

        // Notify waiting thread
        {
            std::lock_guard<std::mutex> lk(mtx_);
            frameReady_ = true;
        }
        cv_.notify_one();
    });

    // Create and queue initial requests
    const auto &buffers = allocator_->buffers(stream_);
    requests_.reserve(buffers.size());
    for (size_t i = 0; i < buffers.size(); ++i) {
        auto req = cam_->createRequest();
        if (!req) {
            std::cerr << "Stream: createRequest failed\n";
            break;
        }
        req->addBuffer(stream_, buffers[i].get());
        if (cam_->queueRequest(req.get())) {
            std::cerr << "Stream: queueRequest failed\n";
            break;
        }
        requests_.push_back(std::move(req));
    }

    return true;
}

StreamFrame CameraStream::grabFrame(int timeoutMs) {
    StreamFrame frame;
    if (!started_) return frame;

    std::unique_lock<std::mutex> lk(mtx_);
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                      [this] { return frameReady_; })) {
        return frame; // timeout
    }
    frameReady_ = false;

    frame.y = yData_.data();
    frame.uv = uvData_.data();
    frame.width = width_;
    frame.height = height_;
    frame.stride = stride_;
    return frame;
}

void CameraStream::stop() {
    if (!started_ || !cam_) return;

    cam_->requestCompleted.disconnect();

    // Stop camera (this waits for all queued requests to complete/cancel)
    cam_->stop();
    started_ = false;

    requests_.clear();
}

void CameraStream::shutdown() {
    stop();
    if (cam_) {
        allocator_.reset();
        cam_->release();
        cam_.reset();
    }
    if (cm_) {
        cm_->stop();
        cm_.reset();
    }
    stream_ = nullptr;
}

} // namespace picamera
