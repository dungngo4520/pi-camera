#include "font.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace picamera {

// ========================================================================== //
// FreeType-based renderer (DejaVu Sans Mono)
// ========================================================================== //
#ifdef HAVE_FREETYPE

namespace {

struct FtState {
  FT_Library library = nullptr;
  FT_Face face = nullptr;
  int advanceX = 6; // default fallback
  int glyphHeight = 8;
  int glyphTop = 7; // baseline offset from top
  bool ok = false;
};

struct GlyphCache {
  int width = 0;
  int height = 0;
  int left = 0;                // bitmap_left from FreeType
  int top = 0;                 // bitmap_top from FreeType
  int advanceX = 0;            // advance in pixels
  std::vector<uint8_t> bitmap; // grayscale, width*height bytes
};

FtState &ftState() {
  static FtState state;
  static std::once_flag flag;
  std::call_once(flag, [&] {
    if (FT_Init_FreeType(&state.library))
      return;

    // Try common monospace font paths on Debian/Raspberry Pi OS.
    const char *fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    };
    bool loaded = false;
    for (const auto *path : fontPaths) {
      if (FT_New_Face(state.library, path, 0, &state.face) == 0) {
        loaded = true;
        break;
      }
    }
    if (!loaded)
      return;

    // 8px pixel size — compact but readable on 128x128 LCD.
    // DejaVu Sans Mono at 8px gives ~5px advance, 7px height.
    if (FT_Set_Pixel_Sizes(state.face, 0, 8) != 0)
      return;

    // Read advance width from the 'M' glyph (monospace = all same).
    if (FT_Load_Char(state.face, 'M', FT_LOAD_RENDER) == 0) {
      state.advanceX = state.face->glyph->advance.x >> 6;
      if (state.advanceX < 1)
        state.advanceX = 6;
    }
    state.glyphHeight = state.face->size->metrics.height >> 6;
    state.glyphTop = state.face->size->metrics.ascender >> 6;
    state.ok = true;
  });
  return state;
}

// Cache of rendered glyphs, keyed by character code.
std::map<unsigned long, GlyphCache> &glyphCache() {
  static std::map<unsigned long, GlyphCache> cache;
  return cache;
}

// Render (or fetch from cache) a single glyph.
const GlyphCache *getGlyph(unsigned long ch) {
  auto &ft = ftState();
  if (!ft.ok)
    return nullptr;

  auto &cache = glyphCache();
  auto it = cache.find(ch);
  if (it != cache.end())
    return &it->second;

  // Use monochrome rendering (no anti-aliasing) for crisp, thin glyphs
  // at small pixel sizes. Anti-aliasing at 8px makes text look bold on
  // the 128x128 LCD. FT_LOAD_TARGET_MONO produces 1-bit bitmaps.
  if (FT_Load_Char(ft.face, ch, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO) != 0)
    return nullptr;

  FT_GlyphSlot g = ft.face->glyph;
  GlyphCache gc;
  gc.width = static_cast<int>(g->bitmap.width);
  gc.height = static_cast<int>(g->bitmap.rows);
  gc.left = g->bitmap_left;
  gc.top = g->bitmap_top;
  gc.advanceX = static_cast<int>(g->advance.x >> 6);
  if (gc.advanceX < 1)
    gc.advanceX = ft.advanceX;

  size_t bmpSize = static_cast<size_t>(gc.width) * gc.height;
  gc.bitmap.resize(bmpSize);
  if (g->bitmap.buffer && bmpSize > 0) {
    // FT_LOAD_TARGET_MONO produces 1-bit packed bitmaps (MSB first,
    // 8 pixels per byte, rows padded to byte boundaries). Unpack to
    // 1 byte per pixel: 0 = off, 255 = on.
    int pitch = g->bitmap.pitch; // bytes per row (may be negative)
    const uint8_t *row = g->bitmap.buffer;
    for (int r = 0; r < gc.height; ++r) {
      const uint8_t *srcRow =
          (pitch >= 0)
              ? row + static_cast<ptrdiff_t>(r) * pitch
              : row + static_cast<ptrdiff_t>(gc.height - 1 - r) * (-pitch);
      for (int c = 0; c < gc.width; ++c) {
        uint8_t byte = srcRow[c / 8];
        bool on = (byte >> (7 - (c % 8))) & 1;
        gc.bitmap[static_cast<size_t>(r) * gc.width + c] = on ? 255 : 0;
      }
    }
  }

  auto [inserted, _] = cache.emplace(ch, std::move(gc));
  return &inserted->second;
}

