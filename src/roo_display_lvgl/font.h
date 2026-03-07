#pragma once

#include "lvgl.h"
#include "roo_display/font/font.h"

namespace roo_display {

/// Adapter exposing a `roo_display::Font` as an LVGL 9 `lv_font_t`.
class LvglFont {
 public:
  /// Construct an adapter around an existing roo_display font.
  explicit LvglFont(const Font& font, bool enable_kerning = true);

  /// Returns the LVGL font handle.
  const lv_font_t* lvFont() const { return &lv_font_; }

 private:
  static bool getGlyphDscCb(const lv_font_t* font, lv_font_glyph_dsc_t* dsc,
                            uint32_t letter, uint32_t letter_next);

  static const void* getGlyphBitmapCb(lv_font_glyph_dsc_t* dsc,
                                      lv_draw_buf_t* draw_buf);

  static void releaseGlyphCb(const lv_font_t* font, lv_font_glyph_dsc_t* dsc);

  const Font& font_;
  lv_font_t lv_font_;
};

}  // namespace roo_display
