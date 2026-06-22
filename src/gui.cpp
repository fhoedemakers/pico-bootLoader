#include "gui.h"

#include <cstdio>
#include <cstring>

#include "FrensHelpers.h"
#include "ff.h"

namespace {
    uint16_t *s_buf_cur  = nullptr;
    uint16_t *s_buf_next = nullptr;
}

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

bool gui_buffers_alloc(void)
{
    const size_t sz = SCREENWIDTH * SCREENHEIGHT * sizeof(uint16_t);
    if (!s_buf_cur)  s_buf_cur  = (uint16_t *)Frens::f_malloc(sz);
    if (!s_buf_next) s_buf_next = (uint16_t *)Frens::f_malloc(sz);
    if (!s_buf_cur || !s_buf_next) {
        printf("[emuLoader] gui: buffer alloc failed (cur=%p next=%p, size=%u)\n",
               s_buf_cur, s_buf_next, (unsigned)sz);
        return false;
    }
    return true;
}

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

// Per-scanline composition. Writes one row of `dst` (length SCREENWIDTH) from
// `a` (full 320x240) and optionally `b` (also 320x240) according to `p` and
// `dir`. Backend-agnostic; caller does the framebuffer/line-buffer routing.
static inline void compose_row(uint16_t *dst, int y,
                               const uint16_t *a, const uint16_t *b,
                               int p, int dir)
{
    const uint16_t *arow = a + y * SCREENWIDTH;
    const uint16_t *brow = b ? (b + y * SCREENWIDTH) : nullptr;

    if (!brow || p <= 0 || dir == 0) {
        memcpy(dst, arow, SCREENWIDTH * sizeof(uint16_t));
    } else if (p >= SCREENWIDTH) {
        memcpy(dst, brow, SCREENWIDTH * sizeof(uint16_t));
    } else if (dir > 0) {
        // b slides in from the right; a slides out to the left.
        // dst = arow[p .. W-1] then brow[0 .. p-1]
        memcpy(dst,                       arow + p, (SCREENWIDTH - p) * sizeof(uint16_t));
        memcpy(dst + (SCREENWIDTH - p),   brow,     p                  * sizeof(uint16_t));
    } else {
        // b slides in from the left; a slides out to the right.
        // dst = brow[W-p .. W-1] then arow[0 .. W-1-p]
        memcpy(dst,                       brow + (SCREENWIDTH - p), p * sizeof(uint16_t));
        memcpy(dst + p,                   arow,                     (SCREENWIDTH - p) * sizeof(uint16_t));
    }
}

void gui_draw_frame(const uint16_t *a, const uint16_t *b, int slide_px, int direction)
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

        compose_row(dst, y, a, b, slide_px, direction);

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
        if (!Frens::isFrameBufferUsed())
#endif
            dvi_->setLineBuffer(y, lb);
#endif
    }
}

} // extern "C"
