#include "timelapse.h"

#include "camera.h"
#include "safe_path.h"
#include "stop_flag.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace picamera {

bool runTimelapse(CameraApp &app, int intervalSec, int count,
                  const std::string &pattern,
                  const std::string &captureDir) {
    bool infinite = (count == 0);
    bool useDir = !captureDir.empty() && captureDir != ".";

    StopFlag stop;
    if (!stop.install()) {
        std::cerr << "Timelapse: failed to install signal handlers\n";
        return false;
    }

    for (int i = 0; infinite || i < count; ++i) {
        if (stop.stopRequested()) {
            std::cerr << "\nTimelapse interrupted by signal after " << i
                      << " shots\n";
            break;
        }

        std::string filename;
        try {
            filename = formatTimelapseName(pattern, i);
        } catch (const std::exception &e) {
            std::cerr << "Bad --output pattern: " << e.what() << "\n";
            return false;
        }

        // Join with capture directory after formatting (the pattern itself
        // must be a safe relative filename; the directory is prepended
        // separately and the full path is validated against the dir).
        std::string fullPath = filename;
        if (useDir) {
            fullPath = (std::filesystem::path(captureDir) / filename).string();
            if (!isFilePathInsideDir(fullPath, captureDir)) {
                std::cerr << "Timelapse path escapes capture directory: "
                          << fullPath << "\n";
                return false;
            }
        }

        // Guard against overlong full paths that would fail at open() time.
        if (fullPath.size() >= 4096) {
            std::cerr << "Timelapse: full path too long (>= 4096): "
                      << fullPath.substr(0, 80) << "...\n";
            return false;
        }

        // Create parent directories if the formatted name contains '/'
        // (e.g. strftime patterns like %Y/%m/%d/photo.jpg).
        auto lastSlash = fullPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            std::string parentDir = fullPath.substr(0, lastSlash);
            std::error_code ec;
            if (!std::filesystem::exists(parentDir, ec) &&
                !std::filesystem::create_directories(parentDir, ec)) {
                std::cerr << "Timelapse: cannot create directory " << parentDir
                          << ": " << ec.message() << "\n";
                return false;
            }
        }

        std::string shotLabel = infinite ? "inf" : std::to_string(count);
        std::cout << "[" << (i + 1) << "/" << shotLabel
                  << "] " << fullPath << "\n";

        if (!app.capture(fullPath)) {
            std::cerr << "Capture failed at shot " << (i + 1) << "\n";
            return false;
        }

        if ((infinite || i < count - 1) && !stop.stopRequested()) {
            // Sleep in small increments so a signal is noticed promptly.
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(intervalSec);
            while (std::chrono::steady_clock::now() < end) {
                if (stop.stopRequested()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    return true;
}

} // namespace picamera
