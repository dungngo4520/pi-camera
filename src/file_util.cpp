#include "file_util.h"
#include "util.h"

#include <cerrno>
#include <iostream>
#include <unistd.h>

namespace picamera {

bool commitFile(FILE *fp, const std::string &path, std::string_view label,
                std::string *actualPath) {
  if (fflush(fp) != 0) {
    std::cerr << label << ": fflush() failed: " << errnoString(errno) << "\n";
    fclose(fp);
    unlink(path.c_str());
    return false;
  }
  if (::fsync(fileno(fp)) != 0) {
    std::cerr << label << ": fsync() failed: " << errnoString(errno) << "\n";
    fclose(fp);
    unlink(path.c_str());
    return false;
  }
  if (fclose(fp) != 0) {
    std::cerr << label << ": fclose() failed: " << errnoString(errno) << "\n";
    unlink(path.c_str());
    return false;
  }
  if (actualPath)
    *actualPath = path;
  return true;
}

void discardFile(FILE *fp, const std::string &path) {
  fclose(fp);
  unlink(path.c_str());
}

bool commitFd(int fd, const std::string &path, std::string_view label,
              std::string *actualPath) {
  if (::fsync(fd) != 0) {
    std::cerr << label << ": fsync() failed: " << errnoString(errno) << "\n";
    close(fd);
    unlink(path.c_str());
    return false;
  }
  if (close(fd) != 0) {
    std::cerr << label << ": close() failed: " << errnoString(errno) << "\n";
    unlink(path.c_str());
    return false;
  }
  if (actualPath)
    *actualPath = path;
  return true;
}

void discardFd(int fd, const std::string &path) {
  close(fd);
  unlink(path.c_str());
}

} // namespace picamera
