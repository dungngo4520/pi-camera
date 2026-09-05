#include "image_effects.h"
#include "test_runner.h"
#include "video_config.h"

#include <chrono>
#include <cstdint>
#include <vector>

using picamera::clampFpsToSensorMode;
using picamera::scaleRgb24Bilinear;
using picamera::SensorMode;
using picamera::videoBitrateToJpegQuality;
using picamera::videoCodecExtension;
using picamera::videoCodecUsesJpeg;
using picamera::videoFrameInterval;
using picamera::videoResolutionToSensorMode;
using picamera::VideoCodec;
using picamera::VideoResolution;

// --- Resolution-to-sensor-mode mapping ---

TEST(video_res_to_sensor_mode_320x240) {
  // 320x240 fits in the smallest mode (1332x990).
  CHECK(videoResolutionToSensorMode(VideoResolution::Res320x240) ==
        SensorMode::Mode1332x990);
}

TEST(video_res_to_sensor_mode_640x480) {
  // 640x480 fits in the smallest mode (1332x990).
  CHECK(videoResolutionToSensorMode(VideoResolution::Res640x480) ==
        SensorMode::Mode1332x990);
}

TEST(video_res_to_sensor_mode_720p) {
  // 1280x720: 1332x990 covers it (1332>=1280, 990>=720).
  CHECK(videoResolutionToSensorMode(VideoResolution::Res1280x720) ==
        SensorMode::Mode1332x990);
}

TEST(video_res_to_sensor_mode_1080p) {
  // 1920x1080: 1332x990 is too short (990<1080); 2028x1080 covers it.
  CHECK(videoResolutionToSensorMode(VideoResolution::Res1920x1080) ==
        SensorMode::Mode2028x1080);
}

// --- Bitrate-to-JPEG-quality mapping ---

TEST(video_bitrate_to_quality_1mbps) {
  CHECK(videoBitrateToJpegQuality(1) == 30);
}

TEST(video_bitrate_to_quality_5mbps) {
  CHECK(videoBitrateToJpegQuality(5) == 50);
}

TEST(video_bitrate_to_quality_10mbps) {
  CHECK(videoBitrateToJpegQuality(10) == 75);
}

TEST(video_bitrate_to_quality_20mbps) {
  CHECK(videoBitrateToJpegQuality(20) == 90);
}

TEST(video_bitrate_to_quality_unknown_defaults) {
  CHECK(videoBitrateToJpegQuality(0) == 50);
  CHECK(videoBitrateToJpegQuality(99) == 50);
}

TEST(video_bitrate_to_quality_monotonic) {
  // Higher bitrate must not yield lower quality.
  CHECK(videoBitrateToJpegQuality(1) <= videoBitrateToJpegQuality(5));
  CHECK(videoBitrateToJpegQuality(5) <= videoBitrateToJpegQuality(10));
  CHECK(videoBitrateToJpegQuality(10) <= videoBitrateToJpegQuality(20));
}

// --- FPS throttling ---

TEST(video_frame_interval_30fps) {
  auto iv = videoFrameInterval(30);
  CHECK(iv.count() == 33333); // 1e6/30 = 33333
}

TEST(video_frame_interval_60fps) {
  auto iv = videoFrameInterval(60);
  CHECK(iv.count() == 16666); // 1e6/60 = 16666
}

TEST(video_frame_interval_10fps) {
  auto iv = videoFrameInterval(10);
  CHECK(iv.count() == 100000);
}

TEST(video_frame_interval_24fps) {
  auto iv = videoFrameInterval(24);
  CHECK(iv.count() == 41666); // 1e6/24 = 41666
}

TEST(video_frame_interval_50fps) {
  auto iv = videoFrameInterval(50);
  CHECK(iv.count() == 20000);
}

TEST(video_frame_interval_zero_fps) {
  CHECK(videoFrameInterval(0).count() == 0);
}

TEST(video_frame_interval_negative_fps) {
  CHECK(videoFrameInterval(-5).count() == 0);
}

// --- Clamp FPS to sensor mode ---

TEST(clamp_fps_1080p_mode_caps_at_50) {
  // 2028x1080 mode maxes at 50fps; 60 should clamp to 50.
  CHECK(clampFpsToSensorMode(60, SensorMode::Mode2028x1080) == 50);
  CHECK(clampFpsToSensorMode(30, SensorMode::Mode2028x1080) == 30);
}

TEST(clamp_fps_4k_mode_caps_at_10) {
  CHECK(clampFpsToSensorMode(30, SensorMode::Mode4056x3040) == 10);
  CHECK(clampFpsToSensorMode(10, SensorMode::Mode4056x3040) == 10);
}

