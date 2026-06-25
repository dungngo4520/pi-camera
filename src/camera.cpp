#include "camera.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <thread>
#include <iomanip>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

#include <png.h>

#include <libcamera/control_ids.h>
#include <libcamera/property_ids.h>
#include <libcamera/formats.h>

using namespace libcamera;

/// Maps human-readable AWB mode names to libcamera AWB control enums.
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

/// Initialise the libcamera CameraManager and open the first available camera.
///
/// The CameraManager must be started before any camera operations. After
/// starting, we take the first camera from the list and acquire exclusive
/// ownership of it.
///
/// @return true on success, false if no camera or acquisition fails.
bool CameraApp::init() {
    cm_ = std::make_shared<CameraManager>();
    if (cm_->start()) {
        std::cerr << "ERROR: CameraManager::start() failed\n";
        return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
        std::cerr << "ERROR: No cameras detected by CameraManager\n";
        return false;
    }

    cam_ = cameras[0];
    std::cout << "Camera: " << cam_->id() << "\n";

    if (cam_->acquire()) {
        std::cerr << "ERROR: Failed to acquire camera\n";
        return false;
    }

    return true;
}

/// Generate a stream configuration, validate it against hardware limits,
/// configure the camera, and allocate DMA frame buffers.
///
/// The configuration pipeline:
///   1. Generate a default StillCapture configuration
///   2. Override resolution and format (RGB888 for simpler processing)
///   3. Validate — hardware may adjust resolution/format to nearest supported
///   4. Configure the camera with the validated config
///   5. Allocate buffers from the FrameBufferAllocator
///
/// @param cfg  Desired resolution and control settings.
void CameraApp::configure(const CameraConfig &cfg) {
    config_ = cfg;

    // Generate a default configuration for a StillCapture role.
    // This produces a single StreamConfiguration with sensible defaults.
    auto roles = {StreamRole::StillCapture};
    auto camCfg = cam_->generateConfiguration(roles);
    if (!camCfg) {
        std::cerr << "ERROR: generateConfiguration returned null\n";
        return;
    }

    // Override the stream's resolution, pixel format, and buffer count.
    auto &sc = camCfg->at(0);
    sc.size.width = cfg.width;
    sc.size.height = cfg.height;
    // NV12 (YUV 4:2:0, 12 bpp) uses half the memory of RGB888 (24 bpp),
    // which is critical for full-res capture on the Pi Zero 2's limited CMA.
    sc.pixelFormat = formats::NV12;
    sc.bufferCount = 2;                // 2 buffers — enough for stills, saves memory for full-res

    // Validate the configuration against what the camera hardware supports.
    // This may adjust resolution to the nearest valid size and may change
    // the pixel format if RGB888 is not supported at the requested size.
    auto status = camCfg->validate();
    if (status == CameraConfiguration::Invalid) {
        std::cerr << "ERROR: Camera configuration is invalid\n";
        return;
    }

    if (cam_->configure(camCfg.get())) {
        std::cerr << "ERROR: cam->configure() failed\n";
        return;
    }

    stream_ = sc.stream();  // Save the configured stream pointer

    // Allocate dma-buf frame buffers for the configured stream.
    // These buffers are shared between the ISP and userspace.
    allocator_ = std::make_unique<FrameBufferAllocator>(cam_);
    if (allocator_->allocate(stream_) < 0) {
        std::cerr << "ERROR: Buffer allocation failed\n";
        return;
    }

    std::cout << "Configured: " << sc.size.width << "x" << sc.size.height
              << " stride:" << sc.stride << "\n";
}

