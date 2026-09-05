#include "test_runner.h"
#include "timelapse.h"

#include <stdexcept>

using namespace picamera;

TEST(timelapse_printf_zero_padded) {
  CHECK_EQ(formatTimelapseName("shot_%04d.png", 0),
           std::string("shot_0000.png"));
  CHECK_EQ(formatTimelapseName("shot_%04d.png", 42),
           std::string("shot_0042.png"));
  CHECK_EQ(formatTimelapseName("shot_%04d.png", 9999),
           std::string("shot_9999.png"));
}

TEST(timelapse_printf_plain_d) {
  CHECK_EQ(formatTimelapseName("frame_%d.ppm", 7), std::string("frame_7.ppm"));
}

TEST(timelapse_printf_no_extension) {
  CHECK_EQ(formatTimelapseName("img_%d", 3), std::string("img_3"));
}

TEST(timelapse_literal_percent) {
  // %% is a literal %, not a conversion — no int conv and no strftime
  // specifier, so the pattern is rejected (a static pattern would
  // produce the same filename for every shot, causing EEXIST).
  bool threw = false;
  try {
    (void)formatTimelapseName("100%%_done", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_strftime_path_used_when_no_int_conv) {
  // %Y is a strftime specifier, not a printf int conversion. With no int
  // conv present, the strftime path runs and produces a 4-digit year.
  auto out = formatTimelapseName("photo_%Y.png", 0);
  // Year is 20xx — check it starts with "photo_20" and ends ".png".
  CHECK(out.rfind("photo_20", 0) == 0);
  CHECK(out.size() >= 4);
  CHECK(out.substr(out.size() - 4) == ".png");
}

TEST(timelapse_rejects_format_string_vuln_s) {
  bool threw = false;
  try {
    (void)formatTimelapseName("evil_%s_%04d", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_format_string_vuln_n) {
  bool threw = false;
  try {
    (void)formatTimelapseName("evil_%n_%04d", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_mixed_printf_strftime) {
  bool threw = false;
  try {
    (void)formatTimelapseName("mix_%Y_%04d.png", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_stray_trailing_percent) {
  bool threw = false;
  try {
    (void)formatTimelapseName("bad%", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_incomplete_specifier) {
  bool threw = false;
  try {
    (void)formatTimelapseName("bad%04", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_plain_pattern_no_percent) {
  // No % at all -> rejected (a static pattern would produce the same
  // filename for every shot, causing EEXIST on the second shot).
  bool threw = false;
  try {
    (void)formatTimelapseName("static_name.png", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_multiple_int_conversions) {
  // Two %d specifiers but only one int arg — snprintf would read
  // garbage from the stack (undefined behavior).
  bool threw = false;
  try {
    (void)formatTimelapseName("img_%d_%d.jpg", 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_rejects_truncation) {
  // A pattern that produces a string >= 512 bytes should be rejected,
  // not silently truncated (which would drop the file extension).
  bool threw = false;
  try {
    // %0510d pads to 510 chars + "a" + "b.ppm" = 518 > 512
    (void)formatTimelapseName("a%0510db.ppm", 0);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  CHECK(threw);
}

TEST(timelapse_unescapes_double_percent_in_int_path) {
  // %% in the integer-conversion path should become a single %,
  // matching printf/strftime semantics.
  auto out = formatTimelapseName("img_%%_%04d.ppm", 7);
  CHECK(out.find("img_%_0007.ppm") != std::string::npos);
  CHECK(out.find("%%") == std::string::npos);
}

TEST(timelapse_rejects_unsupported_flags) {
  // '-', '+', ' ', '#' flags are not supported for integer conversion.
  const char *badPatterns[] = {"%-04d.ppm", "%+04d.ppm", "% 04d.ppm",
                               "%#04d.ppm"};
  for (const char *p : badPatterns) {
    bool threw = false;
    try {
      (void)formatTimelapseName(p, 0);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    CHECK(threw);
  }
}

TEST(timelapse_rejects_precision) {
  // Precision (.N) is not supported for integer conversion.
  bool threw = false;
  try {
    (void)formatTimelapseName("img_%5.3d.ppm", 7);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  CHECK(threw);
}
