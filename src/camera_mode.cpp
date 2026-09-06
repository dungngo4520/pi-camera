#include "camera_mode.h"
#include "font.h"
#include "image_effects.h"
#include "settings_menu.h"
#include "util.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace picamera {

namespace {

constexpr int kMargin = 2;
constexpr int kTextHeight = 7;
constexpr int kTextWidth = 6;
constexpr int kRowStep = kTextHeight + 2;

constexpr uint16_t kColorAmber = rgb565(255, 180, 0);
constexpr uint16_t kColorGrid = rgb565(80, 80, 80);
constexpr uint16_t kColorCyan = rgb565(0, 200, 200);
constexpr int kHistogramSampleStep = 4;
constexpr int kZebraStripePeriod = 4;
constexpr uint32_t kMillisPerSec = 1000;
constexpr int kHistogramBins = 256;

void fillRect(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y, int w,
              int h, uint16_t color) {
  size_t fbBytes = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, fbBytes) ||
      !checkedMul(fbBytes, 2, fbBytes))
    return;
  for (int dy = 0; dy < h; ++dy) {
    for (int dx = 0; dx < w; ++dx) {
      int px = x + dx;
      int py = y + dy;
      if (px >= 0 && py >= 0 && static_cast<uint32_t>(px) < fbW &&
          static_cast<uint32_t>(py) < fbH) {
        size_t idx = (static_cast<size_t>(py) * fbW + px) * 2;
        rgb565[idx] = static_cast<uint8_t>(color >> 8);
        rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF);
      }
    }
  }
}

void hLine(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y, int len,
           uint16_t color) {
  fillRect(rgb565, fbW, fbH, x, y, len, 1, color);
}

void vLine(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y, int len,
           uint16_t color) {
  fillRect(rgb565, fbW, fbH, x, y, 1, len, color);
}

void rectOutline(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                 int w, int h, uint16_t color) {
  hLine(rgb565, fbW, fbH, x, y, w, color);
  hLine(rgb565, fbW, fbH, x, y + h - 1, w, color);
  vLine(rgb565, fbW, fbH, x, y, h, color);
  vLine(rgb565, fbW, fbH, x + w - 1, y, h, color);
}

void drawTextWithBg(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                    std::string_view text, uint16_t fg, uint16_t bg) {
  if (x < 1 || y < 1)
    return;
  if (static_cast<uint32_t>(x) >= fbW || static_cast<uint32_t>(y) >= fbH)
    return;
  size_t textW = 0;
  if (!checkedMul(text.size(), static_cast<size_t>(kTextWidth), textW))
    textW = static_cast<size_t>(fbW);
  int availW = static_cast<int>(fbW) - (x - 1);
  int bgW = static_cast<int>(
      std::min(textW + 2, static_cast<size_t>(std::max(0, availW))));
  if (bgW <= 0)
    return;
  fillRect(rgb565, fbW, fbH, x - 1, y - 1, bgW, kRowStep, bg);
  drawText(rgb565, fbW, fbH, x, y, text, fg, bg, false);
}

void drawTextOutlined(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                      std::string_view text, uint16_t fg) {
  drawText(rgb565, fbW, fbH, x + 1, y + 1, text, kColorBlack, kColorBlack,
           true);
  drawText(rgb565, fbW, fbH, x, y, text, fg, kColorBlack, true);
}

void drawStatusTag(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int row,
                   int rightOffset, std::string_view text, uint16_t color) {
  int y = kMargin + row * (kTextHeight + 3);
  drawTextOutlined(rgb565, fbW, fbH, static_cast<int>(fbW) - rightOffset, y,
                   text, color);
}

void drawCenteredBottomHint(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                            std::string_view text) {
  int w = static_cast<int>(text.size()) * kTextWidth;
  drawTextWithBg(rgb565, fbW, fbH, (static_cast<int>(fbW) - w) / 2,
                 static_cast<int>(fbH) - kTextHeight - 4, text, kColorGray,
                 kColorBlack);
}

// Draw 0-5 rating stars at the top-right of the screen.
void drawRatingStars(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                     int rating) {
  if (rating <= 0)
    return;
  rating = std::clamp(rating, 0, 5);
  std::string stars(static_cast<size_t>(rating), '*');
  int starW = static_cast<int>(stars.size()) * kTextWidth;
  drawTextWithBg(rgb565, fbW, fbH,
                 static_cast<int>(fbW) - starW - kMargin,
                 kMargin + kTextHeight + 2, stars, kColorYellow,
                 kColorBlack);
}

int rgb565Luma(const uint8_t *p) {
  uint16_t px = (static_cast<uint16_t>(p[0]) << 8) | p[1];
  uint8_t r5 = (px >> 11) & 0x1F;
  uint8_t g6 = (px >> 5) & 0x3F;
  uint8_t b5 = px & 0x1F;
  return (77 * (r5 << 3) + 150 * (g6 << 2) + 29 * (b5 << 3)) >> 8;
}

