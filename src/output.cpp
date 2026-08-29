#include "output.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include <png.h>
#ifdef HAVE_JPEG
#include <jpeglib.h>
#endif

namespace picamera {

bool writePng(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h,
              int compressionLevel) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); fclose(fp); return false; }

    // setjmp point: on libpng error we longjmp here. Any C++ object with a
    // non-trivial destructor declared *before* this line would leak on the
    // error path (longjmp does not unwind). So `rows` is declared after the
    // setjmp and built only once png_write_info has succeeded.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    // Compression level: 0=none, 1=fastest, 6=zlib default, 9=best.
    // Lower levels trade ~15% larger files for ~2x faster encode — useful
    // on the Pi Zero where PNG encode dominates capture time.
    png_set_compression_level(png, compressionLevel);
    // Filter heuristic: let libpng pick the best filter per row (default).
    // For maximum speed use PNG_FILTER_NONE; for best ratio use PNG_ALL_FILTERS.
    if (compressionLevel <= 1) {
        png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, w, h, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(h);
    for (uint32_t y = 0; y < h; ++y)
        rows[y] = const_cast<png_bytep>(rgb + static_cast<size_t>(y) * w * 3);

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

bool writeJpeg(const uint8_t *data, size_t size, const std::string &path) {
    // The Pi ISP produces a complete JPEG bitstream in the MJPEG buffer;
    // we just write it to disk. No software encode needed.
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    out.flush();
    return out.good();
}

bool writeJpegRgb(const uint8_t *rgb, uint32_t w, uint32_t h,
                  const std::string &path, int quality) {
#ifdef HAVE_JPEG
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) return false;

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    // libjpeg wants row pointers; allocate once
    std::vector<JSAMPROW> rowPtrs(h);
    for (uint32_t r = 0; r < h; ++r)
        rowPtrs[r] = const_cast<JSAMPROW>(rgb + (size_t)r * w * 3);

    while (cinfo.next_scanline < cinfo.image_height) {
        JDIMENSION written = jpeg_write_scanlines(&cinfo, &rowPtrs[cinfo.next_scanline],
                                                   h - cinfo.next_scanline);
        if (written == 0) break;
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return true;
#else
    (void)rgb; (void)w; (void)h; (void)path; (void)quality;
    std::cerr << "writeJpegRgb: libjpeg not available at build time\n";
    return false;
#endif
}

}
