#pragma once

#include <string>

namespace picamera {

// Build a timelapse filename from a user-supplied pattern.
//
// If the pattern contains a printf-style integer conversion (e.g. "%04d"),
// the sequence index `i` is substituted. Otherwise the pattern is treated as
// a strftime template and expanded with the current local time.
//
// Security: the printf path passes the user pattern to snprintf, so it must
// contain ONLY integer conversions (`%d`/`%i`/`%u`/`%x`/`%X`/`%o`) and `%%`.
// Any other `%` specifier (e.g. `%s`, `%n`, `%p`, or a strftime-style `%Y`)
// mixed with an integer conversion is rejected — otherwise `%s`/`%n` would
// read garbage off the stack. A pure strftime pattern (no integer conversion)
// is safe because strftime only reads the struct tm we pass it.
//
// Throws std::invalid_argument on a bad pattern, std::runtime_error on
// snprintf failure.
std::string formatTimelapseName(const std::string &pattern, int i);

} // namespace picamera
