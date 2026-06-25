#pragma once

#include <libcamera/libcamera.h>

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <map>

enum class OutputFormat {
    PPM,
    RAW_NV12,
    PNG,
};

/// User-facing configuration that maps to libcamera controls.
/// Defaults: full HQ camera resolution (4056x3040), auto exposure, auto AWB.
struct CameraConfig {
    uint32_t width = 4056;          ///< Image width in pixels
    uint32_t height = 3040;         ///< Image height in pixels
    uint64_t exposureTime = 0;      ///< Shutter speed in microseconds (0 = auto)
    float analogueGain = 0;         ///< Sensor gain, typical range 1.0-8.0 (0 = auto)
    float digitalGain = 0;          ///< ISP digital gain (0 = auto)
    std::string awbMode = "auto";   ///< White balance preset (auto, daylight, cloudy, ...)
    bool aeEnable = true;           ///< Enable auto exposure
    bool awbEnable = true;          ///< Enable auto white balance
    bool listControls = false;
    OutputFormat format = OutputFormat::PPM;
};

/// Wraps libcamera lifecycle and capture operations.
///
/// Typical usage:
/// @code
///   CameraApp app;
///   app.init();
///   app.configure({});
///   app.capture("photo.ppm");
///   app.shutdown();
/// @endcode
///
/// The camera pipeline:
///   1. CameraManager::start()  — discover cameras
///   2. Camera::acquire()       — take ownership
///   3. Camera::configure()     — set resolution, format
///   4. FrameBufferAllocator    — allocate DMA buffers
///   5. Camera::queueRequest()  — submit capture
///   6. requestCompleted signal — frame ready
///   7. mmap dma-buf → write PPM
class CameraApp {
public:
    CameraApp();
    ~CameraApp();

    /// Initialise the camera manager and open the first available camera.
    /// Prints the camera model string on success.
    /// @return true if a camera was found and acquired.
    bool init();

    /// Apply a CameraConfig to the camera.
    /// Generates a stream configuration, validates it against hardware
    /// capabilities, configures the camera, and allocates frame buffers.
    /// @param cfg  Desired resolution, format, and control settings.
    void configure(const CameraConfig &cfg);

    /// Capture a single still frame and save as PPM (P6 RGB888).
    /// Blocks until the frame is ready or a 15-second timeout elapses.
    /// @param filename  Output path (*.ppm).
    /// @return true on success.
    bool capture(const std::string &filename);

    /// Run a timed sequence of captures.
    /// @param intervalSec  Seconds between shots.
    /// @param count        Number of shots (0 = infinite, until Ctrl+C).
    /// @param pattern      Output filename pattern.
    ///   Supports strftime (%Y, %m, %d, %H, %M, %S) and %04d for sequence.
    /// @return true if all captures succeeded.
    bool timelapse(int intervalSec, int count, const std::string &pattern);

    /// Print all camera controls (with valid ranges) and sensor properties.
    void listControls();

    /// Release camera and stop the camera manager.
    void shutdown();

private:
    /// Apply exposure, gain, and white balance controls to a request.
    void applyControls(libcamera::Request *req, const CameraConfig &cfg);

    /// Read frame data from dma-buf planes via mmap and write as PPM.
    bool saveFrame(const libcamera::Request *req, const std::string &filename);

    std::shared_ptr<libcamera::CameraManager> cm_;   ///< Camera discovery and management
    std::shared_ptr<libcamera::Camera> cam_;          ///< The opened camera device
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;  ///< DMA buffer pool
    libcamera::Stream *stream_ = nullptr;             ///< The configured stream (still)
    CameraConfig config_;                             ///< Active camera configuration
};
