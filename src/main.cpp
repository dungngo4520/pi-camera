#include "camera.h"
#include "cli.h"
#include "hardware_config.h"
#include "preview.h"
#include "safe_path.h"
#include "timelapse.h"

#include <cctype>
#include <filesystem>
#include <iostream>
#include <new>
#include <system_error>

namespace {

// Must match the path in config/systemd/picamera.service ExecStartPre.
constexpr const char *kDefaultCaptureDir = "/home/pi/captures";

picamera::PreviewConfig defaultPreviewConfig() {
  using namespace picamera;
  PreviewConfig pcfg;
  pcfg.enableBattery = true;
  pcfg.batteryCfg.pgaGain = 0x0000; // ±6.144V for direct LiPo
  // Appliance mode runs as a systemd service with no CWD guarantee.
  pcfg.captureDir = kDefaultCaptureDir;

  HardwareConfig hwCfg = loadHardwareConfig(kDefaultConfigPath);
  if (hwCfg.loaded) {
    std::cout << "Loaded hardware config from " << kDefaultConfigPath << "\n";
    pcfg.displayCfg.spiDevice = hwCfg.spiDevice;
    pcfg.displayCfg.rotation = hwCfg.displayRotation;
    pcfg.enableBattery = hwCfg.enableBattery;
    pcfg.batteryCfg.i2cDevice = hwCfg.batteryI2cDevice;
    pcfg.batteryCfg.i2cAddress = hwCfg.batteryI2cAddress;
    if (!hwCfg.captureDir.empty())
      pcfg.captureDir = hwCfg.captureDir;
    if (!hwCfg.capturePrefix.empty())
      pcfg.capturePrefix = hwCfg.capturePrefix;
    if (hwCfg.previewWidth > 0)
      pcfg.previewWidth = hwCfg.previewWidth;
    if (hwCfg.previewHeight > 0)
      pcfg.previewHeight = hwCfg.previewHeight;
    if (hwCfg.maxFps > 0)
      pcfg.maxFps = hwCfg.maxFps;
    if (hwCfg.captureWidth > 0)
      pcfg.captureWidth = hwCfg.captureWidth;
    if (hwCfg.captureHeight > 0)
      pcfg.captureHeight = hwCfg.captureHeight;
    if (hwCfg.wifiEnabled)
      pcfg.wifiEnabled = true;
  }

  return pcfg;
}

} // namespace

int main(int argc, char **argv) {
  using namespace picamera;

  try {
    // Appliance mode: no arguments → launch preview with defaults.
    if (argc < 2) {
      PreviewConfig pcfg = defaultPreviewConfig();
      return runPreview(pcfg) ? 0 : 1;
    }

    CliOptions opts;
    CameraConfig cfg;
    if (!parseArgs(argc, argv, opts, cfg)) {
      return 1;
    }

    // Preview mode has its own camera lifecycle.
    if (opts.mode == "preview") {
      PreviewConfig pcfg = makePreviewConfig(opts, cfg);
      return runPreview(pcfg) ? 0 : 1;
    }

    CameraApp app;
    if (!app.init())
      return 1;

    if (opts.mode == "list-controls") {
      app.listControls();
      return 0;
    }

    if (!app.configure(cfg)) {
      app.shutdown();
      return 1;
    }

    bool ok = false;
    if (opts.mode == "capture") {
      // Derive default filename from the selected format.
      std::string defaultFile = opts.captureFile;
      if (defaultFile.empty()) {
        defaultFile =
            std::string("capture.") + std::string(extensionFor(cfg.format));
      } else {
        // Warn if the user-supplied extension doesn't match --format.
        auto ext = extensionFor(cfg.format);
        auto dot = defaultFile.find_last_of('.');
        if (dot != std::string::npos) {
          std::string userExt = defaultFile.substr(dot + 1);
          for (auto &c : userExt)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          if (userExt != ext) {
            std::cerr << "Warning: filename extension does not match --format ("
                      << ext << "). File will contain " << ext << " data.\n";
          }
        } else {
          std::cerr << "Warning: filename has no extension, expected ." << ext
                    << " (--format)\n";
        }
      }
      // Prepend capture directory if set (canonicalized by parseArgs).
      std::string fullPath = defaultFile;
      if (!opts.captureDir.empty() && opts.captureDir != ".") {
        fullPath =
            (std::filesystem::path(opts.captureDir) / defaultFile).string();
        // Prevents symlinked subdirectories from escaping.
        if (!isFilePathInsideDir(fullPath, opts.captureDir)) {
          std::cerr << "Capture path escapes capture directory: " << fullPath
                    << "\n";
          app.shutdown();
          return 1;
        }
        // Create the capture directory if it doesn't exist.
        std::error_code ec;
        if (!std::filesystem::exists(opts.captureDir, ec) &&
            !std::filesystem::create_directories(opts.captureDir, ec)) {
          std::cerr << "Capture directory does not exist and could not"
                    << " be created: " << opts.captureDir << " ("
                    << ec.message() << ")\n";
          app.shutdown();
          return 1;
        }
        // Create parent directories for subdirectory paths (e.g.
        // --capture sub/photo.jpg) so safeFileOpen doesn't fail.
        auto lastSlash = fullPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
          std::string parentDir = fullPath.substr(0, lastSlash);
          if (!std::filesystem::exists(parentDir, ec) &&
              !std::filesystem::create_directories(parentDir, ec)) {
            std::cerr << "Cannot create capture subdirectory: " << parentDir
                      << " (" << ec.message() << ")\n";
            app.shutdown();
            return 1;
          }
        }
      }
      if (!cfg.bracketEv.empty()) {
        ok = app.captureBracket(fullPath);
      } else {
        std::string actualPath;
        ok = app.capture(fullPath, &actualPath);
        if (ok && !actualPath.empty() && actualPath != fullPath) {
          std::cout << "Saved: " << actualPath << " (renamed from " << fullPath
                    << ")\n";
        } else if (ok) {
          std::cout << "Saved: " << fullPath << "\n";
        }
      }
    } else if (opts.mode == "timelapse") {
      // Pass capture directory separately from the pattern so
      // formatTimelapseName can validate the pattern as relative.
      ok = runTimelapse(app, opts.timelapseInterval, opts.timelapseCount,
                        opts.outputPattern, opts.captureDir);
    }

    app.shutdown();
    return ok ? 0 : 1;
  } catch (const std::bad_alloc &) {
    std::cerr << "picamera: out of memory (512MB Pi Zero 2 W may need swap)\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "picamera: " << e.what() << "\n";
    return 1;
  } catch (...) {
    // Non-std::exception type (e.g., thrown by C library).
    try {
      std::rethrow_exception(std::current_exception());
    } catch (const std::exception &e) {
      std::cerr << "picamera: " << e.what() << "\n";
    } catch (...) {
      std::cerr << "picamera: unknown fatal error (non-std::exception)\n";
    }
    return 1;
  }
}