// Write a single RGB565 pixel (big-endian, for SPI display) at (x, y).
inline void setPixel(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                     uint16_t color) {
  if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= fbW ||
      static_cast<uint32_t>(y) >= fbH)
    return;
  size_t idx = (static_cast<size_t>(y) * fbW + x) * 2;
  rgb565[idx] = static_cast<uint8_t>(color >> 8);       // MSB
  rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF); // LSB
}

// Blend foreground and background based on alpha (0-255).
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  if (alpha == 0)
    return bg;
  if (alpha == 255)
    return fg;
  // Extract RGB components
  uint8_t fr = (fg >> 11) & 0x1F;
  uint8_t fg_g = (fg >> 5) & 0x3F;
  uint8_t fb = fg & 0x1F;
  uint8_t br = (bg >> 11) & 0x1F;
  uint8_t bg_g = (bg >> 5) & 0x3F;
  uint8_t bb = bg & 0x1F;
  // Blend
  uint8_t a = alpha;
  uint8_t ia = 255 - a;
  uint8_t r = (fr * a + br * ia) / 255;
  uint8_t g = (fg_g * a + bg_g * ia) / 255;
  uint8_t b = (fb * a + bb * ia) / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

} // namespace

void drawChar(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
              char ch, uint16_t fg, uint16_t bg, bool transparent) {
  auto &ft = ftState();
  if (!ft.ok)
    return;

  const GlyphCache *gc = getGlyph(static_cast<unsigned char>(ch));
  if (!gc)
    return;

  // FreeType renders glyphs with bearing offsets. The glyph bitmap's
  // top-left is at (x + gc->left, y + gc->top - 1) relative to the
  // baseline at (x, y + ft.glyphTop). We simplify by placing the
  // bitmap at (x, y) with vertical adjustment for the ascender.
  int drawX = x + gc->left;
  int drawY = y + (ft.glyphTop - gc->top);

  for (int row = 0; row < gc->height; ++row) {
    for (int col = 0; col < gc->width; ++col) {
      uint8_t alpha = gc->bitmap[static_cast<size_t>(row) * gc->width + col];
      if (alpha == 0) {
        if (!transparent) {
          setPixel(rgb565, fbW, fbH, drawX + col, drawY + row, bg);
        }
      } else {
        uint16_t color = transparent ? fg : blend565(fg, bg, alpha);
        setPixel(rgb565, fbW, fbH, drawX + col, drawY + row, color);
      }
    }
  }
}

void drawCharScaled(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                    char ch, int scale, uint16_t fg, uint16_t bg,
                    bool transparent) {
  if (scale <= 0)
    return;
  if (scale == 1) {
    drawChar(rgb565, fbW, fbH, x, y, ch, fg, bg, transparent);
    return;
  }
  auto &ft = ftState();
  if (!ft.ok)
    return;

  const GlyphCache *gc = getGlyph(static_cast<unsigned char>(ch));
  if (!gc)
    return;

  int drawX = x + gc->left * scale;
  int drawY = y + (ft.glyphTop - gc->top) * scale;

  for (int row = 0; row < gc->height; ++row) {
    for (int col = 0; col < gc->width; ++col) {
      uint8_t alpha = gc->bitmap[static_cast<size_t>(row) * gc->width + col];
      if (alpha == 0) {
        if (!transparent) {
          for (int dy = 0; dy < scale; ++dy)
            for (int dx = 0; dx < scale; ++dx)
              setPixel(rgb565, fbW, fbH, drawX + col * scale + dx,
                       drawY + row * scale + dy, bg);
        }
      } else {
        uint16_t color = transparent ? fg : blend565(fg, bg, alpha);
        for (int dy = 0; dy < scale; ++dy)
          for (int dx = 0; dx < scale; ++dx)
            setPixel(rgb565, fbW, fbH, drawX + col * scale + dx,
                     drawY + row * scale + dy, color);
      }
    }
  }
}

