#include "camera_mode.h"
#include "font.h"
#include "util.h"
#include "settings_menu.h"

#include <algorithm>
#include <charconv>
#include <cmath>
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
constexpr int kHistogramSampleStep = 4;
constexpr int kZebraStripePeriod = 4;
constexpr uint32_t kMillisPerSec = 1000;
constexpr int kHistogramBins = 256;

void fillRect(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
              int x, int y, int w, int h, uint16_t color) {
    size_t fbBytes = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, fbBytes) ||
        !checkedMul(fbBytes, 2, fbBytes))
        return;
    for (int dy = 0; dy < h; ++dy) {
        for (int dx = 0; dx < w; ++dx) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && py >= 0 &&
                static_cast<uint32_t>(px) < fbW &&
                static_cast<uint32_t>(py) < fbH) {
                size_t idx = (static_cast<size_t>(py) * fbW + px) * 2;
                rgb565[idx] = static_cast<uint8_t>(color >> 8);
                rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF);
            }
        }
    }
}

void hLine(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
           int x, int y, int len, uint16_t color) {
    fillRect(rgb565, fbW, fbH, x, y, len, 1, color);
}

void vLine(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
           int x, int y, int len, uint16_t color) {
    fillRect(rgb565, fbW, fbH, x, y, 1, len, color);
}

void rectOutline(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                 int x, int y, int w, int h, uint16_t color) {
    hLine(rgb565, fbW, fbH, x, y, w, color);
    hLine(rgb565, fbW, fbH, x, y + h - 1, w, color);
    vLine(rgb565, fbW, fbH, x, y, h, color);
    vLine(rgb565, fbW, fbH, x + w - 1, y, h, color);
}

void drawTextWithBg(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                    int x, int y, std::string_view text,
                    uint16_t fg, uint16_t bg) {
    if (x < 1 || y < 1) return;
    if (static_cast<uint32_t>(x) >= fbW || static_cast<uint32_t>(y) >= fbH)
        return;
    size_t textW = 0;
    if (!checkedMul(text.size(), static_cast<size_t>(kTextWidth), textW))
        textW = static_cast<size_t>(fbW);
    int availW = static_cast<int>(fbW) - (x - 1);
    int bgW = static_cast<int>(std::min(textW + 2, static_cast<size_t>(std::max(0, availW))));
    if (bgW <= 0) return;
    fillRect(rgb565, fbW, fbH, x - 1, y - 1, bgW, kRowStep, bg);
    drawText(rgb565, fbW, fbH, x, y, text, fg, bg, false);
}

void drawTextOutlined(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      int x, int y, std::string_view text, uint16_t fg) {
    drawText(rgb565, fbW, fbH, x + 1, y + 1, text, kColorBlack, kColorBlack, true);
    drawText(rgb565, fbW, fbH, x, y, text, fg, kColorBlack, true);
}