void drawExposureInfo(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      uint32_t shutterMs, uint32_t iso) {
  if (shutterMs == 0 && iso == 0)
    return;
  char info[32];
  size_t infoLen = 0;
  if (shutterMs > 0) {
    if (shutterMs >= kMillisPerSec) {
      auto [ptr, ec] =
          std::to_chars(info, info + sizeof(info), shutterMs / kMillisPerSec);
      if (ec == std::errc{}) {
        if (ptr < info + sizeof(info)) {
          *ptr++ = 's';
        }
        infoLen = static_cast<size_t>(ptr - info);
      }
    } else {
      uint32_t denom = (kMillisPerSec + shutterMs / 2u) / shutterMs;
      if (denom <= 1) {
        infoLen = 2;
        info[0] = '1';
        info[1] = 's';
      } else {
        auto [ptr, ec] = std::to_chars(info + 2, info + sizeof(info), denom);
        if (ec == std::errc{}) {
          info[0] = '1';
          info[1] = '/';
          infoLen = static_cast<size_t>(ptr - info);
        }
      }
    }
  }
  if (iso > 0) {
    size_t isoStart = infoLen;
    if (infoLen > 0 && infoLen < sizeof(info))
      info[infoLen++] = ' ';
    if (infoLen + 3 < sizeof(info)) {
      info[infoLen++] = 'I';
      info[infoLen++] = 'S';
      info[infoLen++] = 'O';
      auto [ptr, ec] = std::to_chars(info + infoLen, info + sizeof(info), iso);
      if (ec == std::errc{})
        infoLen = static_cast<size_t>(ptr - info);
      else
        infoLen = isoStart;
    } else {
      infoLen = isoStart;
    }
  }
  if (infoLen > 0) {
    drawTextOutlined(rgb565, fbW, fbH, kMargin, kMargin + kRowStep,
                     std::string_view(info, infoLen), kColorAmber);
  }
}

void drawCaptureCount(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      uint32_t captureCount, bool hasExposure) {
  if (captureCount == 0)
    return;
  int countY = hasExposure ? kMargin + 2 * kRowStep : kMargin + kRowStep;
  char count[16];
  count[0] = 'N';
  auto [ptr, ec] =
      std::to_chars(count + 1, count + sizeof(count), captureCount);
  if (ec == std::errc{}) {
    drawTextOutlined(rgb565, fbW, fbH, kMargin, countY,
                     std::string_view(count, static_cast<size_t>(ptr - count)),
                     kColorAmber);
  }
}

void drawBatteryOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                        const BatteryReading &battery) {
  int iconX = static_cast<int>(fbW) - 22;
  int iconY = kMargin;
  drawBatteryIcon(rgb565, fbW, fbH, iconX, iconY, battery.percent);
  char pctStr[8];
  auto [ptr, ec] =
      std::to_chars(pctStr, pctStr + sizeof(pctStr) - 1, battery.percent);
  size_t pctLen = 0;
  if (ec == std::errc{}) {
    pctLen = static_cast<size_t>(ptr - pctStr);
    if (pctLen < sizeof(pctStr))
      pctStr[pctLen++] = '%';
  }
  drawTextOutlined(
      rgb565, fbW, fbH, iconX - kTextWidth * static_cast<int>(pctLen) - 2,
      iconY + kTextHeight + 3, std::string_view(pctStr, pctLen), kColorAmber);
}

std::string_view fitBasename(std::string_view path, int maxChars) {
  size_t slash = path.rfind('/');
  std::string_view base =
      (slash != std::string_view::npos) ? path.substr(slash + 1) : path;
  if (static_cast<int>(base.size()) > maxChars)
    base = base.substr(0, static_cast<size_t>(maxChars));
  return base;
}

void drawErrorOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const std::string &errorMessage) {
  int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
  std::string msg = errorMessage;
  if (static_cast<int>(msg.size()) > maxChars)
    msg.resize(maxChars);
  int msgW = static_cast<int>(msg.size()) * kTextWidth;
  int msgX = std::max((static_cast<int>(fbW) - msgW) / 2, 0);
  int msgY = static_cast<int>(fbH) - kTextHeight * 2 - 6;
  fillRect(rgb565, fbW, fbH, msgX - 4, msgY - 2, msgW + 8, kTextHeight + 4,
           kColorRed);
  drawText(rgb565, fbW, fbH, msgX, msgY, msg, kColorWhite, kColorRed, false);
}

} // namespace

const char *modeName(CameraMode mode) {
  switch (mode) {
  case CameraMode::Viewfinder:
    return "VF";
  case CameraMode::Review:
    return "REV";
  case CameraMode::Playback:
    return "PLAY";
  case CameraMode::ImageView:
    return "VIEW";
  case CameraMode::Settings:
    return "SET";
  case CameraMode::Splash:
    return "BOOT";
  }
  return "??";
}

void drawOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                 const OverlayState &state) {
  drawTextOutlined(rgb565, fbW, fbH, kMargin, kMargin, modeName(state.mode),
                   kColorAmber);
  drawExposureInfo(rgb565, fbW, fbH, state.shutterMs, state.iso);
  drawCaptureCount(rgb565, fbW, fbH, state.captureCount,
                   state.shutterMs > 0 || state.iso > 0);
  if (state.batteryValid)
    drawBatteryOverlay(rgb565, fbW, fbH, state.battery);
  if (state.settings.gridType != GridType::Off)
    drawGrid(rgb565, fbW, fbH, state.settings.gridType);
  if (state.meteringLocked)
    drawStatusTag(rgb565, fbW, fbH, 2, 30, "AEL", kColorYellow);
  if (state.timelapseRunning)
    drawStatusTag(rgb565, fbW, fbH, 4, 60, "TL RUN", kColorRed);
  if (state.wifiActive)
    drawStatusTag(rgb565, fbW, fbH, 5, 30, "WIFI", kColorGreen);
  if (state.btActive)
    drawStatusTag(rgb565, fbW, fbH, 6, 30, "BT", kColorCyan);
  if (state.focusMagnify > 0)
    drawFocusMagnifyIndicator(rgb565, fbW, fbH, state.focusMagnify);
  if (state.bulbSeconds > 0)
    drawBulbTimer(rgb565, fbW, fbH, state.bulbSeconds);
  if (state.timerRemaining > 0)
    drawTimerCountdown(rgb565, fbW, fbH, state.timerRemaining);
  if (state.captureInProgress)
    drawCaptureIndicator(rgb565, fbW, fbH);
  if (!state.errorMessage.empty())
    drawErrorOverlay(rgb565, fbW, fbH, state.errorMessage);
}