int drawText(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
             std::string_view text, uint16_t fg, uint16_t bg,
             bool transparent) {
  int cx = x;
  auto &ft = ftState();
  int advance = ft.ok ? ft.advanceX : 6;
  for (char ch : text) {
    drawChar(rgb565, fbW, fbH, cx, y, ch, fg, bg, transparent);
    cx += advance;
  }
  return cx - x;
}

// ========================================================================== //
// Bitmap font fallback (no FreeType)
// ========================================================================== //
#else

// --- 5x7 bitmap font ---
// Each glyph: 5 bytes, one per COLUMN. Bit 0 = top row, bit 4 = bottom row.
// Bits 5-7 are unused. This is column-major data (the classic LCD font format).
//
// The Waveshare 1.44" LCD HAT panel is physically rotated 90° CCW.
// The display driver corrects this with a 90° CW software rotation,
// giving identity mapping FB(x,y) → screen(x,y). But the font data
// encodes glyphs sideways (column-major). To render upright, drawChar
// transposes the glyph: column `col` becomes screen row `col`, and
// bit `row` becomes screen column `row`. This is equivalent to
// rotating the glyph 90° CW, making it upright on the corrected display.
//
// This is a compact subset of the classic 5x7 LCD font, sufficient for
// battery percentage display: digits, %, V, ., -, +, space, A-Z.

// Glyph entry: character + 5 column bytes
struct Glyph {
  char ch;
  uint8_t cols[5];
};

static constexpr Glyph kGlyphTable[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x0E, 0x04, 0x00}},
    {'-', {0x00, 0x00, 0x0E, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x06}},
    {'%', {0x19, 0x0A, 0x04, 0x0A, 0x13}},
    {'0', {0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x0E, 0x02, 0x06, 0x02, 0x0E}},
    {'4', {0x11, 0x11, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x1E}},
    {'6', {0x0E, 0x10, 0x1E, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x04, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x0E, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x0F, 0x01, 0x0E}},
    {'V', {0x11, 0x11, 0x0A, 0x0A, 0x04}},
    {'A', {0x0E, 0x11, 0x1F, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x1E, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x1E, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x1E, 0x10, 0x10}},
    {'G', {0x0E, 0x10, 0x17, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x1F, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x1C, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x1E, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x1E, 0x12, 0x11}},
    {'S', {0x0E, 0x10, 0x0E, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'W', {0x11, 0x11, 0x15, 0x1B, 0x11}},
    {'X', {0x11, 0x0A, 0x04, 0x0A, 0x11}},
    {'Y', {0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x04, 0x10, 0x1F}},
};

static constexpr int kGlyphTableLen =
    sizeof(kGlyphTable) / sizeof(kGlyphTable[0]);

namespace {

const uint8_t *findGlyph(char ch) {
  // Convert lowercase to uppercase
  if (ch >= 'a' && ch <= 'z')
    ch -= 32;
  for (int i = 0; i < kGlyphTableLen; ++i) {
    if (kGlyphTable[i].ch == ch)
      return kGlyphTable[i].cols;
  }
  // Unknown char: return space
  return kGlyphTable[0].cols;
}

// Write a single RGB565 pixel (big-endian, for SPI display) at (x, y)
inline void setPixel(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                     uint16_t color) {
  if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= fbW ||
      static_cast<uint32_t>(y) >= fbH)
    return;
  size_t idx = (static_cast<size_t>(y) * fbW + x) * 2;
  rgb565[idx] = static_cast<uint8_t>(color >> 8);       // MSB
  rgb565[idx + 1] = static_cast<uint8_t>(color & 0xFF); // LSB
}

} // namespace