/// Capture a single frame by creating and queuing a Request.
///
/// The capture flow:
///   1. Create a Request targeting the still stream
///   2. Attach one of the pre-allocated buffers to the Request
///   3. Apply user controls (exposure, gain, AWB) to the Request
///   4. Connect the requestCompleted signal to our save handler
///   5. Queue the Request — this triggers the camera to start exposure
///   6. Wait in a poll loop until the request completes or times out (15s)
///   7. Disconnect the signal and return
///
/// @param filename  Output file path for the PPM image.
/// @return true on successful capture and save.
bool CameraApp::capture(const std::string &filename) {
    if (!cam_ || !allocator_) {
        std::cerr << "ERROR: Camera not initialized or configured\n";
        return false;
    }

    // Get the pre-allocated buffers for this stream.
    auto &buffers = allocator_->buffers(stream_);
    if (buffers.empty()) return false;

    // Create a capture Request and attach a buffer.
    auto req = cam_->createRequest();
    if (!req) {
        std::cerr << "ERROR: Failed to create Request\n";
        return false;
    }

    req->addBuffer(stream_, buffers[0].get());
    applyControls(req.get(), config_);

    // Start the camera (must be running before queueing requests).
    if (cam_->start()) {
        std::cerr << "ERROR: Failed to start camera\n";
        return false;
    }

    // Wait for the request to complete via the signal.
    // We use a done flag and poll because we don't have an event loop.
    bool done = false;
    bool saved = false;
    cam_->requestCompleted.connect(this, [this, &done, &saved, &filename](Request *r) {
        if (r->status() == Request::RequestComplete) {
            saved = saveFrame(r, filename);
        } else {
            std::cerr << "ERROR: Request status = " << r->status() << "\n";
        }
        done = true;
    });

    if (cam_->queueRequest(req.get())) {
        std::cerr << "ERROR: Failed to queue Request\n";
        cam_->stop();
        return false;
    }

    // Poll with 5ms sleep until done or timeout (15s).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done) {
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << "ERROR: Capture timed out after 15s\n";
            cam_->requestCompleted.disconnect();
            cam_->stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    cam_->requestCompleted.disconnect();
    cam_->stop();
    if (!saved) {
        std::cerr << "ERROR: Failed to save frame\n";
    }
    return saved;
}

/// Run a timelapse sequence: capture, sleep, repeat.
///
/// @param intervalSec  Gap between captures in seconds.
/// @param count        Number of captures. 0 means infinite (run until killed).
/// @param pattern      Filename pattern. Uses %04d for sequence numbering,
///                     or strftime specifiers (%Y%m%d_%H%M%S) for timestamps.
/// @return true if all captures succeeded.
bool CameraApp::timelapse(int intervalSec, int count, const std::string &pattern) {
    bool infinite = (count == 0);

    for (int i = 0; infinite || i < count; ++i) {
        // Generate filename: prefer %04d sequence, fall back to strftime.
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
            std::cerr << "ERROR: Capture failed at shot " << i << "\n";
            return false;
        }

        // Sleep between shots (skip after the last if finite).
        if (infinite || i < count - 1) {
            std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
        }
    }
    return true;
}

/// Enumerate and print all camera controls and properties.
///
/// Controls include ExposureTime, AnalogueGain, AwbMode, etc., each
/// showing the minimum, maximum, and default values supported by the sensor.
/// Properties include the sensor model string, resolution limits, etc.
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

/// Release the camera and shut down the camera manager.
///
/// Order matters:
///   1. Stop the camera if it's streaming
///   2. Free the allocator (releases DMA buffers)
///   3. Release the camera (allows other processes to use it)
///   4. Reset the CameraManager (calls stop() internally)
void CameraApp::shutdown() {
    if (cam_) {
        cam_->stop();
        allocator_.reset();
        cam_->release();
        cam_.reset();
    }
    cm_.reset();  // Calls cm_->stop() in CameraManager destructor
}

/// Write the chosen controls into a Request's ControlList.
///
/// Controls are applied on a per-request basis. The camera pipeline
/// configures the sensor according to these values before exposure.
///
/// Auto-exposure and auto-white-balance can be disabled individually.
/// When AE is disabled, ExposureTime must be set explicitly.
/// When AWB is disabled, AwbMode is ignored (the sensor uses its
/// current manual gains, which must be set through the sensor's
/// V4L2 controls separately, if needed).
///
/// @param req  The Request whose controls to modify.
/// @param cfg  The user-specified configuration.
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

static bool writePng(const char *filename, const uint8_t *rgb, unsigned int w, unsigned int h) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); return false; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(h);
    for (unsigned int y = 0; y < h; ++y)
        rows[y] = const_cast<png_bytep>(rgb + y * w * 3);

    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

static bool writePpm(const uint8_t *rgb, size_t rgbSize, unsigned int w, unsigned int h, const std::string &filename) {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(rgb), static_cast<std::streamsize>(rgbSize));
    return true;
}

