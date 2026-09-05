#pragma once
#include <cstdio>
#include <string>
#include <string_view>

namespace picamera {

bool commitFile(FILE *fp, const std::string &path, std::string_view label,
                std::string *actualPath);
void discardFile(FILE *fp, const std::string &path);
bool commitFd(int fd, const std::string &path, std::string_view label,
              std::string *actualPath);
void discardFd(int fd, const std::string &path);

} // namespace picamera