void drawHistogram(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size, const uint8_t *yPlane, uint32_t w,
                   uint32_t h, uint32_t stride, size_t ySize) {
  // Luma-only histogram from the Y plane (cheap on Pi Zero).
  if (!rgb565 || !yPlane || w == 0 || h == 0 || stride < w)
    return;
  size_t needFb = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
      !checkedMul(needFb, 2, needFb))
    return;
  if (needFb > rgb565Size)
    return;
  size_t needed = 0;
  if (!checkedMul(static_cast<size_t>(h), stride, needed))
    return;
  if (ySize < needed)
    return;

  uint32_t hist[kHistogramBins] = {};
  uint32_t sampleCount = 0;
  for (uint32_t y = 0; y < h; y += kHistogramSampleStep) {
    for (uint32_t x = 0; x < w; x += kHistogramSampleStep) {
      uint8_t lum = yPlane[static_cast<size_t>(y) * stride + x];
      hist[lum]++;
      sampleCount++;
    }
  }
  if (sampleCount == 0)
    return;

  uint32_t maxVal = 1;
  for (int i = 1; i < kHistogramBins - 1; ++i) {
    maxVal = std::max(maxVal, hist[i]);
  }

  constexpr int kHistW = 48;
  constexpr int kHistH = 24;
  constexpr int kErrorBannerH = kTextHeight * 2 + 8;
  int histX = static_cast<int>(fbW) - kHistW - kMargin;
  int histY = static_cast<int>(fbH) - kHistH - kErrorBannerH - kMargin;

  fillRect(rgb565, fbW, fbH, histX - 1, histY - 1, kHistW + 2, kHistH + 2,
           kColorBlack);

  for (int col = 0; col < kHistW; ++col) {
    int binStart = (col * kHistogramBins) / kHistW;
    int binEnd = ((col + 1) * kHistogramBins) / kHistW;
    uint32_t sum = 0;
    for (int b = binStart; b < binEnd && b < kHistogramBins; ++b)
      sum += hist[b];
    uint32_t avg = sum / (binEnd - binStart > 0 ? (binEnd - binStart) : 1);
    uint64_t barH64 = (static_cast<uint64_t>(avg) * kHistH) / maxVal;
    int barH =
        static_cast<int>(std::min(barH64, static_cast<uint64_t>(kHistH)));
    for (int dy = 0; dy < barH; ++dy) {
      int px = histX + col;
      int py = histY + kHistH - 1 - dy;
      if (px >= 0 && py >= 0 && static_cast<uint32_t>(px) < fbW &&
          static_cast<uint32_t>(py) < fbH) {
        size_t idx = (static_cast<size_t>(py) * fbW + px) * 2;
        rgb565[idx] = static_cast<uint8_t>(kColorYellow >> 8);
        rgb565[idx + 1] = static_cast<uint8_t>(kColorYellow & 0xFF);
      }
    }
  }
}

void drawSplash(uint8_t *rgb565, uint32_t fbW, uint32_t fbH) {
  fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);

  std::string logo = "PICAM";
  int logoW = static_cast<int>(logo.size()) * kTextWidth;
  int x = (static_cast<int>(fbW) - logoW) / 2;
  int y = (static_cast<int>(fbH) - kTextHeight) / 2 - kTextHeight;
  drawText(rgb565, fbW, fbH, x, y, logo, kColorWhite, kColorBlack, true);

  std::string ver = "V1.0";
  int verW = static_cast<int>(ver.size()) * kTextWidth;
  drawText(rgb565, fbW, fbH, (static_cast<int>(fbW) - verW) / 2,
           y + kTextHeight + 4, ver, kColorGray, kColorBlack, true);
}

void drawCaptureIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH) {
  rectOutline(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorRed);
  std::string text = "SAVING";
  int textW = static_cast<int>(text.size()) * kTextWidth;
  drawTextWithBg(rgb565, fbW, fbH, (static_cast<int>(fbW) - textW) / 2,
                 static_cast<int>(fbH) - kTextHeight - 4, text, kColorRed,
                 kColorBlack);
}

void drawTimerCountdown(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                        uint32_t seconds) {
  std::string num = std::to_string(seconds);
  constexpr int kScale = 3;
  int charW = kTextWidth * kScale;
  int charH = kTextHeight * kScale;
  int totalW = static_cast<int>(num.size()) * charW;
  int x = (static_cast<int>(fbW) - totalW) / 2;
  int y = (static_cast<int>(fbH) - charH) / 2;

  fillRect(rgb565, fbW, fbH, x - 6, y - 6, totalW + 12, charH + 12,
           kColorBlack);
  for (size_t i = 0; i < num.size(); ++i) {
    drawCharScaled(rgb565, fbW, fbH, x + static_cast<int>(i) * charW, y, num[i],
                   kScale, kColorYellow, kColorBlack, false);
  }
}

void drawGrid(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, GridType type) {
  if (type == GridType::Off)
    return;
  uint16_t color = kColorGrid;
  int w = static_cast<int>(fbW);
  int h = static_cast<int>(fbH);
  if (type == GridType::Thirds) {
    vLine(rgb565, fbW, fbH, w / 3, 0, h, color);
    vLine(rgb565, fbW, fbH, 2 * w / 3, 0, h, color);
    hLine(rgb565, fbW, fbH, 0, h / 3, w, color);
    hLine(rgb565, fbW, fbH, 0, 2 * h / 3, w, color);
  } else if (type == GridType::Square) {
    for (int i = 1; i < 4; ++i) {
      vLine(rgb565, fbW, fbH, w * i / 4, 0, h, color);
      hLine(rgb565, fbW, fbH, 0, h * i / 4, w, color);
    }
  } else if (type == GridType::Diagonal) {
    for (int i = 0; i < w && i < h; ++i) {
      size_t idx = (static_cast<size_t>(i) * fbW + i) * 2;
      rgb565[idx] = static_cast<uint8_t>(color >> 8);
      rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF);
      size_t idx2 = (static_cast<size_t>(i) * fbW + (w - 1 - i)) * 2;
      rgb565[idx2] = static_cast<uint8_t>(color >> 8);
      rgb565[idx2 + 1] = static_cast<uint8_t>(color & 0xFF);
    }
  } else if (type == GridType::GoldenRatio) {
    int g1 = static_cast<int>(w * 0.382f);
    int g2 = static_cast<int>(w * 0.618f);
    vLine(rgb565, fbW, fbH, g1, 0, h, color);
    vLine(rgb565, fbW, fbH, g2, 0, h, color);
    int gh1 = static_cast<int>(h * 0.382f);
    int gh2 = static_cast<int>(h * 0.618f);
    hLine(rgb565, fbW, fbH, 0, gh1, w, color);
    hLine(rgb565, fbW, fbH, 0, gh2, w, color);
  }
}

