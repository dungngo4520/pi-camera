#include "timelapse.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>

namespace picamera {

std::string formatTimelapseName(const std::string &pattern, int i) {
    auto isPrintfInt = [](char conv) {
        return conv == 'd' || conv == 'i' || conv == 'u' ||
               conv == 'x' || conv == 'X' || conv == 'o';
    };

    bool hasIntConv = false;
    bool hasOtherConv = false;  // any %-specifier that isn't an int conv or %%

    for (size_t p = 0; p < pattern.size(); ++p) {
        if (pattern[p] != '%') continue;
        if (p + 1 >= pattern.size()) {
            throw std::invalid_argument("output pattern ends with stray '%'");
        }
        char next = pattern[p + 1];
        if (next == '%') { ++p; continue; }            // literal %
        // Skip printf flags/width/precision: [-+ 0#]*[0-9]*.?[0-9]*
        size_t q = p + 1;
        while (q < pattern.size() &&
               (pattern[q] == '-' || pattern[q] == '+' || pattern[q] == ' ' ||
                pattern[q] == '0' || pattern[q] == '#' || pattern[q] == '.' ||
                (pattern[q] >= '0' && pattern[q] <= '9'))) {
            ++q;
        }
        if (q >= pattern.size()) {
            throw std::invalid_argument("output pattern has incomplete '%...' specifier");
        }
        char conv = pattern[q];
        if (isPrintfInt(conv)) {
            hasIntConv = true;
        } else {
            hasOtherConv = true;
        }
        p = q;
    }

    if (hasIntConv && hasOtherConv) {
        throw std::invalid_argument(
            "output pattern mixes integer conversions with other '%...' specifiers; "
            "use either a printf %d-style pattern or a strftime pattern, not both");
    }

    if (hasIntConv) {
        char buf[512];
        int n = snprintf(buf, sizeof(buf), pattern.c_str(), i);
        if (n < 0) throw std::runtime_error("snprintf failed on output pattern");
        return std::string(buf, std::min<int>(n, static_cast<int>(sizeof(buf) - 1)));
    }

    char buf[512];
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&t);
    strftime(buf, sizeof(buf), pattern.c_str(), &tm);
    return std::string(buf);
}

} // namespace picamera
