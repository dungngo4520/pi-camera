#include "test_runner.h"
#include "wb_utils.h"

#include <string>

using picamera::AwbModeValue;
using picamera::kelvinToGains;
using picamera::lookupAwbMode;

// --- lookupAwbMode tests ---

TEST(lookup_awb_mode_auto) {
  CHECK(lookupAwbMode("auto").has_value());
  CHECK(*lookupAwbMode("auto") == AwbModeValue::AwbAuto);
  CHECK(static_cast<int>(*lookupAwbMode("auto")) == 0);
}

TEST(lookup_awb_mode_incandescent) {
  CHECK(*lookupAwbMode("incandescent") == AwbModeValue::AwbIncandescent);
  CHECK(static_cast<int>(*lookupAwbMode("incandescent")) == 1);
}

TEST(lookup_awb_mode_tungsten) {
  CHECK(*lookupAwbMode("tungsten") == AwbModeValue::AwbTungsten);
  CHECK(static_cast<int>(*lookupAwbMode("tungsten")) == 2);
}

TEST(lookup_awb_mode_fluorescent) {
  CHECK(*lookupAwbMode("fluorescent") == AwbModeValue::AwbFluorescent);
  CHECK(static_cast<int>(*lookupAwbMode("fluorescent")) == 3);
}

TEST(lookup_awb_mode_indoor) {
  CHECK(*lookupAwbMode("indoor") == AwbModeValue::AwbIndoor);
  CHECK(static_cast<int>(*lookupAwbMode("indoor")) == 4);
}

TEST(lookup_awb_mode_daylight) {
  CHECK(*lookupAwbMode("daylight") == AwbModeValue::AwbDaylight);
  CHECK(static_cast<int>(*lookupAwbMode("daylight")) == 5);
}

TEST(lookup_awb_mode_cloudy) {
  CHECK(*lookupAwbMode("cloudy") == AwbModeValue::AwbCloudy);
  CHECK(static_cast<int>(*lookupAwbMode("cloudy")) == 6);
}

TEST(lookup_awb_mode_shade_maps_to_daylight) {
  // "shade" aliases AwbDaylight (matching the original controls.cpp table).
  CHECK(*lookupAwbMode("shade") == AwbModeValue::AwbDaylight);
  CHECK(static_cast<int>(*lookupAwbMode("shade")) == 5);
}

TEST(lookup_awb_mode_flash_maps_to_daylight) {
  // "flash" aliases AwbDaylight (matching the original controls.cpp table).
  CHECK(*lookupAwbMode("flash") == AwbModeValue::AwbDaylight);
  CHECK(static_cast<int>(*lookupAwbMode("flash")) == 5);
}

TEST(lookup_awb_mode_unknown_returns_nullopt) {
  CHECK(!lookupAwbMode("nonsense").has_value());
  CHECK(!lookupAwbMode("").has_value());
  CHECK(!lookupAwbMode("AUTO").has_value()); // case-sensitive
  CHECK(!lookupAwbMode("sunny").has_value());
}

TEST(lookup_awb_mode_all_valid_modes_resolve) {
  // Every mode advertised by camera_config.h's kAwbModes[] must resolve.
  for (std::string name : {"auto", "incandescent", "tungsten", "fluorescent",
                           "indoor", "daylight", "cloudy", "shade", "flash"}) {
    CHECK(lookupAwbMode(name).has_value());
  }
}

// --- kelvinToGains tests ---
//
// kelvinToGains() approximates a blackbody curve. Behaviour notes verified
// against the implementation:
//   - Red gain is clamped to 1.0 (max) for t = K/100 <= 66, then decreases
//     via the formula branch for t > ~66.9.
//   - Blue gain follows 138.5177*(t-10)^-0.0755 for 19 < t < 66 (decreasing
//     with K), saturates to 1.0 for t >= 66, and is floored at 0.1 for t <= 19.
//   - At exactly 6600K (t == 66) both branches hit their saturation: r=1.0,
//     b=1.0 — the neutral crossover point.

TEST(kelvin_to_gains_low_kelvin_warm) {
  // 2500K (warm/tungsten): red gain at maximum (1.0), blue gain low (< 1.0).
  float r = 0;
  float b = 0;
  kelvinToGains(2500, r, b);
  CHECK(r == 1.0f); // red maxed out
  CHECK(b < 1.0f);  // blue deficient
  CHECK(r > b);     // warm: more red than blue
}

TEST(kelvin_to_gains_high_kelvin_cool) {
  // 10000K (cool/shade): blue gain at maximum (1.0), red gain reduced.
  float r = 0;
  float b = 0;
  kelvinToGains(10000, r, b);
  CHECK(b == 1.0f); // blue maxed out
  CHECK(r < 1.0f);  // red reduced
  CHECK(b > r);     // cool: more blue than red
}

TEST(kelvin_to_gains_neutral_at_6600k) {
  // 6600K (t == 66) is the neutral crossover: both gains saturate to 1.0.
  float r = 0;
  float b = 0;
  kelvinToGains(6600, r, b);
  CHECK(r == 1.0f);
  CHECK(b == 1.0f);
}