void drawReviewScreen(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size, const std::string &path,
                      const uint8_t *reviewPixels, size_t reviewSize) {
  size_t totalBytes = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, totalBytes) ||
      !checkedMul(totalBytes, 2, totalBytes))
    return;
  if (totalBytes > rgb565Size)
    return;
  if (reviewPixels && reviewSize >= totalBytes) {
    std::memcpy(rgb565, reviewPixels, totalBytes);
  } else {
    fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);
    std::string header = "SAVED";
    int headerW = static_cast<int>(header.size()) * kTextWidth;
    drawText(rgb565, fbW, fbH, (static_cast<int>(fbW) - headerW) / 2,
             (static_cast<int>(fbH) - kTextHeight) / 2, header, kColorGreen,
             kColorBlack, true);
  }

  int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
  drawTextWithBg(rgb565, fbW, fbH, kMargin, kMargin,
                 fitBasename(path, maxChars), kColorWhite, kColorBlack);

  std::string footer = "OK=CONT";
  int footerW = static_cast<int>(footer.size()) * kTextWidth;
  drawTextWithBg(rgb565, fbW, fbH, (static_cast<int>(fbW) - footerW) / 2,
                 static_cast<int>(fbH) - kTextHeight - 4, footer, kColorGreen,
                 kColorBlack);
}

void drawSettingsMenu(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const CameraSettings &settings, SettingsTab tab,
                      int selectedItem) {
  fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);

  if (settings.menuMode == MenuMode::Basic) {
    // Basic mode: single flat list, no tab bar. A simple "BASIC" header.
    std::string_view header = "BASIC";
    int headerW = static_cast<int>(header.size()) * kTextWidth;
    fillRect(rgb565, fbW, fbH, 0, 0, fbW, kTextHeight + 4, kColorAmber);
    drawText(rgb565, fbW, fbH,
             (static_cast<int>(fbW) - headerW) / 2, 2,
             std::string(header), kColorBlack, kColorAmber, false);

    int itemCount = basicMenuItemCount();
    constexpr int kMenuRowH = kTextHeight + 5;
    int y = kTextHeight + 6;
    int maxVisible = (static_cast<int>(fbH) - y - 4) / kMenuRowH;
    int visible = std::max(0, std::min(itemCount, maxVisible));
    int maxScroll = std::max(0, itemCount - visible);
    int scrollOffset = std::clamp(selectedItem, 0, maxScroll);

    for (int i = 0; i < visible; ++i) {
      int idx = scrollOffset + i;
      bool selected = (idx == selectedItem);
      uint16_t fg = selected ? kColorBlack : kColorWhite;
      uint16_t bg = selected ? kColorGreen : kColorBlack;

      std::string label = std::string(basicMenuItemLabel(idx));
      std::string value = basicMenuItemValue(idx, settings);

      std::string line = label;
      if (!value.empty()) {
        int totalChars = (static_cast<int>(fbW) - 4) / kTextWidth;
        int labelChars = static_cast<int>(label.size());
        int valueChars = static_cast<int>(value.size());
        int spaces = totalChars - labelChars - valueChars;
        spaces = std::max(spaces, 1);
        line += std::string(spaces, ' ');
        line += value;
      } else if (idx == selectedItem) {
        line += " >";
      }

      int textY = y + (kMenuRowH - kTextHeight) / 2;
      fillRect(rgb565, fbW, fbH, 0, y, fbW, kMenuRowH, bg);
      drawText(rgb565, fbW, fbH, kMargin + 2, textY, line, fg, bg, false);
      y += kMenuRowH;
    }
    return;
  }

  // Advanced mode: tabbed menu.
  static constexpr std::string_view kTabNames[] = {
      "CAP", "EXP", "COL", "DISP", "VID", "SYS"};
  constexpr int kTabCount = static_cast<int>(std::size(kTabNames));
  int tabWidth = static_cast<int>(fbW) / kTabCount;
  for (int i = 0; i < kTabCount; ++i) {
    bool active = (i == static_cast<int>(tab));
    uint16_t fg = active ? kColorBlack : kColorGray;
    uint16_t bg = active ? kColorAmber : kColorBlack;
    int tx =
        i * tabWidth +
        (tabWidth - static_cast<int>(kTabNames[i].size()) * kTextWidth) / 2;
    fillRect(rgb565, fbW, fbH, i * tabWidth, 0, tabWidth, kTextHeight + 4, bg);
    drawText(rgb565, fbW, fbH, tx, 2, std::string(kTabNames[i]), fg, bg, false);
  }

  int itemCount = settingsTabItemCount(tab);
  constexpr int kMenuRowH = kTextHeight + 5;
  int y = kTextHeight + 6;
  int maxVisible = (static_cast<int>(fbH) - y - 4) / kMenuRowH;
  int visible = std::max(0, std::min(itemCount, maxVisible));
  int maxScroll = std::max(0, itemCount - visible);
  int scrollOffset = std::clamp(selectedItem, 0, maxScroll);

  for (int i = 0; i < visible; ++i) {
    int idx = scrollOffset + i;
    bool selected = (idx == selectedItem);
    uint16_t fg = selected ? kColorBlack : kColorWhite;
    uint16_t bg = selected ? kColorGreen : kColorBlack;

    std::string label = std::string(settingsItemLabel(tab, idx));
    std::string value = settingsItemValue(tab, idx, settings);

    std::string line = label;
    if (!value.empty()) {
      int totalChars = (static_cast<int>(fbW) - 4) / kTextWidth;
      int labelChars = static_cast<int>(label.size());
      int valueChars = static_cast<int>(value.size());
      int spaces = totalChars - labelChars - valueChars;
      spaces = std::max(spaces, 1);
      line += std::string(spaces, ' ');
      line += value;
    }

    int textY = y + (kMenuRowH - kTextHeight) / 2;
    fillRect(rgb565, fbW, fbH, 0, y, fbW, kMenuRowH, bg);
    drawText(rgb565, fbW, fbH, kMargin + 2, textY, line, fg, bg, false);
    y += kMenuRowH;
  }
}

