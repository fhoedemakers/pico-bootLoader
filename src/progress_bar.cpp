#include "progress_bar.h"

#include "FrensHelpers.h"
#include "FrensFonts.h"
#include "font_8x8.h"

// Bar geometry, fixed at compile time. The "Flashing… / <name> / Do not
// power off." text from showMessage() renders at char rows 12/14/16 (pixel
// rows 96..135). Park the bar comfortably below it at y=160..179, centred
// horizontally with a 40-pixel margin on each side.
#define PB_X       40
#define PB_Y       160
#define PB_W       240
#define PB_H       20

// Percentage label, drawn in a fixed-width field just below the bar so the
// field self-clears: the text is right-aligned into PB_LBL_CHARS cells and
// every cell (spaces included) is painted, so "100%" -> "  7%" leaves no
// stale pixels behind. y=182 gives a 2-pixel gap under the bar; the 8 glyph
// rows then end at 189, clear of image_convert's filename band at 190.
#define PB_LBL_CHARS 4                                       // "100%"
#define PB_LBL_W     (PB_LBL_CHARS * FONT_CHAR_WIDTH)
#define PB_LBL_X     (PB_X + (PB_W - PB_LBL_W) / 2)
#define PB_LBL_Y     (PB_Y + PB_H + 2)

// Glyph table for the label: digits '0'..'9' plus '%' and ' '. Extracted from
// font_8x8[] by a constexpr initializer so the table lands in .data (SRAM,
// filled by the pre-main copy loop) rather than in flash -- same reason the
// renderer itself is __not_in_flash_func. Nothing here is read from XIP at
// draw time.
#define PB_GLYPH_SPACE  10
#define PB_GLYPH_PCT    11
#define PB_GLYPH_COUNT  12

struct PbGlyphs { uint8_t rows[PB_GLYPH_COUNT][FONT_CHAR_HEIGHT]; };

static constexpr char pb_glyph_char(int i)
{
    return (i < 10) ? (char)('0' + i) : (i == PB_GLYPH_SPACE ? ' ' : '%');
}

static constexpr PbGlyphs pb_make_glyphs()
{
    PbGlyphs g{};
    for (int i = 0; i < PB_GLYPH_COUNT; i++) {
        for (int r = 0; r < FONT_CHAR_HEIGHT; r++) {
            // Same indexing as getcharslicefrom8x8font(): the font is stored
            // row-major across all characters.
            g.rows[i][r] = (uint8_t)font_8x8[(pb_glyph_char(i) - FONT_FIRST_ASCII)
                                             + r * FONT_N_CHARS];
        }
    }
    return g;
}

static PbGlyphs __not_in_flash("pb_glyphs") pb_glyphs = pb_make_glyphs();

// Blit one glyph. Bit 0 of a font slice is the leftmost pixel (matches
// DrawScreen() and ic_fb_draw_text_centered()).
static void __not_in_flash_func(pb_draw_glyph)(uint16_t *fb, int x, int gi,
                                               uint16_t fg, uint16_t bg)
{
    for (int r = 0; r < FONT_CHAR_HEIGHT; r++) {
        uint16_t *dst  = fb + (PB_LBL_Y + r) * 320 + x;
        uint8_t   slice = pb_glyphs.rows[gi][r];
        for (int b = 0; b < FONT_CHAR_WIDTH; b++) {
            dst[b] = (slice & 1) ? fg : bg;
            slice >>= 1;
        }
    }
}

// __not_in_flash_func: invoked from the flash callback. During the flash op,
// core1 is still alive servicing HSTX scanlines and any code on core1's path
// is in SRAM. This function on core0 is also called between bootrom flash
// calls (XIP is restored by then) but we keep it in SRAM anyway to remove
// every code-path question.
extern "C" void __not_in_flash_func(progress_bar_draw)(uint32_t numer, uint32_t denom,
                                                      uint16_t col_fill,
                                                      uint16_t col_empty,
                                                      uint16_t col_border)
{
    // Pick the framebuffer pointer for whichever display backend is active.
    uint16_t *fb;
#if HSTX
    fb = (uint16_t *)hstx_getframebuffer();
#else
  #if FRAMEBUFFERISPOSSIBLE
    if (!Frens::isFrameBufferUsed()) return;   // line-stream mode -- no FB to update
    fb = Frens::framebuffer;
  #else
    (void)numer; (void)denom; (void)col_fill; (void)col_empty; (void)col_border;
    return;                                    // RP2040 PicoDVI line-stream: no FB
  #endif
#endif
    if (!fb) return;

    // How much of the interior is filled?  Interior = BAR_W - 2 (subtract the
    // 1-px left + right border). Clamp so a >100% numer never overruns.
    uint32_t fill_px = (denom > 0) ? (numer * (PB_W - 2) / denom) : 0;
    if (fill_px > (uint32_t)(PB_W - 2)) fill_px = PB_W - 2;

    uint32_t pct = (denom > 0) ? (numer * 100u / denom) : 0;
    if (pct > 100) pct = 100;

    // Top + bottom borders: solid horizontal stripe.
    for (int x = 0; x < PB_W; x++) {
        fb[ PB_Y                * 320 + PB_X + x] = col_border;
        fb[(PB_Y + PB_H - 1)    * 320 + PB_X + x] = col_border;
    }

    // Interior rows: left border, filled portion, empty portion, right border.
    for (int y = 1; y < PB_H - 1; y++) {
        uint16_t *row = fb + (PB_Y + y) * 320 + PB_X;
        row[0]         = col_border;
        for (int x = 1; x <= (int)fill_px; x++)         row[x] = col_fill;
        for (int x = (int)fill_px + 1; x < PB_W - 1; x++) row[x] = col_empty;
        row[PB_W - 1]  = col_border;
    }

    // Percentage label, right-aligned into the fixed field: "  0%" .. "100%".
    // Text in the border colour on the bar-interior colour -- both callers pick
    // those two for contrast (black-on-white while flashing, white-on-grey
    // during image conversion), so the label reads as part of the bar.
    int gi[PB_LBL_CHARS];
    gi[3] = PB_GLYPH_PCT;
    gi[2] = (int)(pct % 10);
    gi[1] = (pct >= 10)  ? (int)((pct / 10) % 10) : PB_GLYPH_SPACE;
    gi[0] = (pct >= 100) ? (int)(pct / 100)       : PB_GLYPH_SPACE;
    for (int i = 0; i < PB_LBL_CHARS; i++) {
        pb_draw_glyph(fb, PB_LBL_X + i * FONT_CHAR_WIDTH, gi[i], col_border, col_empty);
    }
}