TEST(kelvin_to_gains_mid_kelvin_red_maxed_blue_moderate) {
  // 5500K (daylight): red is still at maximum (1.0); blue is moderate
  // (between the low-K and high-K extremes), so the result is warm-ish.
  float r = 0;
  float b = 0;
  kelvinToGains(5500, r, b);
  CHECK(r == 1.0f);
  CHECK(b > 0.3f);
  CHECK(b < 0.5f);
  CHECK(r > b);
}

TEST(kelvin_to_gains_red_non_increasing_with_kelvin) {
  // Red gain is 1.0 (max) for low/mid Kelvin, then drops for high Kelvin.
  float rLow = 0;
  float rMid = 0;
  float rHigh = 0;
  float bDummy = 0;
  kelvinToGains(2500, rLow, bDummy);
  kelvinToGains(5500, rMid, bDummy);
  kelvinToGains(10000, rHigh, bDummy);
  CHECK(rLow >= rMid);
  CHECK(rMid >= rHigh);
  CHECK(rHigh < 1.0f); // drops below max at high K
}

TEST(kelvin_to_gains_blue_high_kelvin_exceeds_low_kelvin) {
  // Overall warm->cool trend: blue at 10000K exceeds blue at 2500K.
  // (Blue is non-monotonic in the mid range — it decreases from 2000K to
  // 6500K then jumps to 1.0 at 6600K — so only the endpoint trend holds.)
  float bLow = 0;
  float bHigh = 0;
  float rDummy = 0;
  kelvinToGains(2500, rDummy, bLow);
  kelvinToGains(10000, rDummy, bHigh);
  CHECK(bHigh > bLow);
}

TEST(kelvin_to_gains_always_positive_and_bounded) {
  // Gains must always be within [0.1, 8.0] across a wide range.
  for (int k : {1000, 1500, 1900, 2000, 2500, 3000, 4000, 5000, 5500, 6500,
                6600, 8000, 10000, 20000, 50000}) {
    float r = -1;
    float b = -1;
    kelvinToGains(k, r, b);
    CHECK(r >= 0.1f);
    CHECK(r <= 8.0f);
    CHECK(b >= 0.1f);
    CHECK(b <= 8.0f);
  }
}

TEST(kelvin_to_gains_very_low_kelvin_clamped) {
  // Very low Kelvin: blue hits the floor (0.0 raw -> clamped to 0.1 gain),
  // red is at maximum (255 raw -> 1.0 gain).
  float r = 0;
  float b = 0;
  kelvinToGains(1000, r, b);
  CHECK(r == 1.0f); // t=10 <= 66 -> r=255 -> 1.0
  CHECK(b == 0.1f); // t=10 <= 19 -> b=0 -> clamped to 0.1
}

TEST(kelvin_to_gains_very_high_kelvin_clamped_blue) {
  // Very high Kelvin: blue saturates to 255 -> 1.0 gain; red drops below 1.0.
  float r = 0;
  float b = 0;
  kelvinToGains(50000, r, b);
  CHECK(b == 1.0f); // t=500 >= 66 -> b=255 -> 1.0
  CHECK(r < 1.0f);
}

TEST(kelvin_to_gains_boundary_6600k_blue_saturation) {
  // 6600K is where blue saturates to 1.0 (t >= 66 branch). Just below
  // (6599K) blue is still on the formula branch (~0.40).
  float rAt = 0;
  float bAt = 0;
  float rBelow = 0;
  float bBelow = 0;
  kelvinToGains(6600, rAt, bAt);
  kelvinToGains(6599, rBelow, bBelow);
  CHECK(bAt == 1.0f);
  CHECK(bBelow < 1.0f);
  CHECK(rAt == 1.0f);
  CHECK(rBelow == 1.0f);
}

TEST(kelvin_to_gains_boundary_red_drops_below_max) {
  // Red stays at 1.0 through 6601K (formula clamps to 255 just above t=66),
  // then drops below 1.0 by 7000K (formula yields < 255).
  float rAt6601 = 0;
  float bDummy = 0;
  float rAt7000 = 0;
  kelvinToGains(6601, rAt6601, bDummy);
  kelvinToGains(7000, rAt7000, bDummy);
  CHECK(rAt6601 == 1.0f); // formula still clamps to 255 just above 66
  CHECK(rAt7000 < 1.0f);  // drops below max by 7000K
  CHECK(rAt7000 > 0.0f);
}

TEST(kelvin_to_gains_boundary_1900k_blue_branch) {
  // 1900K is the boundary where blue is 0 (t == 19). Just above (2000K)
  // the blue formula branch is used; both must produce valid gains.
  float rAt = 0;
  float bAt = 0;
  float rAbove = 0;
  float bAbove = 0;
  kelvinToGains(1900, rAt, bAt);
  kelvinToGains(2000, rAbove, bAbove);
  CHECK(bAt == 0.1f);   // t=19 <= 19 -> b=0 -> clamped to 0.1
  CHECK(bAbove > 0.1f); // t=20 > 19 -> formula, above the floor
  CHECK(bAbove < 1.0f);
  CHECK(rAt == 1.0f);
  CHECK(rAbove == 1.0f);
}