void drawPlaybackBrowser(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         const std::vector<std::string> &files, int selectedIdx,
                         int &scrollOffset) {
  fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);

  std::string title = "PLAYBACK";
  int titleW = static_cast<int>(title.size()) * kTextWidth;
  drawText(rgb565, fbW, fbH, (static_cast<int>(fbW) - titleW) / 2, kMargin,
           title, kColorWhite, kColorBlack, true);

  if (files.empty()) {
    std::string empty = "NO IMAGES";
    int emptyW = static_cast<int>(empty.size()) * kTextWidth;
    drawText(rgb565, fbW, fbH, (static_cast<int>(fbW) - emptyW) / 2,
             (static_cast<int>(fbH) - kTextHeight) / 2, empty, kColorGray,
             kColorBlack, true);
    return;
  }

  constexpr int kMenuRowH = kTextHeight + 5;
  int y = kMargin + kTextHeight + 6;
  int maxVisible = (static_cast<int>(fbH) - y - 4) / kMenuRowH;
  int maxChars = std::max(0, (static_cast<int>(fbW) - 4) / kTextWidth);

  maxVisible = std::max(0, maxVisible);
  int maxScroll = std::max(0, static_cast<int>(files.size()) - maxVisible);
  scrollOffset = std::clamp(scrollOffset, 0, maxScroll);

  for (int row = 0; row < maxVisible; ++row) {
    int fileIdx = scrollOffset + row;
    if (fileIdx >= static_cast<int>(files.size()))
      break;

    std::string_view name = fitBasename(files[fileIdx], maxChars);

    bool selected = (fileIdx == selectedIdx);
    uint16_t fg = selected ? kColorBlack : kColorWhite;
    uint16_t bg = selected ? kColorGreen : kColorBlack;
    int textY = y + (kMenuRowH - kTextHeight) / 2;
    fillRect(rgb565, fbW, fbH, 0, y, fbW, kMenuRowH, bg);
    drawText(rgb565, fbW, fbH, kMargin + 2, textY, name, fg, bg, false);
    y += kMenuRowH;
  }

  std::string counter =
      std::to_string(selectedIdx + 1) + "/" + std::to_string(files.size());
  int counterW = static_cast<int>(counter.size()) * kTextWidth;
  drawText(rgb565, fbW, fbH, (static_cast<int>(fbW) - counterW) / 2,
           static_cast<int>(fbH) - kTextHeight - 2, counter, kColorGray,
           kColorBlack, true);
}

void drawImageView(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size, const uint8_t *imageRgb565,
                   size_t imageSize, const std::string &path,
                   bool rotateTall, int rating) {
  size_t totalBytes = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, totalBytes) ||
      !checkedMul(totalBytes, 2, totalBytes))
    return;
  if (totalBytes > rgb565Size)
    return;
  // Rotate portrait RGB565 90° CW if requested.
  std::vector<uint8_t> rotated;
  if (rotateTall && imageRgb565 && imageSize >= totalBytes) {
    if (isPortrait(fbW, fbH)) {
      rotated = rotateRgb565Cw(imageRgb565, fbW, fbH);
      if (rotated.size() >= totalBytes)
        imageRgb565 = rotated.data();
    }
  }
  if (imageSize >= totalBytes && imageRgb565) {
    std::memcpy(rgb565, imageRgb565, totalBytes);
  } else {
    std::memset(rgb565, 0, totalBytes);
  }

  int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
  drawTextWithBg(rgb565, fbW, fbH, kMargin, kMargin,
                 fitBasename(path, maxChars), kColorWhite, kColorBlack);

  if (imageSize >= totalBytes && imageRgb565)
    drawImageViewHistogramAndBlinkies(rgb565, fbW, fbH, rgb565Size);

  drawRatingStars(rgb565, fbW, fbH, rating);
  drawCenteredBottomHint(rgb565, fbW, fbH, "OK=BACK K1=LOCK K3=DEL");
}

