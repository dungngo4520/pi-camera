#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

#include <libcamera/framebuffer.h>

#include <sys/mman.h>
#include <unistd.h>

#include "safe_path.h" // errnoString

namespace picamera {

// RAII wrapper for an mmap'd dmabuf plane.
//
// libcamera backends may put the two NV12 planes in the same dmabuf (Pi VC4)
// or in separate ones; mapping per-fd is correct in both cases. Single-plane
// formats (DNG raw, HW MJPEG) only map planes[0].
//
// The mapping is released automatically on destruction or explicit reset(),
// so callers can't leak the mapping on an early-return / exception path.
class MappedPlane {
public:
  MappedPlane() = default;

  // Map a libcamera FrameBuffer::Plane for reading. `what` is used in
  // error messages (e.g. "plane0", "plane1"). On failure, data() == nullptr.
  MappedPlane(const libcamera::FrameBuffer::Plane &plane, const char *what) {
    // Reject zero-length planes explicitly — a non-zero offset would
    // make mapLen > 0 (passing the mmap check) but data_ would point
    // past the meaningful region.
    if (plane.length == 0) {
      std::cerr << "mmap " << what << " failed: zero-length plane\n";
      return;
    }
    // Checked addition to prevent integer overflow in map length
    size_t mapLen = 0;
    if (plane.offset > SIZE_MAX - plane.length) {
      std::cerr << "mmap " << what << " failed: offset+length overflow\n";
      return;
    }
    mapLen = plane.offset + plane.length;
    void *base =
        mmap(nullptr, mapLen, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
    if (base == MAP_FAILED) {
      std::cerr << "mmap " << what << " failed: " << errnoString(errno) << "\n";
      return;
    }
    base_ = base;
    mapLen_ = mapLen;
    data_ = static_cast<const uint8_t *>(base) + plane.offset;
    size_ = plane.length;
  }

  ~MappedPlane() { reset(); }

  MappedPlane(const MappedPlane &) = delete;
  MappedPlane &operator=(const MappedPlane &) = delete;

  MappedPlane(MappedPlane &&other) noexcept { steal(other); }
  MappedPlane &operator=(MappedPlane &&other) noexcept {
    if (this != &other) {
      reset();
      steal(other);
    }
    return *this;
  }

  const uint8_t *data() const { return data_; }
  size_t size() const { return size_; }
  bool valid() const { return data_ != nullptr; }
  std::span<const uint8_t> bytes() const { return {data_, size_}; }

  void reset() {
    if (base_) {
      munmap(base_, mapLen_);
      base_ = nullptr;
      data_ = nullptr;
      mapLen_ = 0;
      size_ = 0;
    }
  }

private:
  void steal(MappedPlane &o) {
    base_ = o.base_;
    data_ = o.data_;
    mapLen_ = o.mapLen_;
    size_ = o.size_;
    o.base_ = nullptr;
    o.data_ = nullptr;
    o.mapLen_ = 0;
    o.size_ = 0;
  }

  void *base_ = nullptr;
  const uint8_t *data_ = nullptr;
  size_t mapLen_ = 0;
  size_t size_ = 0;
};

} // namespace picamera
