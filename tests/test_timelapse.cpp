#include "test_runner.h"
#include "timelapse.h"

#include <stdexcept>

using namespace picamera;

TEST(timelapse_printf_zero_padded) {
    CHECK_EQ(formatTimelapseName("shot_%04d.png", 0), std::string("shot_0000.png"));
    CHECK_EQ(formatTimelapseName("shot_%04d.png", 42), std::string("shot_0042.png"));
    CHECK_EQ(formatTimelapseName("shot_%04d.png", 9999), std::string("shot_9999.png"));
}

TEST(timelapse_printf_plain_d) {
    CHECK_EQ(formatTimelapseName("frame_%d.ppm", 7), std::string("frame_7.ppm"));
}

TEST(timelapse_printf_no_extension) {
    CHECK_EQ(formatTimelapseName("img_%d", 3), std::string("img_3"));
}

TEST(timelapse_literal_percent) {
    // %% is a literal %, not a conversion — no int conv, so strftime path,
    // which leaves %% as %. Result should contain a literal %.
    auto out = formatTimelapseName("100%%_done", 0);
    CHECK(out.find("100%_done") != std::string::npos);
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
    // No % at all -> strftime path, pattern returned verbatim (strftime of a
    // string with no specifiers is the string itself).
    CHECK_EQ(formatTimelapseName("static_name.png", 0), std::string("static_name.png"));
}
