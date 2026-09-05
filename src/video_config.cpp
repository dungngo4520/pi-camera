#include "video_config.h"

#include <algorithm>

namespace picamera {

namespace {

// IMX477 sensor modes. The 2028x1080 mode is the closest to 1080p; the
// 4056x3040 mode is the closest to 4K (but capped at 10fps).
constexpr SensorModeInfo kSensorModes[kSensorModeCount] = {
    {1332, 990, 120},
    {2028, 1080, 50},
    {2028, 1520, 40},
    {4056, 3040, 10},
};

} // namespace

const SensorModeInfo *sensorModeTable() { return kSensorModes; }

SensorMode videoResolutionToSensorMode(VideoResolution r) {
  // Requested target dimensions.
  uint32_t targetW = 0;
  uint32_t targetH = 0;
  switch (r) {
  case VideoResolution::Res320x240:
    targetW = 320;
    targetH = 240;
    break;
  case VideoResolution::Res640x480:
    targetW = 640;
    targetH = 480;
    break;
  case VideoResolution::Res1280x720:
    targetW = 1280;
    targetH = 720;
    break;
  case VideoResolution::Res1920x1080:
    targetW = 1920;
    targetH = 1080;
    break;
  }
  // Pick the smallest sensor mode that can cover the request (both width
  // and height >= target). If none covers it (e.g. 1080p > 1332x990 in
  // height), fall back to the largest mode. For sub-QVGA requests the
  // smallest mode (1332x990) is used and the ISP scales down.
  SensorMode best = SensorMode::Mode1332x990;
  uint32_t bestArea = 0;
  bool found = false;
  for (int i = 0; i < kSensorModeCount; ++i) {
    const auto &m = kSensorModes[i];
    if (m.width >= targetW && m.height >= targetH) {
      uint32_t area = m.width * m.height;
      if (!found || area < bestArea) {
        bestArea = area;
        best = static_cast<SensorMode>(i + 1);
        found = true;
      }
    }
  }
  if (found)
    return best;
  // Request larger than every mode (shouldn't happen with the current
  // enum) — use the largest mode and let the ISP/crop handle it.
  return SensorMode::Mode4056x3040;
}

int videoBitrateToJpegQuality(int bitrateMbps) {
  switch (bitrateMbps) {
  case 1:
    return 30;
  case 5:
    return 50;
  case 10:
    return 75;
  case 20:
    return 90;
  default:
    return 50;
  }
}

std::chrono::microseconds videoFrameInterval(int fps) {
  if (fps <= 0)
    return std::chrono::microseconds(0);
  // 1e6 / fps, rounded to the nearest microsecond.
  return std::chrono::microseconds(1000000 / fps);
}

int clampFpsToSensorMode(int fps, SensorMode mode) {
  int maxFps = 0;
  switch (mode) {
  case SensorMode::Auto:
    maxFps = 60; // conservative default for Auto
    break;
  case SensorMode::Mode1332x990:
    maxFps = 120;
    break;
  case SensorMode::Mode2028x1080:
    maxFps = 50;
    break;
  case SensorMode::Mode2028x1520:
    maxFps = 40;
    break;
  case SensorMode::Mode4056x3040:
    maxFps = 10;
    break;
  }
  return std::min(fps, maxFps);
}

const char *videoCodecExtension(VideoCodec c) {
  // MJPEG and H264 both produce a .mjpeg container: H264 has no HW
  // encoder via libcamera on Pi Zero 2 W, so it falls back to MJPEG-
  // encoded JPEG frames. Never write JPEG frames to a .h264 file
  // (that would be an invalid H.264 bitstream).
  if (c == VideoCodec::YUV)
    return ".yuv";
  return ".mjpeg";
}

bool videoCodecUsesJpeg(VideoCodec c) {
  return c == VideoCodec::MJPEG || c == VideoCodec::H264;
}

} // namespace picamera