TEST(clamp_fps_1332x990_mode_allows_120) {
  CHECK(clampFpsToSensorMode(60, SensorMode::Mode1332x990) == 60);
  CHECK(clampFpsToSensorMode(120, SensorMode::Mode1332x990) == 120);
}

TEST(clamp_fps_2028x1520_mode_caps_at_40) {
  CHECK(clampFpsToSensorMode(50, SensorMode::Mode2028x1520) == 40);
  CHECK(clampFpsToSensorMode(40, SensorMode::Mode2028x1520) == 40);
}

TEST(clamp_fps_auto_mode_conservative_60) {
  CHECK(clampFpsToSensorMode(60, SensorMode::Auto) == 60);
  CHECK(clampFpsToSensorMode(120, SensorMode::Auto) == 60);
}

// --- Codec extension / uses-JPEG ---

TEST(video_codec_extension_mjpeg) {
  CHECK(std::string(videoCodecExtension(VideoCodec::MJPEG)) == ".mjpeg");
}

TEST(video_codec_extension_h264_falls_back_to_mjpeg) {
  // H264 must NOT produce a .h264 extension (would be an invalid bitstream).
  CHECK(std::string(videoCodecExtension(VideoCodec::H264)) == ".mjpeg");
}

TEST(video_codec_extension_yuv) {
  CHECK(std::string(videoCodecExtension(VideoCodec::YUV)) == ".yuv");
}

TEST(video_codec_uses_jpeg_mjpeg) {
  CHECK(videoCodecUsesJpeg(VideoCodec::MJPEG));
}

TEST(video_codec_uses_jpeg_h264_fallback) {
  CHECK(videoCodecUsesJpeg(VideoCodec::H264));
}

TEST(video_codec_uses_jpeg_yuv_false) {
  CHECK(!videoCodecUsesJpeg(VideoCodec::YUV));
}

// --- scaleRgb24Bilinear ---

TEST(scale_rgb24_identity_copy) {
  std::vector<uint8_t> src = {10, 20, 30, 40, 50, 60};
  auto out = scaleRgb24Bilinear(src.data(), 1, 2, 1, 2);
  CHECK(out.size() == 6);
  CHECK(out == src);
}

TEST(scale_rgb24_invalid_null) {
  CHECK(scaleRgb24Bilinear(nullptr, 4, 4, 2, 2).empty());
}

TEST(scale_rgb24_invalid_zero_dims) {
  std::vector<uint8_t> src(12, 0);
  CHECK(scaleRgb24Bilinear(src.data(), 0, 4, 2, 2).empty());
  CHECK(scaleRgb24Bilinear(src.data(), 4, 0, 2, 2).empty());
  CHECK(scaleRgb24Bilinear(src.data(), 4, 4, 0, 2).empty());
  CHECK(scaleRgb24Bilinear(src.data(), 4, 4, 2, 0).empty());
}

TEST(scale_rgb24_upscale_2x) {
  // 1x1 red pixel -> 2x2, all pixels should be red.
  std::vector<uint8_t> src = {255, 0, 0};
  auto out = scaleRgb24Bilinear(src.data(), 1, 1, 2, 2);
  CHECK(out.size() == 12);
  for (size_t i = 0; i < 4; ++i) {
    CHECK(out[i * 3] == 255);
    CHECK(out[i * 3 + 1] == 0);
    CHECK(out[i * 3 + 2] == 0);
  }
}

TEST(scale_rgb24_downscale_3x3_to_2x2) {
  // 3x3 source with red at top-left and blue at bottom-right; the 2x2
  // output samples the four corners (sx/sy map to {0, srcDim-1}).
  // Layout (RGB, row-major): row0 = [R, ., .], row1 = [.], row2 = [. . B]
  std::vector<uint8_t> src(27, 0);
  src[0] = 255;                       // (0,0) red
  src[(2 * 3 + 2) * 3 + 2] = 255;     // (2,2) blue
  auto out = scaleRgb24Bilinear(src.data(), 3, 3, 2, 2);
  CHECK(out.size() == 12);
  // (0,0) -> src(0,0) = red
  CHECK(out[0] == 255);
  CHECK(out[2] == 0);
  // (1,1) -> src(2,2) = blue
  CHECK(out[(1 * 2 + 1) * 3] == 0);
  CHECK(out[(1 * 2 + 1) * 3 + 2] == 255);
}

TEST(scale_rgb24_preserves_size) {
  // 2x2 -> 2x2 should be an exact copy.
  std::vector<uint8_t> src(12);
  for (int i = 0; i < 12; ++i)
    src[i] = static_cast<uint8_t>(i);
  auto out = scaleRgb24Bilinear(src.data(), 2, 2, 2, 2);
  CHECK(out.size() == 12);
  CHECK(out == src);
}