void drawImageViewZoomed(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         size_t rgb565Size, const uint8_t *imageRgb565,
                         size_t imageSize, uint32_t imageW, uint32_t imageH,
                         int zoom, int panX, int panY,
                         const std::string &path, bool rotateTall,
                         int rating) {
  size_t totalBytes = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, totalBytes) ||
      !checkedMul(totalBytes, 2, totalBytes))
    return;
  if (totalBytes > rgb565Size)
    return;

  zoom = std::max(zoom, 1);
  zoom = std::min(zoom, 4);

  std::vector<uint8_t> rotated;
  if (rotateTall && imageRgb565 && imageW > 0 && imageH > 0 &&
      isPortrait(imageW, imageH)) {
    size_t needSrc = 0;
    if (checkedMul(static_cast<size_t>(imageW), imageH, needSrc) &&
        checkedMul(needSrc, 2, needSrc) && imageSize >= needSrc) {
      rotated = rotateRgb565Cw(imageRgb565, imageW, imageH);
      if (rotated.size() >= needSrc) {
        imageRgb565 = rotated.data();
        std::swap(imageW, imageH);
      }
    }
  }

  if (imageSize >= totalBytes && imageRgb565 && imageW > 0 && imageH > 0) {
    if (zoom == 1) {
      std::memcpy(rgb565, imageRgb565, totalBytes);
    } else {
      uint32_t srcRegionW = fbW / static_cast<uint32_t>(zoom);
      uint32_t srcRegionH = fbH / static_cast<uint32_t>(zoom);
      panX = std::max(panX, 0);
      panY = std::max(panY, 0);
      if (panX + srcRegionW > imageW)
        panX = static_cast<int>(imageW - srcRegionW);
      if (panY + srcRegionH > imageH)
        panY = static_cast<int>(imageH - srcRegionH);
      panX = std::max(panX, 0);
      panY = std::max(panY, 0);

      for (uint32_t dy = 0; dy < fbH; ++dy) {
        uint32_t sy =
            static_cast<uint32_t>(panY) + dy / static_cast<uint32_t>(zoom);
        if (sy >= imageH)
          sy = imageH - 1;
        for (uint32_t dx = 0; dx < fbW; ++dx) {
          uint32_t sx =
              static_cast<uint32_t>(panX) + dx / static_cast<uint32_t>(zoom);
          if (sx >= imageW)
            sx = imageW - 1;
          size_t srcIdx = (static_cast<size_t>(sy) * imageW + sx) * 2;
          size_t dstIdx = (static_cast<size_t>(dy) * fbW + dx) * 2;
          if (srcIdx + 1 < imageSize && dstIdx + 1 < totalBytes) {
            rgb565[dstIdx] = imageRgb565[srcIdx];
            rgb565[dstIdx + 1] = imageRgb565[srcIdx + 1];
          }
        }
      }
    }
  } else {
    std::memset(rgb565, 0, totalBytes);
  }

  int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
  drawTextWithBg(rgb565, fbW, fbH, kMargin, kMargin,
                 fitBasename(path, maxChars), kColorWhite, kColorBlack);

  if (zoom > 1) {
    std::string zoomStr = std::to_string(zoom) + "X";
    int zoomW = static_cast<int>(zoomStr.size()) * kTextWidth;
    drawTextWithBg(rgb565, fbW, fbH, static_cast<int>(fbW) - zoomW - kMargin,
                   kMargin, zoomStr, kColorYellow, kColorBlack);
  }

  if (zoom == 1 && imageSize >= totalBytes && imageRgb565)
    drawImageViewHistogramAndBlinkies(rgb565, fbW, fbH, rgb565Size);

  drawRatingStars(rgb565, fbW, fbH, rating);
  drawCenteredBottomHint(rgb565, fbW, fbH,
                         zoom > 1 ? "OK=BACK K1=LOCK K3=DEL JOY=PAN"
                                  : "OK=BACK K1=LOCK K3=DEL");
}

void drawZebra(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, size_t rgb565Size,
               const uint8_t *yPlane, uint32_t w, uint32_t h, uint32_t stride,
               size_t ySize, uint8_t threshold) {
  if (!yPlane || !rgb565)
    return;
  if (w == 0 || h == 0)
    return;
  if (stride < w)
    return;
  size_t needY = 0;
  if (!checkedMul(static_cast<size_t>(stride), h, needY))
    return;
  if (ySize < needY)
    return;
  size_t needFb = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
      !checkedMul(needFb, 2, needFb))
    return;
  if (needFb > rgb565Size)
    return;

  for (uint32_t dy = 0; dy < fbH; ++dy) {
    uint32_t sy = (static_cast<uint64_t>(dy) * h) / fbH;
    if (sy >= h)
      sy = h - 1;
    for (uint32_t dx = 0; dx < fbW; ++dx) {
      uint32_t sx = (static_cast<uint64_t>(dx) * w) / fbW;
      if (sx >= w)
        sx = w - 1;
      uint8_t y = yPlane[static_cast<size_t>(sy) * stride + sx];
      if (y >= threshold) {
        bool stripe = ((dx + dy) / kZebraStripePeriod) % 2 == 0;
        uint16_t color = stripe ? kColorWhite : kColorBlack;
        size_t idx = (static_cast<size_t>(dy) * fbW + dx) * 2;
        rgb565[idx] = static_cast<uint8_t>(color >> 8);
        rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF);
      }
    }
  }
}

void drawFocusPeaking(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size, const uint8_t *yPlane, uint32_t w,
                      uint32_t h, uint32_t stride, size_t ySize) {
  if (!yPlane || !rgb565)
    return;
  if (w < 2 || h == 0)
    return;
  if (stride < w)
    return;
  size_t needY = 0;
  if (!checkedMul(static_cast<size_t>(stride), h, needY))
    return;
  if (ySize < needY)
    return;
  size_t needFb = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
      !checkedMul(needFb, 2, needFb))
    return;
  if (needFb > rgb565Size)
    return;

  constexpr uint8_t kEdgeThreshold = 40;
  const uint16_t kPeakColor = ::picamera::rgb565(0, 255, 0);

  for (uint32_t dy = 0; dy < fbH; ++dy) {
    uint32_t sy = (static_cast<uint64_t>(dy) * h) / fbH;
    if (sy >= h)
      sy = h - 1;
    for (uint32_t dx = 0; dx < fbW; ++dx) {
      uint32_t sx = (static_cast<uint64_t>(dx) * w) / fbW;
      if (sx >= w - 1)
        sx = w - 2;
      size_t yIdx = static_cast<size_t>(sy) * stride + sx;
      int16_t diff = static_cast<int16_t>(yPlane[yIdx]) -
                     static_cast<int16_t>(yPlane[yIdx + 1]);
      if (std::abs(diff) >= kEdgeThreshold) {
        size_t idx = (static_cast<size_t>(dy) * fbW + dx) * 2;
        rgb565[idx] = static_cast<uint8_t>(kPeakColor >> 8);
        rgb565[idx + 1] = static_cast<uint8_t>(kPeakColor & 0xFF);
      }
    }
  }
}

