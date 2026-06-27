#include "gui.h"

#include <cstdio>
#include <cstring>

#include "FrensFonts.h"
#include "FrensHelpers.h"
#include "ff.h"

namespace {
    uint16_t *s_buf_cur          = nullptr;
    uint16_t *s_buf_next         = nullptr;
    bool      s_next_is_half_res = false;   // true on SRAM-only configs
    const char *s_footer         = nullptr; // one-line button-hint overlay
}

// Half-resolution dimensions for the slide-only buffer used when there is
// no PSRAM. 160x120 = 38,400 bytes, 4x smaller than the full image.
#define GUI_HALF_W (SCREENWIDTH  / 2)
#define GUI_HALF_H (SCREENHEIGHT / 2)

// Footer band: one 8-pixel character row at the very bottom of the screen.
#define GUI_FOOTER_H   FONT_CHAR_HEIGHT                  // 8
#define GUI_FOOTER_Y0  (SCREENHEIGHT - GUI_FOOTER_H)     // 232

// Backend-specific 16-bit pixel encodings for the footer band -- same
// per-backend split main.cpp uses for the error overlay (RGB555 on HSTX,
// RGB444 packed 0x0RGB on PicoDVI).
#if HSTX
#define GUI_FOOTER_FG  0x7FFFu
#define GUI_FOOTER_BG  0x0000u
#else
#define GUI_FOOTER_FG  0x0FFFu
#define GUI_FOOTER_BG  0x0000u
#endif

extern "C" {

// Returns the saved mode. When the file is absent or unreadable the default
// is graphical -- that's the experience first-time users should land on.
bool gui_load_mode(const char *path)
{
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return true;
    char c = '1';
    UINT br = 0;
    f_read(&fil, &c, 1, &br);
    f_close(&fil);
    if (br != 1) return true;
    return c != '0';
}

void gui_save_mode(const char *path, bool graphical)
{
    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[emuLoader] gui: cannot write mode file %s\n", path);
        return;
    }
    char c = graphical ? '1' : '0';
    UINT bw = 0;
    f_write(&fil, &c, 1, &bw);
    f_close(&fil);
}

void gui_set_footer(const char *text) { s_footer = text; }

bool gui_buffers_alloc(void)
{
    const size_t sz_full = SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t);
    if (!s_buf_cur)  s_buf_cur  = (uint16_t *)Frens::f_malloc(sz_full);
    if (!s_buf_cur) {
        printf("[emuLoader] gui: cur buffer alloc failed (size=%u)\n", (unsigned)sz_full);
        return false;
    }
    // Second buffer enables the slide animation. With PSRAM we mirror cur
    // at full resolution and the swap-pointers-after-slide pattern works.
    // Without PSRAM we'd OOM (215 KB base + 2x150 KB > 512 KB SRAM), so
    // allocate the next buffer at half res -- still enough to drive a
    // slide, just visibly chunkier. The caller reloads cur at full res
    // after the slide to restore the static-display sharpness.
    if (!s_buf_next) {
        if (Frens::isPsramEnabled()) {
            s_buf_next = (uint16_t *)Frens::f_malloc(sz_full);
            s_next_is_half_res = false;
        } else {
            const size_t sz_half = GUI_HALF_W * GUI_HALF_H * sizeof(uint16_t);
            s_buf_next = (uint16_t *)Frens::f_malloc(sz_half);
            s_next_is_half_res = true;
        }
    }
    if (!s_buf_next) {
        printf("[emuLoader] gui: slide buffer alloc failed; animation disabled\n");
    } else {
        printf("[emuLoader] gui: buffers ready (cur=full, next=%s)\n",
               s_next_is_half_res ? "half" : "full");
    }
    return true;
}

bool gui_next_is_half_res(void) { return s_next_is_half_res; }

uint16_t *gui_buf_cur(void)  { return s_buf_cur;  }
uint16_t *gui_buf_next(void) { return s_buf_next; }

void gui_swap_buffers(void)
{
    uint16_t *t = s_buf_cur;
    s_buf_cur  = s_buf_next;
    s_buf_next = t;
}

void gui_fill_solid(uint16_t *dest, uint16_t color)
{
    if (!dest) return;
    const size_t pixels = SCREENWIDTH * SCREENHEIGHT;
    for (size_t i = 0; i < pixels; i++) dest[i] = color;
}

void gui_fill_solid_half_res(uint16_t *dest, uint16_t color)
{
    if (!dest) return;
    const size_t pixels = GUI_HALF_W * GUI_HALF_H;
    for (size_t i = 0; i < pixels; i++) dest[i] = color;
}

