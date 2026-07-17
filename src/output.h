#pragma once

#include <cstdint>
#include <string>

namespace picamera {

bool writePng(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h,
              int compressionLevel = 6);
bool writePpm(const uint8_t *rgb, size_t size, uint32_t w, uint32_t h, const std::string &path);
bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv, size_t uvSize, const std::string &path);
bool writeJpeg(const uint8_t *data, size_t size, const std::string &path);

}