uint8_t zebraThreshold(ZebraMode mode) {
  switch (mode) {
  case ZebraMode::Off:
    return 0;
  case ZebraMode::Threshold70:
    return 178; // ~0.70 * 255
  case ZebraMode::Threshold80:
    return 204; // ~0.80 * 255
  case ZebraMode::Threshold100:
    return 255;
  }
  return 0;
}

bool computeWbGainsFromNv12(const uint8_t *uv, uint32_t w, uint32_t h,
                            size_t uvSize, float &outRed, float &outBlue) {
  if (!uv || w < 2 || h < 2)
    return false;
  // NV12 chroma plane is 4:2:0: (w/2)*(h/2) interleaved Cb,Cr byte pairs.
  uint32_t cw = w / 2;
  uint32_t ch = h / 2;
  size_t need = 0;
  if (!checkedMul(static_cast<size_t>(cw), ch, need))
    return false;
  if (!checkedMul(need, 2, need))
    return false;
  if (uvSize < need)
    return false;

  // Average Cb (blue-diff, even offsets) and Cr (red-diff, odd offsets).
  // For neutral gray both average 128; gain = 128 / avg neutralizes.
  uint64_t sumCb = 0;
  uint64_t sumCr = 0;
  uint64_t samples = 0;
  for (uint32_t y = 0; y < ch; ++y) {
    const uint8_t *row = uv + static_cast<size_t>(y) * cw * 2;
    for (uint32_t x = 0; x < cw; ++x) {
      sumCb += row[static_cast<size_t>(x) * 2];
      sumCr += row[static_cast<size_t>(x) * 2 + 1];
      ++samples;
    }
  }
  if (samples == 0)
    return false;
  float avgCb = static_cast<float>(sumCb) / static_cast<float>(samples);
  float avgCr = static_cast<float>(sumCr) / static_cast<float>(samples);
  avgCb = std::max(avgCb, 1.0f);
  avgCr = std::max(avgCr, 1.0f);
  float red = 128.0f / avgCr;
  float blue = 128.0f / avgCb;
  outRed = std::clamp(red, 0.1f, 8.0f);
  outBlue = std::clamp(blue, 0.1f, 8.0f);
  return true;
}

void drawBulbTimer(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   uint32_t seconds) {
  if (!rgb565 || fbW == 0 || fbH == 0)
    return;
  std::string label = "BULB " + std::to_string(seconds) + "s";
  int textW = static_cast<int>(label.size()) * kTextWidth;
  int x = (static_cast<int>(fbW) - textW) / 2;
  int y = static_cast<int>(fbH) / 2 - kTextHeight - 2;
  drawTextWithBg(rgb565, fbW, fbH, x, y, label, kColorYellow, kColorBlack);
}

void drawAspectRatioMask(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                         AspectRatio ratio) {
  if (ratio == AspectRatio::Native)
    return;
  // Display is square (128x128) — mask black bars to crop to target aspect.
  float target = 1.0f;
  switch (ratio) {
  case AspectRatio::Ratio43:
    target = 4.0f / 3.0f;
    break;
  case AspectRatio::Ratio169:
    target = 16.0f / 9.0f;
    break;
  case AspectRatio::Ratio11:
    target = 1.0f;
    break;
  default:
    break;
  }
  float dispAspect = static_cast<float>(fbW) / fbH;
  uint32_t maskW = fbW;
  uint32_t maskH = fbH;
  if (target > dispAspect) {
    maskH = static_cast<uint32_t>(fbW / target);
  } else {
    maskW = static_cast<uint32_t>(fbH * target);
  }
  maskW = std::min(maskW, fbW);
  maskH = std::min(maskH, fbH);
  uint32_t barW = (fbW - maskW) / 2;
  uint32_t barH = (fbH - maskH) / 2;
  if (barW > 0) {
    fillRect(rgb565, fbW, fbH, 0, 0, barW, fbH, kColorBlack);
    fillRect(rgb565, fbW, fbH, fbW - barW, 0, barW, fbH, kColorBlack);
  }
  if (barH > 0) {
    fillRect(rgb565, fbW, fbH, 0, 0, fbW, barH, kColorBlack);
    fillRect(rgb565, fbW, fbH, 0, fbH - barH, fbW, barH, kColorBlack);
  }
}

void drawFocusMagnifyIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                               int magnify) {
  if (magnify == 0)
    return;
  std::string text = magnify >= 4 ? "4x" : "2x";
  int textW = static_cast<int>(text.size()) * kTextWidth;
  int x = static_cast<int>(fbW) - textW - kMargin - 2;
  int y = kMargin + 3 * (kTextHeight + 3);
  drawTextOutlined(rgb565, fbW, fbH, x, y, text, kColorYellow);
}