void drawExposureInfo(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      uint32_t shutterMs, uint32_t iso) {
    if (shutterMs == 0 && iso == 0) return;
    char info[32];
    size_t infoLen = 0;
    if (shutterMs > 0) {
        if (shutterMs >= kMillisPerSec) {
            auto [ptr, ec] = std::to_chars(info, info + sizeof(info),
                                           shutterMs / kMillisPerSec);
            if (ec == std::errc{}) {
                if (ptr < info + sizeof(info)) { *ptr++ = 's'; }
                infoLen = static_cast<size_t>(ptr - info);
            }
        } else {
            uint32_t denom = (kMillisPerSec + shutterMs / 2u) / shutterMs;
            if (denom <= 1) {
                infoLen = 2;
                info[0] = '1'; info[1] = 's';
            } else {
                auto [ptr, ec] = std::to_chars(
                    info + 2, info + sizeof(info), denom);
                if (ec == std::errc{}) {
                    info[0] = '1'; info[1] = '/';
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
            auto [ptr, ec] = std::to_chars(
                info + infoLen, info + sizeof(info), iso);
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
    if (captureCount == 0) return;
    int countY = hasExposure ? kMargin + 2 * kRowStep : kMargin + kRowStep;
    char count[16];
    count[0] = 'N';
    auto [ptr, ec] = std::to_chars(count + 1, count + sizeof(count), captureCount);
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
    auto [ptr, ec] = std::to_chars(pctStr, pctStr + sizeof(pctStr) - 1,
                                   battery.percent);
    size_t pctLen = 0;
    if (ec == std::errc{}) {
        pctLen = static_cast<size_t>(ptr - pctStr);
        if (pctLen < sizeof(pctStr)) pctStr[pctLen++] = '%';
    }
    drawTextOutlined(rgb565, fbW, fbH,
                     iconX - kTextWidth * static_cast<int>(pctLen) - 2,
                     iconY + kTextHeight + 3,
                     std::string_view(pctStr, pctLen), kColorAmber);
}

std::string_view fitBasename(std::string_view path, int maxChars) {
    size_t slash = path.rfind('/');
    std::string_view base = (slash != std::string_view::npos)
        ? path.substr(slash + 1) : path;
    if (static_cast<int>(base.size()) > maxChars)
        base = base.substr(0, static_cast<size_t>(maxChars));
    return base;
}

void drawErrorOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const std::string &errorMessage) {
    int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
    std::string msg = errorMessage;
    if (static_cast<int>(msg.size()) > maxChars) msg.resize(maxChars);
    int msgW = static_cast<int>(msg.size()) * kTextWidth;
    int msgX = std::max((static_cast<int>(fbW) - msgW) / 2, 0);
    int msgY = static_cast<int>(fbH) - kTextHeight * 2 - 6;
    fillRect(rgb565, fbW, fbH, msgX - 4, msgY - 2, msgW + 8, kTextHeight + 4, kColorRed);
    drawText(rgb565, fbW, fbH, msgX, msgY, msg, kColorWhite, kColorRed, false);
}

}

const char *modeName(CameraMode mode) {
    switch (mode) {
        case CameraMode::Viewfinder: return "VF";
        case CameraMode::Review:     return "REV";
        case CameraMode::Playback:   return "PLAY";
        case CameraMode::ImageView:  return "VIEW";
        case CameraMode::Settings:   return "SET";
        case CameraMode::Splash:     return "BOOT";
    }
    return "??";
}

void drawOverlay(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                 const OverlayState &state) {
    drawTextOutlined(rgb565, fbW, fbH, kMargin, kMargin,
                     modeName(state.mode), kColorAmber);
    drawExposureInfo(rgb565, fbW, fbH, state.shutterMs, state.iso);
    drawCaptureCount(rgb565, fbW, fbH, state.captureCount,
                     state.shutterMs > 0 || state.iso > 0);
    if (state.batteryValid)
        drawBatteryOverlay(rgb565, fbW, fbH, state.battery);
    if (state.settings.gridType != GridType::Off)
        drawGrid(rgb565, fbW, fbH, state.settings.gridType);
    if (state.meteringLocked) {
        constexpr int kAelY = kMargin + 2 * (kTextHeight + 3);
        drawTextOutlined(rgb565, fbW, fbH,
                         static_cast<int>(fbW) - 30, kAelY,
                         "AEL", kColorYellow);
    }
    if (state.timerRemaining > 0)
        drawTimerCountdown(rgb565, fbW, fbH, state.timerRemaining);
    if (state.captureInProgress)
        drawCaptureIndicator(rgb565, fbW, fbH);
    if (!state.errorMessage.empty())
        drawErrorOverlay(rgb565, fbW, fbH, state.errorMessage);
}

void drawHistogram(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size,
                   const uint8_t *yPlane, uint32_t w, uint32_t h,
                   uint32_t stride, size_t ySize) {
    if (!rgb565 || !yPlane || w == 0 || h == 0 || stride < w) return;
    size_t needFb = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
        !checkedMul(needFb, 2, needFb))
        return;
    if (needFb > rgb565Size) return;
    size_t needed = 0;
    if (!checkedMul(static_cast<size_t>(h), stride, needed)) return;
    if (ySize < needed) return;

    uint32_t hist[kHistogramBins] = {};
    uint32_t sampleCount = 0;
    for (uint32_t y = 0; y < h; y += kHistogramSampleStep) {
        for (uint32_t x = 0; x < w; x += kHistogramSampleStep) {
            uint8_t lum = yPlane[static_cast<size_t>(y) * stride + x];
            hist[lum]++;
            sampleCount++;
        }
    }
    if (sampleCount == 0) return;

    uint32_t maxVal = 1;
    for (int i = 1; i < kHistogramBins - 1; ++i) {
        maxVal = std::max(maxVal, hist[i]);
    }

    constexpr int kHistW = 48;
    constexpr int kHistH = 24;
    constexpr int kErrorBannerH = kTextHeight * 2 + 8;
    int histX = static_cast<int>(fbW) - kHistW - kMargin;
    int histY = static_cast<int>(fbH) - kHistH - kErrorBannerH - kMargin;

    fillRect(rgb565, fbW, fbH, histX - 1, histY - 1, kHistW + 2, kHistH + 2, kColorBlack);

    for (int col = 0; col < kHistW; ++col) {
        int binStart = (col * kHistogramBins) / kHistW;
        int binEnd = ((col + 1) * kHistogramBins) / kHistW;
        uint32_t sum = 0;
        for (int b = binStart; b < binEnd && b < kHistogramBins; ++b) sum += hist[b];
        uint32_t avg = sum / (binEnd - binStart > 0 ? (binEnd - binStart) : 1);
        uint64_t barH64 = (static_cast<uint64_t>(avg) * kHistH) / maxVal;
        int barH = static_cast<int>(std::min(barH64, static_cast<uint64_t>(kHistH)));
        for (int dy = 0; dy < barH; ++dy) {
            int px = histX + col;
            int py = histY + kHistH - 1 - dy;
            if (px >= 0 && py >= 0 &&
                static_cast<uint32_t>(px) < fbW &&
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
    drawText(rgb565, fbW, fbH,
             (static_cast<int>(fbW) - verW) / 2,
             y + kTextHeight + 4,
             ver, kColorGray, kColorBlack, true);
}

void drawCaptureIndicator(uint8_t *rgb565, uint32_t fbW, uint32_t fbH) {
    rectOutline(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorRed);
    std::string text = "SAVING";
    int textW = static_cast<int>(text.size()) * kTextWidth;
    drawTextWithBg(rgb565, fbW, fbH,
                   (static_cast<int>(fbW) - textW) / 2,
                   static_cast<int>(fbH) - kTextHeight - 4,
                   text, kColorRed, kColorBlack);
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

    fillRect(rgb565, fbW, fbH,
             x - 6, y - 6, totalW + 12, charH + 12, kColorBlack);
    for (size_t i = 0; i < num.size(); ++i) {
        drawCharScaled(rgb565, fbW, fbH,
                       x + static_cast<int>(i) * charW, y,
                       num[i], kScale, kColorYellow, kColorBlack, false);
    }
}

void drawGrid(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, GridType type) {
    if (type == GridType::Off) return;
    uint16_t color = kColorGrid;
    int w = static_cast<int>(fbW);
    int h = static_cast<int>(fbH);
    if (type == GridType::Thirds) {
        vLine(rgb565, fbW, fbH, w / 3, 0, h, color);
        vLine(rgb565, fbW, fbH, 2 * w / 3, 0, h, color);
        hLine(rgb565, fbW, fbH, 0, h / 3, w, color);
        hLine(rgb565, fbW, fbH, 0, 2 * h / 3, w, color);
    } else {
        for (int i = 1; i < 4; ++i) {
            vLine(rgb565, fbW, fbH, w * i / 4, 0, h, color);
            hLine(rgb565, fbW, fbH, 0, h * i / 4, w, color);
        }
    }
}

void drawReviewScreen(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      size_t rgb565Size,
                      const std::string &path,
                      const uint8_t *reviewPixels, size_t reviewSize) {
    size_t totalBytes = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, totalBytes) ||
        !checkedMul(totalBytes, 2, totalBytes)) return;
    if (totalBytes > rgb565Size) return;
    if (reviewPixels && reviewSize >= totalBytes) {
        std::memcpy(rgb565, reviewPixels, totalBytes);
    } else {
        fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);
        std::string header = "SAVED";
        int headerW = static_cast<int>(header.size()) * kTextWidth;
        drawText(rgb565, fbW, fbH,
                 (static_cast<int>(fbW) - headerW) / 2,
                 (static_cast<int>(fbH) - kTextHeight) / 2,
                 header, kColorGreen, kColorBlack, true);
    }

    int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
    drawTextWithBg(rgb565, fbW, fbH, kMargin, kMargin,
                   fitBasename(path, maxChars), kColorWhite, kColorBlack);

    std::string footer = "OK=CONT";
    int footerW = static_cast<int>(footer.size()) * kTextWidth;
    drawTextWithBg(rgb565, fbW, fbH,
                   (static_cast<int>(fbW) - footerW) / 2,
                   static_cast<int>(fbH) - kTextHeight - 4,
                   footer, kColorGreen, kColorBlack);
}

void drawSettingsMenu(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                      const CameraSettings &settings,
                      SettingsTab tab, int selectedItem) {
    fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);

    static constexpr std::string_view kTabNames[] = {"SHOT", "IMG", "DISP", "SYS"};
    constexpr int kTabCount = static_cast<int>(std::size(kTabNames));
    int tabWidth = static_cast<int>(fbW) / kTabCount;
    for (int i = 0; i < kTabCount; ++i) {
        bool active = (i == static_cast<int>(tab));
        uint16_t fg = active ? kColorBlack : kColorGray;
        uint16_t bg = active ? kColorAmber : kColorBlack;
        int tx = i * tabWidth + (tabWidth - static_cast<int>(kTabNames[i].size()) * kTextWidth) / 2;
        fillRect(rgb565, fbW, fbH, i * tabWidth, 0, tabWidth, kTextHeight + 4, bg);
        drawText(rgb565, fbW, fbH, tx, 2,
                 std::string(kTabNames[i]), fg, bg, false);
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
                         const std::vector<std::string> &files,
                         int selectedIdx, int &scrollOffset) {
    fillRect(rgb565, fbW, fbH, 0, 0, fbW, fbH, kColorBlack);

    std::string title = "PLAYBACK";
    int titleW = static_cast<int>(title.size()) * kTextWidth;
    drawText(rgb565, fbW, fbH,
             (static_cast<int>(fbW) - titleW) / 2, kMargin,
             title, kColorWhite, kColorBlack, true);

    if (files.empty()) {
        std::string empty = "NO IMAGES";
        int emptyW = static_cast<int>(empty.size()) * kTextWidth;
        drawText(rgb565, fbW, fbH,
                 (static_cast<int>(fbW) - emptyW) / 2,
                 (static_cast<int>(fbH) - kTextHeight) / 2,
                 empty, kColorGray, kColorBlack, true);
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
        if (fileIdx >= static_cast<int>(files.size())) break;

        std::string_view name = fitBasename(files[fileIdx], maxChars);

        bool selected = (fileIdx == selectedIdx);
        uint16_t fg = selected ? kColorBlack : kColorWhite;
        uint16_t bg = selected ? kColorGreen : kColorBlack;
        int textY = y + (kMenuRowH - kTextHeight) / 2;
        fillRect(rgb565, fbW, fbH, 0, y, fbW, kMenuRowH, bg);
        drawText(rgb565, fbW, fbH, kMargin + 2, textY, name, fg, bg, false);
        y += kMenuRowH;
    }

    std::string counter = std::to_string(selectedIdx + 1) + "/" +
                          std::to_string(files.size());
    int counterW = static_cast<int>(counter.size()) * kTextWidth;
    drawText(rgb565, fbW, fbH,
             (static_cast<int>(fbW) - counterW) / 2,
             static_cast<int>(fbH) - kTextHeight - 2,
             counter, kColorGray, kColorBlack, true);
}

void drawImageView(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
                   size_t rgb565Size,
                   const uint8_t *imageRgb565, size_t imageSize,
                   const std::string &path) {
    size_t totalBytes = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, totalBytes) ||
        !checkedMul(totalBytes, 2, totalBytes)) return;
    if (totalBytes > rgb565Size) return;
    if (imageSize >= totalBytes && imageRgb565) {
        std::memcpy(rgb565, imageRgb565, totalBytes);
    } else {
        std::memset(rgb565, 0, totalBytes);
    }

    int maxChars = std::max(0, static_cast<int>(fbW) / kTextWidth);
    drawTextWithBg(rgb565, fbW, fbH, kMargin, kMargin,
                   fitBasename(path, maxChars), kColorWhite, kColorBlack);

    std::string hint = "OK=BACK K3=DEL";
    int hintW = static_cast<int>(hint.size()) * kTextWidth;
    drawTextWithBg(rgb565, fbW, fbH,
                   (static_cast<int>(fbW) - hintW) / 2,
                   static_cast<int>(fbH) - kTextHeight - 4,
                   hint, kColorGray, kColorBlack);
}

