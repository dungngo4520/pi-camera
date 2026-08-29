#include "timelapse.h"

#include "camera.h"
#include "stop_flag.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace picamera {

bool runTimelapse(CameraApp &app, int intervalSec, int count,
                  const std::string &pattern) {
    bool infinite = (count == 0);

    StopFlag stop;
    stop.install();

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
        std::cout << "[" << (i + 1) << (infinite ? "/inf" : "/" + std::to_string(count))
                  << "] " << filename << "\n";

        if (!app.capture(filename)) {
            std::cerr << "Capture failed at shot " << i << "\n";
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