void drawImageViewHistogramAndBlinkies(uint8_t *rgb565, uint32_t fbW,
                                       uint32_t fbH, size_t rgb565Size) {
  // Histogram from 128×128 FB, not full-res image.
  size_t needFb = 0;
  if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
      !checkedMul(needFb, 2, needFb))
    return;
  if (needFb > rgb565Size)
    return;

  constexpr int kHistBins = 256;
  uint32_t hist[kHistBins] = {};
  uint32_t sampleCount = 0;
  for (uint32_t y = 0; y < fbH; y += 4) {
    for (uint32_t x = 0; x < fbW; x += 4) {
      size_t idx = (static_cast<size_t>(y) * fbW + x) * 2;
      int lum = std::min(rgb565Luma(rgb565 + idx), 255);
      hist[static_cast<uint8_t>(lum)]++;
      sampleCount++;
    }
  }
  if (sampleCount == 0)
    return;

  for (uint32_t y = 0; y < fbH; y += 2) {
    for (uint32_t x = 0; x < fbW; x += 2) {
      size_t idx = (static_cast<size_t>(y) * fbW + x) * 2;
      if (rgb565Luma(rgb565 + idx) >= 250)
        fillRect(rgb565, fbW, fbH, x, y, 2, 2, kColorRed);
    }
  }

  uint32_t maxVal = 1;
  for (int i = 1; i < kHistBins - 1; ++i)
    maxVal = std::max(maxVal, hist[i]);
  constexpr int kHistW = 64;
  constexpr int kHistH = 20;
  int histX = (static_cast<int>(fbW) - kHistW) / 2;
  int histY = static_cast<int>(fbH) - kHistH - kTextHeight - 6;
  fillRect(rgb565, fbW, fbH, histX - 1, histY - 1, kHistW + 2, kHistH + 2,
           kColorBlack);
  for (int col = 0; col < kHistW; ++col) {
    int binStart = (col * kHistBins) / kHistW;
    int binEnd = ((col + 1) * kHistBins) / kHistW;
    uint32_t sum = 0;
    for (int b = binStart; b < binEnd && b < kHistBins; ++b)
      sum += hist[b];
    uint32_t avg = sum / (binEnd - binStart > 0 ? (binEnd - binStart) : 1);
    uint64_t barH64 = (static_cast<uint64_t>(avg) * kHistH) / maxVal;
    int barH =
        static_cast<int>(std::min(barH64, static_cast<uint64_t>(kHistH)));
    for (int dy = 0; dy < barH; ++dy) {
      int px = histX + col;
      int py = histY + kHistH - 1 - dy;
      if (px >= 0 && py >= 0 && static_cast<uint32_t>(px) < fbW &&
          static_cast<uint32_t>(py) < fbH) {
        size_t idx = (static_cast<size_t>(py) * fbW + px) * 2;
        rgb565[idx] = static_cast<uint8_t>(kColorYellow >> 8);
        rgb565[idx + 1] = static_cast<uint8_t>(kColorYellow & 0xFF);
      }
    }
  }
}

void drawProtectionIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH) {
  if (!rgb565 || fbW == 0 || fbH == 0)
    return;
  std::string text = "PROT";
  int textW = static_cast<int>(text.size()) * kTextWidth;
  int x = static_cast<int>(fbW) - textW - kMargin - 2;
  int y = kMargin + 2 * (kTextHeight + 3);
  drawTextOutlined(rgb565, fbW, fbH, x, y, text, kColorAmber);
}

void drawCopyrightEditOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                              const std::string &buf, int cursor) {
  int boxY = static_cast<int>(fbH) / 2 - kRowStep * 2;
  int boxH = kRowStep * 4;
  fillRect(rgb565, fbW, fbH, 0, boxY, static_cast<int>(fbW), boxH,
           kColorBlack);
  rectOutline(rgb565, fbW, fbH, 0, boxY, static_cast<int>(fbW), boxH,
              kColorWhite);
  drawTextWithBg(rgb565, fbW, fbH, kMargin, boxY + 2, "COPYRIGHT",
                 kColorYellow, kColorBlack);
  int textY = boxY + kRowStep + 2;
  std::string display = buf;
  while (display.size() < 20)
    display += ' ';
  drawTextWithBg(rgb565, fbW, fbH, kMargin, textY, display, kColorWhite,
                 kColorBlack);
  if (cursor >= 0 && cursor < static_cast<int>(display.size())) {
    int cx = kMargin + cursor * kTextWidth;
    fillRect(rgb565, fbW, fbH, cx - 1, textY - 1, kTextWidth + 2, kRowStep,
             kColorWhite);
    drawChar(rgb565, fbW, fbH, cx, textY, display[cursor], kColorBlack,
             kColorWhite, false);
  }
  drawTextWithBg(rgb565, fbW, fbH, kMargin, textY + kRowStep,
                 "U/D=CHAR L/R=MOVE OK=SAVE", kColorGray, kColorBlack);
}

void drawQuickFnOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                        const std::vector<std::string> &labels,
                        const std::vector<std::string> &values,
                        int selectedIdx) {
  if (labels.empty())
    return;
  int count = static_cast<int>(std::min(labels.size(), values.size()));
  int rowH = kRowStep;
  int boxH = rowH * count + kRowStep + 4;
  int boxY = static_cast<int>(fbH) / 2 - boxH / 2;
  int boxW = static_cast<int>(fbW) - 2 * kMargin;
  int boxX = kMargin;
  fillRect(rgb565, fbW, fbH, boxX, boxY, boxW, boxH, kColorBlack);
  rectOutline(rgb565, fbW, fbH, boxX, boxY, boxW, boxH, kColorWhite);
  // Title
  drawTextWithBg(rgb565, fbW, fbH, boxX + 2, boxY + 2, "FN", kColorYellow,
                 kColorBlack);
  // Items
  for (int i = 0; i < count; ++i) {
    int y = boxY + kRowStep + 2 + i * rowH;
    uint16_t fg = (i == selectedIdx) ? kColorBlack : kColorWhite;
    uint16_t bg = (i == selectedIdx) ? kColorGreen : kColorBlack;
    std::string line = labels[i] + " " + values[i];
    drawTextWithBg(rgb565, fbW, fbH, boxX + 2, y, line, fg, bg);
  }
}

void drawRecIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      uint32_t seconds) {
  fillRect(rgb565, fbW, fbH, kMargin, kMargin, 6, 6, kColorRed);
  char timer[16];
  uint32_t mm = seconds / 60;
  uint32_t ss = seconds % 60;
  std::snprintf(timer, sizeof(timer), "%02u:%02u", mm, ss);
  drawTextWithBg(rgb565, fbW, fbH, kMargin + 10, kMargin, timer,
                 kColorRed, kColorBlack);
}

} // namespace picamera