bool gui_load_image(const char *image_key, uint16_t *dest)
{
    if (!dest || !image_key || !*image_key) return false;

    char path[FF_MAX_LFN + 1];
    snprintf(path, sizeof(path), "/emu/assets/%s%s", image_key, FILEXTFORSEARCH);

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[emuLoader] gui: cannot open %s (fr=%d)\n", path, (int)fr);
        return false;
    }

    uint16_t hdr[2];
    UINT br = 0;
    fr = f_read(&fil, hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br != sizeof(hdr)) {
        printf("[emuLoader] gui: %s header read failed (fr=%d br=%u)\n",
               path, (int)fr, (unsigned)br);
        f_close(&fil);
        return false;
    }
    if (hdr[0] != SCREENWIDTH || hdr[1] != SCREENHEIGHT) {
        printf("[emuLoader] gui: %s is %u x %u, expected %d x %d\n",
               path, (unsigned)hdr[0], (unsigned)hdr[1], SCREENWIDTH, SCREENHEIGHT);
        f_close(&fil);
        return false;
    }

    const UINT want = SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t);
    fr = f_read(&fil, dest, want, &br);
    f_close(&fil);
    if (fr != FR_OK || br != want) {
        printf("[emuLoader] gui: %s pixel read short (fr=%d br=%u/%u)\n",
               path, (int)fr, (unsigned)br, (unsigned)want);
        return false;
    }
    return true;
}

// Load + downsample 2:1 in each axis on the fly. Reads the file scanline by
// scanline (640-byte stack scratch) so it never needs a large temporary
// buffer -- the half-res path exists exactly to avoid one of those.
bool gui_load_image_half_res(const char *image_key, uint16_t *dest)
{
    if (!dest || !image_key || !*image_key) return false;

    char path[FF_MAX_LFN + 1];
    snprintf(path, sizeof(path), "/emu/assets/%s%s", image_key, FILEXTFORSEARCH);

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[emuLoader] gui: cannot open %s (fr=%d)\n", path, (int)fr);
        return false;
    }

    uint16_t hdr[2];
    UINT br = 0;
    fr = f_read(&fil, hdr, sizeof(hdr), &br);
    if (fr != FR_OK || br != sizeof(hdr) ||
        hdr[0] != SCREENWIDTH || hdr[1] != SCREENHEIGHT) {
        printf("[emuLoader] gui: %s header bad (fr=%d br=%u, %ux%u)\n",
               path, (int)fr, (unsigned)br, (unsigned)hdr[0], (unsigned)hdr[1]);
        f_close(&fil);
        return false;
    }

    uint16_t row[SCREENWIDTH];   // 640 bytes on the stack
    const UINT row_bytes = SCREENWIDTH * sizeof(uint16_t);

    for (int y_out = 0; y_out < GUI_HALF_H; y_out++) {
        // Read the even source row, downsample horizontally into dest.
        fr = f_read(&fil, row, row_bytes, &br);
        if (fr != FR_OK || br != row_bytes) {
            printf("[emuLoader] gui: %s short read row %d\n", path, 2 * y_out);
            f_close(&fil);
            return false;
        }
        uint16_t *out = dest + y_out * GUI_HALF_W;
        for (int x = 0; x < GUI_HALF_W; x++) out[x] = row[2 * x];

        // Read and discard the odd row (nearest-neighbour vertical decimation).
        fr = f_read(&fil, row, row_bytes, &br);
        if (fr != FR_OK || br != row_bytes) {
            printf("[emuLoader] gui: %s short read row %d\n", path, 2 * y_out + 1);
            f_close(&fil);
            return false;
        }
    }
    f_close(&fil);
    return true;
}

// Helper: write n pixels of b's scanline into dst, starting at b-column
// b_start. b is the full-resolution row at output-row y, so this is just
// a memcpy.
static inline void copy_b_full(uint16_t *dst, const uint16_t *brow,
                               int b_start, int n)
{
    memcpy(dst, brow + b_start, n * sizeof(uint16_t));
}

// Helper: write n pixels of half-res b into dst, starting at output-column
// b_start in 320-px space. b's row is shared between two output rows
// (y/2) and each b pixel covers two output columns. Each output column
// x reads brow_half[x / 2].
static inline void copy_b_half(uint16_t *dst, const uint16_t *brow_half,
                               int b_start, int n)
{
    for (int i = 0; i < n; i++) {
        dst[i] = brow_half[(b_start + i) >> 1];
    }
}

