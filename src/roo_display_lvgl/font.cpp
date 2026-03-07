#include "roo_display_lvgl/font.h"

#include <cstring>

#include "roo_display/color/color_modes.h"
#include "roo_display/color/named.h"
#include "roo_display/core/offscreen.h"
#include "roo_logging.h"

namespace roo_display {

LvglFont::LvglFont(const Font &font, bool enable_kerning) : font_(font) {
  std::memset(&lv_font_, 0, sizeof(lv_font_));
  lv_font_.get_glyph_dsc = &LvglFont::getGlyphDscCb;
  lv_font_.get_glyph_bitmap = &LvglFont::getGlyphBitmapCb;
  lv_font_.release_glyph = &LvglFont::releaseGlyphCb;
  lv_font_.line_height = font_.metrics().linespace();
  lv_font_.base_line = -font_.metrics().descent() + font_.metrics().linegap();
  lv_font_.subpx = LV_FONT_SUBPX_NONE;
  lv_font_.kerning =
      enable_kerning ? LV_FONT_KERNING_NORMAL : LV_FONT_KERNING_NONE;
  lv_font_.static_bitmap = 0;
  lv_font_.underline_position = 0;
  lv_font_.underline_thickness = 0;
  lv_font_.dsc = this;
  lv_font_.fallback = nullptr;
  lv_font_.user_data = nullptr;
}

bool LvglFont::getGlyphDscCb(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                             uint32_t letter, uint32_t letter_next) {
  DCHECK(font != nullptr);
  DCHECK(dsc != nullptr);
  auto *self = static_cast<const LvglFont *>(font->dsc);
  DCHECK(self != nullptr);
  GlyphMetrics m;
  if (!self->font_.getGlyphMetrics(static_cast<char32_t>(letter),
                                   FontLayout::kHorizontal, &m)) {
    return false;
  }

  int advance = m.advance();
  if (font->kerning == LV_FONT_KERNING_NORMAL && letter_next != 0) {
    advance -= self->font_.getKerning(static_cast<char32_t>(letter),
                                      static_cast<char32_t>(letter_next));
  }
  if (advance < 0) {
    advance = 0;
  }

  dsc->resolved_font = font;
  dsc->adv_w = static_cast<uint16_t>(advance);
  dsc->box_w = static_cast<uint16_t>(m.width());
  dsc->box_h = static_cast<uint16_t>(m.height());
  dsc->ofs_x = static_cast<int16_t>(m.glyphXMin());
  // LVGL expects ofs_y with 1px-inclusive box bounds semantics.
  dsc->ofs_y = static_cast<int16_t>(m.glyphYMin() - 1);
  dsc->stride = lv_draw_buf_width_to_stride(dsc->box_w, LV_COLOR_FORMAT_A8);
  dsc->format = LV_FONT_GLYPH_FORMAT_A8;
  dsc->is_placeholder = 0;
  dsc->req_raw_bitmap = 0;
  dsc->outline_stroke_width = 0;
  dsc->gid.index = letter;
  dsc->entry = nullptr;
  return true;
}

const void *LvglFont::getGlyphBitmapCb(lv_font_glyph_dsc_t *dsc,
                                       lv_draw_buf_t *draw_buf) {
  DCHECK(dsc != nullptr);
  DCHECK(draw_buf != nullptr);
  if (dsc->box_w == 0 || dsc->box_h == 0) {
    return nullptr;
  }

  DCHECK(draw_buf->data != nullptr);
  DCHECK(draw_buf->header.h >= dsc->box_h);
  DCHECK(draw_buf->header.w >= dsc->box_w);
  if (draw_buf->header.h < dsc->box_h || draw_buf->header.w < dsc->box_w) {
    LOG(WARNING) << "LVGL requested a glyph bitmap with box size "
                 << Box(0, 0, dsc->box_w - 1, dsc->box_h - 1)
                 << " that doesn't fit in the provided draw buffer of size "
                 << Box(0, 0, draw_buf->header.w - 1, draw_buf->header.h - 1);
    return nullptr;
  }

  const lv_font_t *font = dsc->resolved_font;
  DCHECK(font != nullptr);
  auto* self = static_cast<const LvglFont *>(font->dsc);
  DCHECK(self != nullptr);

  const uint32_t stride = draw_buf->header.stride;

  // Use stride as offscreen width so each rendered row lands at the right
  // position in LVGL's potentially padded draw buffer.
  Offscreen<Alpha8> offscreen(static_cast<int16_t>(stride), dsc->box_h,
                              draw_buf->data, Alpha8(color::Black));
  Surface surface(offscreen.output(), -dsc->ofs_x,
                  static_cast<int16_t>(dsc->box_h + dsc->ofs_y),
                  Box(0, 0, dsc->box_w - 1, dsc->box_h - 1), false,
                  color::Transparent, FillMode::kExtents,
                  BlendingMode::kSource);

  self->font_.drawGlyph(surface, static_cast<char32_t>(dsc->gid.index),
                        FontLayout::kHorizontal, color::Black);

  return draw_buf;
}

void LvglFont::releaseGlyphCb(const lv_font_t *font, lv_font_glyph_dsc_t *dsc) {
  (void)font;
  (void)dsc;
}

} // namespace roo_display
