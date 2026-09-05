#include "timelapse.h"
#include "safe_path.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <string>

namespace picamera {

std::string formatTimelapseName(const std::string &pattern, int i) {
  auto isPrintfInt = [](char conv) {
    // Only allow signed int conversions (%d, %i) — the argument
    // passed to snprintf is `int`. %u/%x/%X/%o expect unsigned int,
    // and passing int to them is undefined behavior (C++ varargs).
    return conv == 'd' || conv == 'i';
  };

  bool hasIntConv = false;
  bool hasOtherConv = false; // any %-specifier that isn't an int conv or %%
  int intConvCount = 0;      // must be exactly 1 to match the single `int` arg

  for (size_t p = 0; p < pattern.size(); ++p) {
    if (pattern[p] != '%')
      continue;
    if (p + 1 >= pattern.size()) {
      throw std::invalid_argument("output pattern ends with stray '%'");
    }
    char next = pattern[p + 1];
    if (next == '%') {
      ++p;
      continue;
    } // literal %
    // Skip printf flags/width/precision: [-+ 0#]*[0-9]*.?[0-9]*
    size_t q = p + 1;
    while (q < pattern.size() &&
           (pattern[q] == '-' || pattern[q] == '+' || pattern[q] == ' ' ||
            pattern[q] == '0' || pattern[q] == '#' || pattern[q] == '.' ||
            (pattern[q] >= '0' && pattern[q] <= '9'))) {
      ++q;
    }
    if (q >= pattern.size()) {
      throw std::invalid_argument(
          "output pattern has incomplete '%...' specifier");
    }
    char conv = pattern[q];
    if (isPrintfInt(conv)) {
      hasIntConv = true;
      ++intConvCount;
    } else {
      hasOtherConv = true;
    }
    p = q;
  }

  if (hasIntConv && hasOtherConv) {
    throw std::invalid_argument(
        "output pattern mixes integer conversions with other '%...' "
        "specifiers; "
        "use either a printf %d-style pattern or a strftime pattern, not both");
  }

  // Reject patterns with no conversion specifiers at all — a static
  // pattern would produce the same filename for every shot, and the
  // second shot would fail with EEXIST (O_EXCL is used for file creation).
  if (!hasIntConv && !hasOtherConv) {
    throw std::invalid_argument(
        "output pattern must contain at least one conversion specifier "
        "(%d/%i for integer sequence, or a strftime specifier like %F_%H%M%S)");
  }

  if (hasIntConv) {
    // Reject patterns with more than one integer conversion.
    if (intConvCount != 1) {
      throw std::invalid_argument(
          "output pattern must contain exactly one integer conversion");
    }
    // Instead of passing the user pattern as a snprintf format string
    // (CWE-134: externally-controlled format string), split the pattern
    // at the %d/%i specifier and format the integer separately.
    // Find the specifier location (already validated above).
    size_t specStart = std::string::npos;
    size_t specEnd = 0;
    for (size_t p = 0; p < pattern.size(); ++p) {
      if (pattern[p] != '%')
        continue;
      if (p + 1 >= pattern.size())
        break;
      char next = pattern[p + 1];
      if (next == '%') {
        ++p;
        continue;
      }
      size_t q = p + 1;
      while (q < pattern.size() &&
             (pattern[q] == '-' || pattern[q] == '+' || pattern[q] == ' ' ||
              pattern[q] == '0' || pattern[q] == '#' || pattern[q] == '.' ||
              (pattern[q] >= '0' && pattern[q] <= '9'))) {
        ++q;
      }
      if (q < pattern.size() && (pattern[q] == 'd' || pattern[q] == 'i')) {
        specStart = p;
        specEnd = q + 1;
        break;
      }
      p = q;
    }
    if (specStart == std::string::npos) {
      throw std::runtime_error(
          "internal error: could not locate int conversion");
    }

    // Extract the format specifier to determine width/zero-padding.
    std::string fmtSpec = pattern.substr(specStart, specEnd - specStart);
    // Parse: %[flags][width][.precision](d|i)
    // Only '0' flag (zero-padding) is supported. Other printf flags
    // ('-', '+', ' ', '#') and precision are rejected to avoid
    // silently producing output that doesn't match user expectations.
    size_t width = 0;
    bool zeroPad = false;
    size_t fp = 1; // skip '%'
    // Parse flags — only '0' is supported.
    while (fp < fmtSpec.size() &&
           (fmtSpec[fp] == '-' || fmtSpec[fp] == '+' || fmtSpec[fp] == ' ' ||
            fmtSpec[fp] == '0' || fmtSpec[fp] == '#')) {
      if (fmtSpec[fp] != '0') {
        throw std::invalid_argument(
            std::string("output pattern: unsupported format flag '") +
            fmtSpec[fp] + "'");
      }
      zeroPad = true;
      ++fp;
    }
    // Parse width with overflow protection — cap at 511 (max useful
    // given the 512-byte result limit).
    while (fp < fmtSpec.size() && fmtSpec[fp] >= '0' && fmtSpec[fp] <= '9') {
      width = width * 10 + static_cast<size_t>(fmtSpec[fp] - '0');
      if (width > 511) {
        throw std::invalid_argument("output pattern width too large (max 511)");
      }
      ++fp;
    }
    // Reject precision (dot followed by digits) — not supported.
    if (fp < fmtSpec.size() && fmtSpec[fp] == '.') {
      throw std::invalid_argument(
          "output pattern: precision not supported for integer conversion");
    }

    // Format the integer with optional zero-padding or space-padding.
    // For negative numbers, the sign is placed before the padding
    // (e.g. %05d of -7 gives "-0007"), matching printf semantics.
    std::string numStr = std::to_string(i);
    if (width > 0 && numStr.size() < width) {
      char padChar = zeroPad ? '0' : ' ';
      if (i < 0) {
        // numStr starts with '-'; pad between sign and digits.
        std::string sign = numStr.substr(0, 1);
        std::string digits = numStr.substr(1);
        size_t padWidth = width - 1; // width includes the sign
        if (digits.size() < padWidth) {
          digits = std::string(padWidth - digits.size(), padChar) + digits;
        }
        numStr = sign + digits;
      } else {
        numStr = std::string(width - numStr.size(), padChar) + numStr;
      }
    }

    // Assemble: prefix + number + suffix
    // Unescape %% → % in prefix and suffix (matching printf/strftime
    // semantics for literal percent signs).
    auto unescapePct = [](std::string s) {
      std::string out;
      out.reserve(s.size());
      for (size_t p = 0; p < s.size(); ++p) {
        if (s[p] == '%' && p + 1 < s.size() && s[p + 1] == '%') {
          out += '%';
          ++p;
        } else {
          out += s[p];
        }
      }
      return out;
    };
    std::string prefix = unescapePct(pattern.substr(0, specStart));
    std::string suffix = unescapePct(pattern.substr(specEnd));
    std::string result = prefix + numStr + suffix;

    if (result.size() >= 512) {
      throw std::runtime_error(
          "output pattern produces a filename too long (>= 512 chars)");
    }
    if (!isSafeFilePath(result)) {
      throw std::runtime_error(
          "formatted timelapse filename contains unsafe path components");
    }
    return result;
  }

  char buf[512];
  auto now = std::chrono::system_clock::now();
  auto t = std::chrono::system_clock::to_time_t(now);
  std::tm tm;
  std::tm *tmPtr = nullptr;
#ifdef _WIN32
  if (std::localtime_s(&tm, &t) == 0)
    tmPtr = &tm;
#else
  tmPtr = localtime_r(&t, &tm);
#endif
  if (!tmPtr) {
    throw std::runtime_error("localtime failed (time_t out of range)");
  }
  size_t n = strftime(buf, sizeof(buf), pattern.c_str(), &tm);
  if (n == 0) {
    // strftime returns 0 if the result doesn't fit (or on error);
    // buf contents are unspecified in that case.
    throw std::runtime_error("strftime output too long or invalid pattern");
  }
  std::string result(buf, n);
  // Validate the formatted result — strftime specifiers like %D, %x,
  // or locale-dependent formats can produce path separators (/, ..)
  // that would allow directory escape.
  if (!isSafeFilePath(result)) {
    throw std::runtime_error("formatted timelapse filename contains unsafe "
                             "path components (.. or control characters)");
  }
  return result;
}

} // namespace picamera
