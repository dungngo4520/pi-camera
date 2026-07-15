#include "output.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <png.h>

namespace picamera {

bool writePng(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    FILE *fp = fopen(path, "wb");
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
    for (uint32_t y = 0; y < h; ++y)
        rows[y] = const_cast<png_bytep>(rgb + y * w * 3);

    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

bool writePpm(const uint8_t *rgb, size_t size, uint32_t w, uint32_t h, const std::string &path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << w << " " << h << "\n255\n";
    out.write(reinterpret_cast<const char *>(rgb), static_cast<std::streamsize>(size));
    out.flush();
    return out.good();
}

bool writeRaw(const uint8_t *y, size_t ySize, const uint8_t *uv, size_t uvSize, const std::string &path) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(y), static_cast<std::streamsize>(ySize));
    out.write(reinterpret_cast<const char *>(uv), static_cast<std::streamsize>(uvSize));
    out.flush();
    return out.good();
}

}
