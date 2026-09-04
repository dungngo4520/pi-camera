#pragma once

#include <string>

namespace picamera {

class CameraApp;

// Build a timelapse filename from a user-supplied pattern.
//
// If the pattern contains a printf-style integer conversion (e.g. "%04d"),
// the sequence index `i` is substituted. Otherwise the pattern is treated as
// a strftime template and expanded with the current local time.
//
// Security: the printf path passes the user pattern to snprintf, so it must
// contain ONLY signed integer conversions (`%d`/`%i`) and `%%`.
// Any other `%` specifier (e.g. `%s`, `%n`, `%p`, `%u`, `%x`, or a
// strftime-style `%Y`) mixed with an integer conversion is rejected —
// otherwise `%s`/`%n` would read garbage off the stack. A pure strftime
// pattern (no integer conversion) is safe because strftime only reads the
// struct tm we pass it.
//
// Throws std::invalid_argument on a bad pattern, std::runtime_error on
// snprintf failure.
std::string formatTimelapseName(const std::string &pattern, int i);

// Run a timelapse capture loop: `count` shots at `intervalSec` apart (0 = infinite),
// saving each to a filename derived from `pattern` via formatTimelapseName().
// If `captureDir` is non-empty and not ".", the formatted filename is joined
// with the directory and validated with isFilePathInsideDir() to ensure the
// final path stays within the capture directory.
// The camera must already be init()'d and configure()'d by the caller.
// SIGINT/SIGTERM stops gracefully after the current shot completes.
// Returns true if all shots succeeded (or the loop was interrupted cleanly),
// false on a capture or pattern error.
bool runTimelapse(CameraApp &app, int intervalSec, int count,
                  const std::string &pattern,
                  const std::string &captureDir = "");

} // namespace picamera
