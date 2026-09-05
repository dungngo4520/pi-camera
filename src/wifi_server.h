#pragma once

#include "camera_mode.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace picamera {

// --- Pure-logic helpers (unit-testable, no socket dependency) ---

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

// Escape a string for use inside a JSON string value.
std::string jsonEscape(std::string_view s);

// Parse a raw HTTP request buffer. Extracts method, path, and body (for POST).
// Returns true if at least a valid request line was found.
bool parseHttpRequest(const std::string &raw, HttpRequest &out);

std::string settingsToJson(const CameraSettings &s);
std::string statusToJson(int batteryPercent, const CameraSettings &s,
                         uint32_t captureCount);
std::string fileListingHtml(const std::vector<std::string> &files);

// Only updates fields present in the JSON; others are left unchanged.
void applySettingsJson(const std::string &json, CameraSettings &s);

std::string urlDecode(std::string_view s);

// Extract the filename from a "/file/<name>" path, or "" if not a file path.
std::string extractFileName(const std::string &path);

// Pure-logic form of the path-traversal guard in WifiServer::handleConnection();
// rejects '/' and ".." to prevent serving files outside captureDir.
inline bool isSafeFileName(const std::string &filename) {
    return filename.find('/') == std::string::npos &&
           filename.find("..") == std::string::npos;
}

// --- WifiServer: minimal HTTP server for remote control / image transfer ---
// Runs in a background thread; handles one connection at a time.
// Uses raw POSIX sockets — no external dependencies.
class WifiServer {
public:
    WifiServer();
    ~WifiServer();
    WifiServer(const WifiServer &) = delete;
    WifiServer &operator=(const WifiServer &) = delete;

    bool start(int port, const std::string &captureDir,
               CameraSettings &settings, std::mutex &settingsMtx,
               std::atomic<bool> &captureRequest,
               const std::atomic<int> &batteryPercent,
               const std::atomic<uint32_t> &captureCount);

    void stop();

    bool isRunning() const { return running_.load(std::memory_order_acquire); }

private:
    void serverLoop();
    void handleConnection(int clientFd);

    std::atomic<bool> running_{false};
    std::atomic<bool> stopFlag_{false};
    int port_ = 0;
    int listenFd_ = -1;
    std::string captureDir_;
    std::thread thread_;
    CameraSettings *settings_ = nullptr;
    std::mutex *settingsMtx_ = nullptr;
    std::atomic<bool> *captureRequest_ = nullptr;
    const std::atomic<int> *batteryPercent_ = nullptr;
    const std::atomic<uint32_t> *captureCount_ = nullptr;
};

} // namespace picamera