void drawChar(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
              char ch, uint16_t fg, uint16_t bg, bool transparent) {
  const uint8_t *glyph = findGlyph(ch);
  // Glyph data is column-major: glyph[col] is a byte where bit `row`
  // = pixel at (col, row). To render upright on the corrected display,
  // transpose with vertical flip: screen pixel (x+(4-row), y+col)
  // = glyph bit (col, row). This rotates the sideways glyph 90° CW
  // and flips vertically, producing an upright, non-mirrored character.
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 5; ++row) {
      bool on = (bits >> row) & 1;
      uint16_t color = on ? fg : bg;
      if (on || !transparent) {
        setPixel(rgb565, fbW, fbH, x + (4 - row), y + col, color);
      }
    }
  }
}

void drawCharScaled(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                    char ch, int scale, uint16_t fg, uint16_t bg,
                    bool transparent) {
  if (scale <= 0)
    return; // scale 0 = draw nothing
  if (scale == 1) {
    drawChar(rgb565, fbW, fbH, x, y, ch, fg, bg, transparent);
    return;
  }
  const uint8_t *glyph = findGlyph(ch);
  for (int col = 0; col < 5; ++col) {
    uint8_t bits = glyph[col];
    for (int row = 0; row < 5; ++row) {
      bool on = (bits >> row) & 1;
      if (!on && transparent)
        continue;
      uint16_t color = on ? fg : bg;
      // Fill a scale x scale block (transposed + V-flipped)
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          setPixel(rgb565, fbW, fbH, x + (4 - row) * scale + dx,
                   y + col * scale + dy, color);
        }
      }
    }
  }
}

int drawText(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
             std::string_view text, uint16_t fg, uint16_t bg,
             bool transparent) {
  int cx = x;
  for (char ch : text) {
    drawChar(rgb565, fbW, fbH, cx, y, ch, fg, bg, transparent);
    cx += 6; // 5px glyph + 1px spacing
  }
  return cx - x;
}

#endif // HAVE_FREETYPE

// ========================================================================== //
// Battery icon (shared by both renderers)
// ========================================================================== //

void drawBatteryIcon(uint8_t *rgb565, uint32_t fbW, uint32_t fbH, int x, int y,
                     int percent) {
  // Battery icon: 16px wide body + 2px terminal nub = 18px total, 9px tall
  // Body: x to x+15, y to y+8
  // Terminal: x+16 to x+17, y+2 to y+6

  // Clamp percent
  percent = std::max(0, std::min(100, percent));

  // Fill color based on charge level
  uint16_t fillColor;
  if (percent > 50)
    fillColor = kColorGreen;
  else if (percent > 20)
    fillColor = kColorYellow;
  else
    fillColor = kColorRed;

  // Draw outline (1px border)
  for (int dx = 0; dx < 16; ++dx) {
    setPixel(rgb565, fbW, fbH, x + dx, y, kColorWhite);
    setPixel(rgb565, fbW, fbH, x + dx, y + 8, kColorWhite);
  }
  for (int dy = 0; dy < 9; ++dy) {
    setPixel(rgb565, fbW, fbH, x, y + dy, kColorWhite);
    setPixel(rgb565, fbW, fbH, x + 15, y + dy, kColorWhite);
  }

  // Draw terminal nub
  for (int dy = 2; dy <= 6; ++dy) {
    setPixel(rgb565, fbW, fbH, x + 16, y + dy, kColorWhite);
    setPixel(rgb565, fbW, fbH, x + 17, y + dy, kColorWhite);
  }

  // Draw fill bar (inside the outline: x+1 to x+14, y+1 to y+7)
  // Fill width proportional to percent
  int fillW = (14 * percent) / 100; // max 14px interior width
  for (int dx = 0; dx < fillW; ++dx) {
    for (int dy = 1; dy <= 7; ++dy) {
      setPixel(rgb565, fbW, fbH, x + 1 + dx, y + dy, fillColor);
    }
  }

  // Clear unfilled portion to black
  for (int dx = fillW; dx < 14; ++dx) {
    for (int dy = 1; dy <= 7; ++dy) {
      setPixel(rgb565, fbW, fbH, x + 1 + dx, y + dy, kColorBlack);
    }
  }
}

} // namespace picamera