static std::vector<uint8_t> nv12ToRgb(const uint8_t *yMap, const uint8_t *uvMap,
                                       unsigned int w, unsigned int h, unsigned int stride) {
    auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    size_t rgbSize = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> rgb(rgbSize);

    for (unsigned int y = 0; y < h; y += 2) {
        for (unsigned int x = 0; x < w; x += 2) {
            int U = uvMap[(y / 2) * stride + (x / 2) * 2];
            int V = uvMap[(y / 2) * stride + (x / 2) * 2 + 1];
            int D = U - 128;
            int E = V - 128;
            int Ruv = 409 * E;
            int Guv = -100 * D - 208 * E;
            int Buv = 516 * D;

            for (unsigned int dy = 0; dy < 2 && y + dy < h; ++dy) {
                for (unsigned int dx = 0; dx < 2 && x + dx < w; ++dx) {
                    int C = yMap[(y + dy) * stride + (x + dx)] - 16;
                    int R = (298 * C + Ruv + 128) >> 8;
                    int G = (298 * C + Guv + 128) >> 8;
                    int B = (298 * C + Buv + 128) >> 8;
                    size_t off = (static_cast<size_t>(y + dy) * w + (x + dx)) * 3;
                    rgb[off + 0] = clamp(R);
                    rgb[off + 1] = clamp(G);
                    rgb[off + 2] = clamp(B);
                }
            }
        }
    }
    return rgb;
}

bool CameraApp::saveFrame(const Request *req, const std::string &filename) {
    auto &buffers = req->buffers();
    if (buffers.empty()) return false;

    auto &[stream, buffer] = *buffers.begin();
    auto planes = buffer->planes();
    if (planes.size() < 2) {
        std::cerr << "ERROR: Expected at least 2 planes (NV12), got " << planes.size() << "\n";
        return false;
    }

    auto &sc = stream_->configuration();
    auto &yPlane = planes[0];
    auto &uvPlane = planes[1];
    size_t bufLen = uvPlane.offset + uvPlane.length;
    unsigned int w = sc.size.width;
    unsigned int h = sc.size.height;
    unsigned int stride = sc.stride;

    auto *map = static_cast<uint8_t *>(
        mmap(nullptr, bufLen, PROT_READ, MAP_SHARED, yPlane.fd.get(), 0));
    if (map == MAP_FAILED) {
        std::cerr << "ERROR: mmap buffer failed: " << strerror(errno) << "\n";
        return false;
    }

    auto *yMap = map + yPlane.offset;
    auto *uvMap = map + uvPlane.offset;
    bool ok = true;

    if (config_.format == OutputFormat::RAW_NV12) {
        std::ofstream out(filename, std::ios::binary);
        if (!out) {
            std::cerr << "ERROR: Cannot write output file: " << filename << "\n";
            ok = false;
        } else {
            size_t ySize = static_cast<size_t>(stride) * h;
            size_t uvSize = static_cast<size_t>(stride) * (h / 2);
            out.write(reinterpret_cast<const char *>(yMap), ySize);
            out.write(reinterpret_cast<const char *>(uvMap), uvSize);
            std::cout << "Saved RAW: " << filename
                      << " (" << w << "x" << h << ")"
                      << "\n";
        }
    } else {
        auto rgb = nv12ToRgb(yMap, uvMap, w, h, stride);
        if (rgb.empty()) {
            ok = false;
        } else if (config_.format == OutputFormat::PNG) {
            ok = writePng(filename.c_str(), rgb.data(), w, h);
            if (ok)
                std::cout << "Saved PNG: " << filename
                          << " (" << w << "x" << h << ")"
                          << "\n";
            else
                std::cerr << "ERROR: Failed to write PNG: " << filename << "\n";
        } else {
            ok = writePpm(rgb.data(), rgb.size(), w, h, filename);
            if (ok)
                std::cout << "Saved PPM: " << filename
                          << " (" << w << "x" << h << ")"
                          << " " << rgb.size() << " bytes\n";
            else
                std::cerr << "ERROR: Cannot write output file: " << filename << "\n";
        }
    }

    munmap(map, bufLen);
    return ok;
}
