#pragma once

// Pure-logic helpers for the video recording pipeline. These map the
// user-facing video settings (resolution, frame rate, codec, bitrate) to
// the concrete values the recording loop needs: the IMX477 sensor mode to
// reconfigure, the JPEG quality for MJPEG encoding, the output file
// extension, and the per-frame interval for FPS throttling. All functions
// are unit-testable on x86 (no camera dependency).

#include "camera_mode.h"

#include <chrono>
#include <cstdint>

namespace picamera {

// IMX477 sensor modes (width, height, max frame rate). The sensor exposes
// a fixed set of modes; the recording pipeline picks the closest one for
// the requested video resolution and lets the ISP scale to the target.
struct SensorModeInfo {
  uint32_t width;
  uint32_t height;
  int maxFps;
};

// Number of IMX477 sensor modes (excluding Auto).
constexpr int kSensorModeCount = 4;

// IMX477 sensor mode table (index 0 = 1332x990, 1 = 2028x1080,
// 2 = 2028x1520, 3 = 4056x3040).
const SensorModeInfo *sensorModeTable();

// Map a requested video resolution to the closest IMX477 sensor mode that
// can produce it (i.e. the smallest mode whose dimensions are >= the
// request). Returns the SensorMode enum value. Used to reconfigure the
// capture stream at recording start so the output matches the selection.
SensorMode videoResolutionToSensorMode(VideoResolution r);

// Map a video bitrate (Mbps: 1/5/10/20) to a JPEG quality factor (1-100)
// for MJPEG encoding. Higher bitrate => higher quality. Returns the
// quality; for YUV raw the caller ignores this.
int videoBitrateToJpegQuality(int bitrateMbps);

// Compute the per-frame interval (microseconds) for the given FPS, for
// throttling the recording loop. Returns 0 for fps <= 0.
std::chrono::microseconds videoFrameInterval(int fps);

// Clamp a requested frame rate to the maximum supported by a sensor mode.
// Don't capture faster than the sensor mode allows.
int clampFpsToSensorMode(int fps, SensorMode mode);

// File extension (including the leading dot) for a video codec. H264
// falls back to MJPEG encoding (no HW H264 encoder on Pi Zero 2 W via
// libcamera), so it returns ".mjpeg" — JPEG frames are never written to a
// .h264 file (that would be an invalid H.264 bitstream).
const char *videoCodecExtension(VideoCodec c);

// Whether a codec produces MJPEG-style JPEG frames (true for MJPEG and
// H264-fallback; false for raw YUV).
bool videoCodecUsesJpeg(VideoCodec c);

} // namespace picamera