// Paint one scanline of the footer band on top of whatever compose_row()
// just wrote into dst. y is in [GUI_FOOTER_Y0, SCREENHEIGHT). Fills the
// whole row with bg so the band is opaque, then overlays glyph pixels from
// the shared 8x8 font (same pattern as menu.cpp's charcell pipeline).
static void overlay_footer_row(uint16_t *dst, int y, const char *text)
{
    const uint16_t fg = GUI_FOOTER_FG;
    const uint16_t bg = GUI_FOOTER_BG;

    for (int x = 0; x < SCREENWIDTH; x++) dst[x] = bg;

    const int len      = (int)strlen(text);
    const int text_w   = len * FONT_CHAR_WIDTH;
    const int x_start  = (SCREENWIDTH - text_w) / 2;
    const int row_in_g = y - GUI_FOOTER_Y0;   // 0..7 within the glyph

    if (x_start < 0 || text_w > SCREENWIDTH) return;   // string too wide

    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS)
            c = '?';
        char slice = getcharslicefrom8x8font((char)c, row_in_g);
        uint16_t *out = dst + x_start + i * FONT_CHAR_WIDTH;
        for (int bit = 0; bit < 8; bit++) {
            out[bit] = (slice & 1) ? fg : bg;
            slice >>= 1;
        }
    }
}

// Per-scanline composition. `a` is always full 320x240. `b` may be NULL,
// 320x240 (b_half_res=false), or 160x120 (b_half_res=true).
static inline void compose_row(uint16_t *dst, int y,
                               const uint16_t *a, const uint16_t *b,
                               int p, int dir, bool b_half_res)
{
    const uint16_t *arow = a + y * SCREENWIDTH;

    if (!b || p <= 0 || dir == 0) {
        memcpy(dst, arow, SCREENWIDTH * sizeof(uint16_t));
        return;
    }

    // Locate b's row for this output scanline.
    const uint16_t *brow = b_half_res
        ? (b + (y >> 1) * GUI_HALF_W)
        : (b + y * SCREENWIDTH);

    if (p >= SCREENWIDTH) {
        // All b.
        if (b_half_res) copy_b_half(dst, brow, 0, SCREENWIDTH);
        else            copy_b_full(dst, brow, 0, SCREENWIDTH);
    } else if (dir > 0) {
        // b slides in from the right. Old image scrolls left.
        // dst = arow[p .. W-1] then b mapped to columns 0 .. p-1.
        memcpy(dst, arow + p, (SCREENWIDTH - p) * sizeof(uint16_t));
        if (b_half_res) copy_b_half(dst + (SCREENWIDTH - p), brow, 0, p);
        else            copy_b_full(dst + (SCREENWIDTH - p), brow, 0, p);
    } else {
        // b slides in from the left. Old image scrolls right.
        // dst = b mapped to columns W-p .. W-1 then arow[0 .. W-1-p].
        if (b_half_res) copy_b_half(dst, brow, SCREENWIDTH - p, p);
        else            copy_b_full(dst, brow, SCREENWIDTH - p, p);
        memcpy(dst + p, arow, (SCREENWIDTH - p) * sizeof(uint16_t));
    }
}

void gui_draw_frame(const uint16_t *a, const uint16_t *b,
                    int slide_px, int direction, bool b_half_res)
{
    if (!a) return;
    if (slide_px < 0) slide_px = 0;
    if (slide_px > SCREENWIDTH) slide_px = SCREENWIDTH;

    for (int y = 0; y < SCREENHEIGHT; y++) {
        uint16_t *dst;
#if !HSTX
        dvi::DVI::LineBuffer *lb = nullptr;
#if FRAMEBUFFERISPOSSIBLE
        if (Frens::isFrameBufferUsed()) {
            dst = &Frens::framebuffer[y * SCREENWIDTH];
        } else
#endif
        {
            lb  = dvi_->getLineBuffer();
            dst = lb->data();
        }
#else
        dst = hstx_getlineFromFramebuffer(y);
#endif

        compose_row(dst, y, a, b, slide_px, direction, b_half_res);

        // Footer band: composed into dst before the line-stream push so
        // every backend (HSTX FB, PicoDVI FB, PicoDVI line-stream) gets it.
        if (s_footer && *s_footer && y >= GUI_FOOTER_Y0) {
            overlay_footer_row(dst, y, s_footer);
        }

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
        if (!Frens::isFrameBufferUsed())
#endif
            dvi_->setLineBuffer(y, lb);
#endif
    }
}

} // extern "C"