void drawZebra(uint8_t *rgb565, uint32_t fbW, uint32_t fbH,
               size_t rgb565Size,
               const uint8_t *yPlane, uint32_t w, uint32_t h,
               uint32_t stride, size_t ySize, uint8_t threshold) {
    if (!yPlane || !rgb565) return;
    if (w == 0 || h == 0) return;
    if (stride < w) return;
    size_t needY = 0;
    if (!checkedMul(static_cast<size_t>(stride), h, needY)) return;
    if (ySize < needY) return;
    size_t needFb = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
        !checkedMul(needFb, 2, needFb)) return;
    if (needFb > rgb565Size) return;

    for (uint32_t dy = 0; dy < fbH; ++dy) {
        uint32_t sy = (static_cast<uint64_t>(dy) * h) / fbH;
        if (sy >= h) sy = h - 1;
        for (uint32_t dx = 0; dx < fbW; ++dx) {
            uint32_t sx = (static_cast<uint64_t>(dx) * w) / fbW;
            if (sx >= w) sx = w - 1;
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
                      size_t rgb565Size,
                      const uint8_t *yPlane, uint32_t w, uint32_t h,
                      uint32_t stride, size_t ySize) {
    if (!yPlane || !rgb565) return;
    if (w < 2 || h == 0) return;
    if (stride < w) return;
    size_t needY = 0;
    if (!checkedMul(static_cast<size_t>(stride), h, needY)) return;
    if (ySize < needY) return;
    size_t needFb = 0;
    if (!checkedMul(static_cast<size_t>(fbW), fbH, needFb) ||
        !checkedMul(needFb, 2, needFb)) return;
    if (needFb > rgb565Size) return;

    constexpr uint8_t kEdgeThreshold = 40;
    const uint16_t kPeakColor = ::picamera::rgb565(0, 255, 0);

    for (uint32_t dy = 0; dy < fbH; ++dy) {
        uint32_t sy = (static_cast<uint64_t>(dy) * h) / fbH;
        if (sy >= h) sy = h - 1;
        for (uint32_t dx = 0; dx < fbW; ++dx) {
            uint32_t sx = (static_cast<uint64_t>(dx) * w) / fbW;
            if (sx >= w - 1) sx = w - 2;
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

}
