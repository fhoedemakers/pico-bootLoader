// Streaming PNG/JPEG -> .444/.555 converter.
//
// The pipeline is deliberately allocation-light so it fits in SRAM (no PSRAM
// required). One conversion:
//   * open the source file via a FatFs-backed callback (no whole-image RAM buffer),
//   * feed rows/MCUs from PNGdec / JPEGDEC into a per-output-row RGB accumulator,
//   * pack each completed output row as .444 + .555 and stream both to disk
//     line-by-line (with atomic .tmp + f_rename finalize).
//
// Peak SRAM footprint per conversion is dominated by the decoder state:
// ~53 KB for PNGdec (32 KB inflate window + two de-filter lines sized for
// 1280-px sources), ~18 KB for JPEGDEC. Boards without PSRAM run all
// conversions at boot (see main.cpp), before the ~190 KB of GUI slide
// buffers claim the heap; PSRAM boards keep the scratch in lwmem and can
// convert at any time.

#include "image_convert.h"

#include "FrensHelpers.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <new>

#include "ff.h"
#include "FrensFonts.h"
#include "progress_bar.h"

#define __LINUX__ 1  // route PNGdec / JPEGDEC away from <Arduino.h>

// PNG_MAX_BUFFERED_PIXELS is set in CMakeLists.txt (project-wide -D) so that
// PNGdec.cpp and this file agree on sizeof(PNGIMAGE). Do NOT #define it here:
// an in-file value would win in this translation unit only, PNG::open()'s
// memset(sizeof(PNGIMAGE)) in PNGdec.cpp would then run past our smaller
// f_malloc block, and the zeroed heap metadata bricks every later allocation.
// The static_assert below keeps the CMake value honest.

#include "third_party/PNGdec/PNGdec.h"
#include "third_party/JPEGDEC/JPEGDEC.h"

namespace {

constexpr uint16_t kMaxSrcW = 1280;
constexpr uint16_t kMaxSrcH = 960;
// PNGdec keeps BOTH the current and previous de-filter lines (each
// iPitch + 1 bytes, 16-byte aligned) inside ucPixels[PNG_MAX_BUFFERED_PIXELS],
// and its own too-big check (png.inl, PNG_TOO_BIG) only compares a single
// pitch against the whole buffer. The buffer must cover two lines of the
// widest source we accept: kMaxSrcW pixels at 4 bytes/px (8-bit channels;
// PNGdec rejects 16-bit and interlaced PNGs at open()).
static_assert(PNG_MAX_BUFFERED_PIXELS >= (kMaxSrcW * 4 + 1) * 2 + 32,
              "PNG_MAX_BUFFERED_PIXELS (set in CMakeLists.txt) must cover two de-filter lines of kMaxSrcW RGBA pixels");

// ---------------------------------------------------------------------------
// Shared streaming state. Held in a file-scope struct so PNG/JPEG callbacks
// (which only get a void* user pointer) can reach it without threading state
// through the decoder API.
// ---------------------------------------------------------------------------
struct ConvState {
    // Source
    int src_w, src_h;

    // Output geometry (dst_w x dst_h scaled image, off_x/off_y into canvas_w x canvas_h)
    int dst_w, dst_h;
    int off_x, off_y;
    int canvas_w, canvas_h;

    // Per-output-row RGB accumulator (dst_w cells)
    uint32_t *sumR, *sumG, *sumB;
    uint16_t *count;

    // Packed output row buffers (canvas_w u16 each). Left/right letterbox
    // padding is zeroed once at setup and never touched, so we only write
    // the middle [off_x, off_x+dst_w) each row.
    uint16_t *row_444;
    uint16_t *row_555;

    // Pre-zeroed rows for top/bottom letterbox padding.
    uint16_t *black_row_444;
    uint16_t *black_row_555;

    // Source RGB565 line scratch (src_w u16), used by PNG path only.
    uint16_t *src_line565;

    // Current output row being accumulated (or the next one to flush).
    int cur_out_y;

    // Output files (open concurrently).
    FIL fil_444, fil_555;
    bool fil_444_open, fil_555_open;

    // Sticky error flag; once set, callbacks fast-return without further work.
    bool ok;
};

ConvState g_st{};  // zero-initialised

// ---------------------------------------------------------------------------
// FatFs bridges for PNGdec / JPEGDEC file callbacks.
// ---------------------------------------------------------------------------
// Shared by the PNG and JPEG open callbacks. PNGdec's open() returns
// PNG_SUCCESS when this returns NULL (zeroed state, 0x0 dims), so log the
// real cause here -- the caller can't tell.
void *src_open_cb(const char *fname, int32_t *piSize)
{
    FIL *fp = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (!fp) {
        printf("[bootLoader] image_convert: FIL alloc (%u B) failed for %s\n",
               (unsigned)sizeof(FIL), fname);
        Frens::dumpHeapStats("src FIL alloc failed");
        return nullptr;
    }
    FRESULT fr = f_open(fp, fname, FA_READ);
    if (fr != FR_OK) {
        printf("[bootLoader] image_convert: f_open(%s) failed (fr=%d)\n", fname, (int)fr);
        Frens::f_free(fp);
        return nullptr;
    }
    *piSize = (int32_t)f_size(fp);
    return fp;
}

void *png_open_cb(const char *fname, int32_t *piSize)
{
    return src_open_cb(fname, piSize);
}
void png_close_cb(void *handle)
{
    if (!handle) return;
    FIL *fp = (FIL *)handle;
    f_close(fp);
    Frens::f_free(fp);
}
int32_t png_read_cb(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    FIL *fp = (FIL *)pFile->fHandle;
    UINT br = 0;
    if (f_read(fp, pBuf, (UINT)iLen, &br) != FR_OK) return 0;
    return (int32_t)br;
}
int32_t png_seek_cb(PNGFILE *pFile, int32_t iPosition)
{
    FIL *fp = (FIL *)pFile->fHandle;
    if (f_lseek(fp, (FSIZE_t)iPosition) != FR_OK) return -1;
    return iPosition;
}

void *jpg_open_cb(const char *fname, int32_t *piSize)
{
    return src_open_cb(fname, piSize);
}
void jpg_close_cb(void *handle)
{
    if (!handle) return;
    FIL *fp = (FIL *)handle;
    f_close(fp);
    Frens::f_free(fp);
}
int32_t jpg_read_cb(JPEGFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
    FIL *fp = (FIL *)pFile->fHandle;
    UINT br = 0;
    if (f_read(fp, pBuf, (UINT)iLen, &br) != FR_OK) return 0;
    return (int32_t)br;
}
int32_t jpg_seek_cb(JPEGFILE *pFile, int32_t iPosition)
{
    FIL *fp = (FIL *)pFile->fHandle;
    if (f_lseek(fp, (FSIZE_t)iPosition) != FR_OK) return -1;
    return iPosition;
}

// ---------------------------------------------------------------------------
// Filesystem helpers.
// ---------------------------------------------------------------------------
bool file_exists(const char *path)
{
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

// How a conversion attempt ended. Transient failures (out of heap, SD I/O)
// are worth retrying on a later boot; Unsupported means the file itself can
// never convert (progressive JPEG, interlaced PNG, corrupt data, too large)
// and gets quarantined so scans stop retrying it forever.
enum class ConvStatus { Ok, Transient, Unsupported };

// Move a permanently unconvertible source into <dir>/unsupported/.
void move_to_unsupported(const char *dir, const char *src_path)
{
    char sub[FF_MAX_LFN + 1];
    snprintf(sub, sizeof(sub), "%s/unsupported", dir);
    FRESULT fr = f_mkdir(sub);
    if (fr != FR_OK && fr != FR_EXIST) {
        printf("[bootLoader] image_convert: cannot create %s (fr=%d)\n", sub, (int)fr);
        return;
    }
    const char *name = strrchr(src_path, '/');
    name = name ? name + 1 : src_path;
    char dst[FF_MAX_LFN + 1];
    snprintf(dst, sizeof(dst), "%s/%s", sub, name);
    f_unlink(dst);   // replace any stale copy from an earlier run
    fr = f_rename(src_path, dst);
    if (fr == FR_OK) {
        printf("[bootLoader] image_convert: moved %s -> %s (won't retry)\n", src_path, dst);
    } else {
        printf("[bootLoader] image_convert: move %s -> %s failed (fr=%d)\n",
               src_path, dst, (int)fr);
    }
}

// Open <dir>/<base><ext>.tmp for streaming write and emit the 4-byte header.
bool open_stream_out(FIL *fil, const char *dir, const char *base, const char *ext,
                     uint16_t w, uint16_t h, char *tmp_path_out, size_t tmp_path_sz)
{
    snprintf(tmp_path_out, tmp_path_sz, "%s/%s%s.tmp", dir, base, ext);
    if (f_open(fil, tmp_path_out, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[bootLoader] image_convert: cannot open %s for write\n", tmp_path_out);
        return false;
    }
    uint16_t hdr[2] = { w, h };
    UINT bw = 0;
    if (f_write(fil, hdr, sizeof(hdr), &bw) != FR_OK || bw != sizeof(hdr)) {
        // f_open() succeeded but header write didn't -- close the file so the
        // caller doesn't have to track a half-open FIL.
        f_close(fil);
        return false;
    }
    return true;
}

// Rename <tmp> -> <dir>/<base><ext>, replacing any existing file.
bool commit_stream_out(const char *dir, const char *base, const char *ext,
                       const char *tmp_path)
{
    char final_path[FF_MAX_LFN + 1];
    snprintf(final_path, sizeof(final_path), "%s/%s%s", dir, base, ext);
    f_unlink(final_path);
    if (f_rename(tmp_path, final_path) != FR_OK) {
        printf("[bootLoader] image_convert: rename %s -> %s failed\n", tmp_path, final_path);
        f_unlink(tmp_path);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Row accumulator management.
// ---------------------------------------------------------------------------
void reset_row_accumulator()
{
    memset(g_st.sumR,  0, sizeof(uint32_t) * g_st.dst_w);
    memset(g_st.sumG,  0, sizeof(uint32_t) * g_st.dst_w);
    memset(g_st.sumB,  0, sizeof(uint32_t) * g_st.dst_w);
    memset(g_st.count, 0, sizeof(uint16_t) * g_st.dst_w);
}

// Pack the current accumulator into row_444/row_555 (at the letterbox offset)
// and write both to disk.
bool flush_current_row()
{
    const int off = g_st.off_x;
    for (int x = 0; x < g_st.dst_w; x++) {
        uint32_t c = g_st.count[x];
        uint8_t R, G, B;
        if (c == 0) { R = G = B = 0; }
        else {
            R = (uint8_t)(g_st.sumR[x] / c);
            G = (uint8_t)(g_st.sumG[x] / c);
            B = (uint8_t)(g_st.sumB[x] / c);
        }
        g_st.row_444[off + x] = (uint16_t)(((R >> 4) << 8) | ((G >> 4) << 4) |  (B >> 4));
        g_st.row_555[off + x] = (uint16_t)(((R >> 3) << 10)| ((G >> 3) << 5) |  (B >> 3));
    }
    UINT want = (UINT)g_st.canvas_w * sizeof(uint16_t);
    UINT bw = 0;
    if (f_write(&g_st.fil_444, g_st.row_444, want, &bw) != FR_OK || bw != want) return false;
    if (f_write(&g_st.fil_555, g_st.row_555, want, &bw) != FR_OK || bw != want) return false;
    reset_row_accumulator();
    return true;
}

bool write_black_row()
{
    UINT want = (UINT)g_st.canvas_w * sizeof(uint16_t);
    UINT bw = 0;
    if (f_write(&g_st.fil_444, g_st.black_row_444, want, &bw) != FR_OK || bw != want) return false;
    if (f_write(&g_st.fil_555, g_st.black_row_555, want, &bw) != FR_OK || bw != want) return false;
    return true;
}

// Advance emission until g_st.cur_out_y >= target_out_y. Each advance
// flushes the current accumulator (which is now "closed") to disk.
bool advance_to_row(int target_out_y)
{
    while (g_st.cur_out_y < target_out_y && g_st.cur_out_y < g_st.dst_h) {
        if (!flush_current_row()) return false;
        g_st.cur_out_y++;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PNG draw callback: one source scanline delivered at pDraw->y in row-major
// order (0..src_h-1). Deferred pixel-type -> RGB565 -> RGB888 conversion via
// getLineAsRGB565 keeps the code uniform across truecolor/indexed/grayscale;
// the final pack is 5- or 4-bit per channel anyway.
// ---------------------------------------------------------------------------
PNG *g_png = nullptr;  // set for the duration of decode() so callback can call getLineAsRGB565

int png_draw_cb_fn(PNGDRAW *pDraw)
{
    if (!g_st.ok) return 0;

    const int sy = pDraw->y;
    const int yo = (sy * g_st.dst_h) / g_st.src_h;

    // Flush any completed output rows before this source line contributes.
    if (yo > g_st.cur_out_y) {
        if (!advance_to_row(yo)) { g_st.ok = false; return 0; }
    }
    if (g_st.cur_out_y >= g_st.dst_h) return 0;

    // Convert this source line to RGB565 uniformly.
    g_png->getLineAsRGB565(pDraw, g_st.src_line565, PNG_RGB565_LITTLE_ENDIAN, 0);

    // Accumulate into the current output-row cells.
    const int src_w = g_st.src_w;
    const int dst_w = g_st.dst_w;
    for (int sx = 0; sx < src_w; sx++) {
        int xo = (sx * dst_w) / src_w;
        if (xo >= dst_w) xo = dst_w - 1;
        uint16_t p = g_st.src_line565[sx];
        uint8_t r5 = (p >> 11) & 0x1F;
        uint8_t g6 = (p >>  5) & 0x3F;
        uint8_t b5 =  p        & 0x1F;
        uint8_t R = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t G = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t B = (uint8_t)((b5 << 3) | (b5 >> 2));
        g_st.sumR[xo] += R;
        g_st.sumG[xo] += G;
        g_st.sumB[xo] += B;
        g_st.count[xo]++;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// JPEG draw callback: one MCU rectangle delivered in RGB8888
// (byte order R,G,B,A per pixel). We accumulate pixel-by-pixel; no strip
// buffer needed since accumulators are indexed by output column.
// ---------------------------------------------------------------------------
int jpg_draw_cb_fn(JPEGDRAW *pDraw)
{
    if (!g_st.ok) return 0;

    const int src_w = g_st.src_w;
    const int src_h = g_st.src_h;
    const int dst_w = g_st.dst_w;
    const int dst_h = g_st.dst_h;

    // pPixels laid out as 4 bytes/px (R,G,B,A). Row pitch in pixels = iWidth
    // for the rect callback (contiguous strip).
    const uint8_t *px = (const uint8_t *)pDraw->pPixels;
    const int rect_w = pDraw->iWidthUsed ? pDraw->iWidthUsed : pDraw->iWidth;
    const int rect_h = pDraw->iHeight;
    const int rect_x = pDraw->x;
    const int rect_y = pDraw->y;
    const int stride_bytes = pDraw->iWidth * 4;   // full rect stride (uncropped)

    for (int r = 0; r < rect_h; r++) {
        const int sy = rect_y + r;
        if (sy >= src_h) break;
        const int yo = (sy * dst_h) / src_h;
        if (yo > g_st.cur_out_y) {
            if (!advance_to_row(yo)) { g_st.ok = false; return 0; }
        }
        if (g_st.cur_out_y >= dst_h) return 0;

        const uint8_t *row = px + (size_t)r * stride_bytes;
        for (int c = 0; c < rect_w; c++) {
            const int sx = rect_x + c;
            if (sx >= src_w) break;
            int xo = (sx * dst_w) / src_w;
            if (xo >= dst_w) xo = dst_w - 1;
            g_st.sumR[xo] += row[c * 4 + 0];
            g_st.sumG[xo] += row[c * 4 + 1];
            g_st.sumB[xo] += row[c * 4 + 2];
            g_st.count[xo]++;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Per-conversion buffer alloc/free. All allocations routed through Frens::
// f_malloc so PSRAM builds land in lwmem and SRAM-only builds land in libc.
// ---------------------------------------------------------------------------
void free_conv_buffers()
{
    Frens::f_free(g_st.sumR);          g_st.sumR = nullptr;
    Frens::f_free(g_st.sumG);          g_st.sumG = nullptr;
    Frens::f_free(g_st.sumB);          g_st.sumB = nullptr;
    Frens::f_free(g_st.count);         g_st.count = nullptr;
    Frens::f_free(g_st.row_444);       g_st.row_444 = nullptr;
    Frens::f_free(g_st.row_555);       g_st.row_555 = nullptr;
    Frens::f_free(g_st.black_row_444); g_st.black_row_444 = nullptr;
    Frens::f_free(g_st.black_row_555); g_st.black_row_555 = nullptr;
    Frens::f_free(g_st.src_line565);   g_st.src_line565 = nullptr;
}

bool alloc_conv_buffers()
{
    const size_t dw = (size_t)g_st.dst_w;
    const size_t cw = (size_t)g_st.canvas_w;
    const size_t sw = (size_t)g_st.src_w;
    g_st.sumR  = (uint32_t *)Frens::f_malloc(dw * sizeof(uint32_t));
    g_st.sumG  = (uint32_t *)Frens::f_malloc(dw * sizeof(uint32_t));
    g_st.sumB  = (uint32_t *)Frens::f_malloc(dw * sizeof(uint32_t));
    g_st.count = (uint16_t *)Frens::f_malloc(dw * sizeof(uint16_t));
    g_st.row_444 = (uint16_t *)Frens::f_malloc(cw * sizeof(uint16_t));
    g_st.row_555 = (uint16_t *)Frens::f_malloc(cw * sizeof(uint16_t));
    g_st.black_row_444 = (uint16_t *)Frens::f_malloc(cw * sizeof(uint16_t));
    g_st.black_row_555 = (uint16_t *)Frens::f_malloc(cw * sizeof(uint16_t));
    g_st.src_line565   = (uint16_t *)Frens::f_malloc(sw * sizeof(uint16_t));
    if (!g_st.sumR || !g_st.sumG || !g_st.sumB || !g_st.count ||
        !g_st.row_444 || !g_st.row_555 ||
        !g_st.black_row_444 || !g_st.black_row_555 ||
        !g_st.src_line565) {
        free_conv_buffers();
        return false;
    }
    // Pre-zero row buffers so letterbox left/right pad is opaque black without
    // per-row work. Black-row buffers stay zero for their whole lifetime.
    memset(g_st.row_444,        0, cw * sizeof(uint16_t));
    memset(g_st.row_555,        0, cw * sizeof(uint16_t));
    memset(g_st.black_row_444,  0, cw * sizeof(uint16_t));
    memset(g_st.black_row_555,  0, cw * sizeof(uint16_t));
    reset_row_accumulator();
    return true;
}

// ---------------------------------------------------------------------------
// Peek at source dimensions and geometry BEFORE opening output files or
// allocating buffers. Fills g_st.src_w/h/dst_w/h/off_x/y/canvas_w/h.
// Returns 0 = ok, 1 = source too big, 2 = decode error, 3 = unsupported.
// ---------------------------------------------------------------------------
int compute_geometry(int src_w, int src_h, uint16_t max_w, uint16_t max_h, bool letterbox)
{
    if (src_w <= 0 || src_h <= 0) return 2;
    if (src_w > (int)kMaxSrcW || src_h > (int)kMaxSrcH) return 1;

    // Don't upscale; smaller sources stay 1:1.
    float sx = (float)max_w / (float)src_w;
    float sy = (float)max_h / (float)src_h;
    float scale = sx < sy ? sx : sy;
    if (scale > 1.0f) scale = 1.0f;
    int dst_w = (int)((float)src_w * scale + 0.5f);
    int dst_h = (int)((float)src_h * scale + 0.5f);
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (dst_w > (int)max_w) dst_w = (int)max_w;
    if (dst_h > (int)max_h) dst_h = (int)max_h;

    g_st.src_w = src_w;
    g_st.src_h = src_h;
    g_st.dst_w = dst_w;
    g_st.dst_h = dst_h;
    if (letterbox) {
        g_st.canvas_w = (int)max_w;
        g_st.canvas_h = (int)max_h;
        g_st.off_x    = (g_st.canvas_w - dst_w) / 2;
        g_st.off_y    = (g_st.canvas_h - dst_h) / 2;
    } else {
        g_st.canvas_w = dst_w;
        g_st.canvas_h = dst_h;
        g_st.off_x    = 0;
        g_st.off_y    = 0;
    }
    g_st.cur_out_y = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Common finalize path: closes both output FILs, and either commits both
// via .tmp -> final rename, or unlinks both .tmp files on failure.
// ---------------------------------------------------------------------------
bool finalize_outputs(bool ok, const char *dir, const char *base,
                      const char *tmp_444, const char *tmp_555)
{
    if (g_st.fil_444_open) { f_close(&g_st.fil_444); g_st.fil_444_open = false; }
    if (g_st.fil_555_open) { f_close(&g_st.fil_555); g_st.fil_555_open = false; }
    if (!ok) {
        f_unlink(tmp_444);
        f_unlink(tmp_555);
        return false;
    }
    bool ok_a = commit_stream_out(dir, base, ".444", tmp_444);
    bool ok_b = commit_stream_out(dir, base, ".555", tmp_555);
    return ok_a && ok_b;
}

// ---------------------------------------------------------------------------
// Prepare g_st and open both .444.tmp / .555.tmp with headers + top padding.
// ---------------------------------------------------------------------------
bool setup_output_streams(const char *dir, const char *base,
                          char *tmp_444, size_t tmp_444_sz,
                          char *tmp_555, size_t tmp_555_sz)
{
    if (!open_stream_out(&g_st.fil_444, dir, base, ".444",
                         (uint16_t)g_st.canvas_w, (uint16_t)g_st.canvas_h,
                         tmp_444, tmp_444_sz)) {
        return false;
    }
    g_st.fil_444_open = true;

    if (!open_stream_out(&g_st.fil_555, dir, base, ".555",
                         (uint16_t)g_st.canvas_w, (uint16_t)g_st.canvas_h,
                         tmp_555, tmp_555_sz)) {
        return false;
    }
    g_st.fil_555_open = true;

    // Top letterbox padding.
    for (int y = 0; y < g_st.off_y; y++) {
        if (!write_black_row()) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// PNG path.
// ---------------------------------------------------------------------------
ConvStatus convert_png(const char *src_path, const char *dir, const char *base,
                       uint16_t max_w, uint16_t max_h, bool letterbox)
{
    void *png_mem = Frens::f_malloc(sizeof(PNG));
    if (!png_mem) {
        printf("[bootLoader] image_convert: PNG state alloc %u B failed\n",
               (unsigned)sizeof(PNG));
        Frens::dumpHeapStats("png state alloc failed");
        return ConvStatus::Transient;
    }
    PNG *png = new (png_mem) PNG();
    g_png = png;

    int rc = png->open(src_path, png_open_cb, png_close_cb,
                       png_read_cb, png_seek_cb, png_draw_cb_fn);
    // Quirk: PNG::open() returns 0 (== PNG_SUCCESS) when the open CALLBACK
    // fails, leaving the whole state zeroed. Catch that as 0x0 dims here;
    // src_open_cb already logged the underlying cause.
    if (rc != PNG_SUCCESS || png->getWidth() <= 0 || png->getHeight() <= 0) {
        printf("[bootLoader] image_convert: %s PNG open failed (err=%d, lastError=%d)\n",
               src_path, rc, png->getLastError());
        // rc == PNG_SUCCESS here means the open CALLBACK failed (heap / SD;
        // src_open_cb logged the cause) -- retryable. A nonzero parse error
        // condemns the file itself, except the resource-level codes.
        ConvStatus st = ConvStatus::Transient;
        if (rc == PNG_UNSUPPORTED_FEATURE || rc == PNG_INVALID_FILE ||
            rc == PNG_TOO_BIG || rc == PNG_DECODE_ERROR) {
            st = ConvStatus::Unsupported;
        }
        png->close();
        png->~PNG();
        Frens::f_free(png_mem);
        g_png = nullptr;
        return st;
    }

    int src_w = png->getWidth();
    int src_h = png->getHeight();
    int geo   = compute_geometry(src_w, src_h, max_w, max_h, letterbox);
    if (geo != 0) {
        if (geo == 1) {
            printf("[bootLoader] image_convert: %s too large (%dx%d, max %ux%u)\n",
                   src_path, src_w, src_h, (unsigned)kMaxSrcW, (unsigned)kMaxSrcH);
        } else {
            printf("[bootLoader] image_convert: %s bad dims %dx%d\n",
                   src_path, src_w, src_h);
        }
        png->close();
        png->~PNG();
        Frens::f_free(png_mem);
        g_png = nullptr;
        return ConvStatus::Unsupported;
    }
    if (png->isInterlaced()) {
        printf("[bootLoader] image_convert: %s is interlaced (not supported), skipped\n",
               src_path);
        png->close();
        png->~PNG();
        Frens::f_free(png_mem);
        g_png = nullptr;
        return ConvStatus::Unsupported;
    }

    printf("[bootLoader] image_convert: %s: %dx%d PNG (bpp=%d ptype=%d) -> %dx%d %s\n",
           src_path, src_w, src_h, png->getBpp(), png->getPixelType(),
           g_st.canvas_w, g_st.canvas_h, letterbox ? "letterboxed" : "unpadded");

    if (!alloc_conv_buffers()) {
        printf("[bootLoader] image_convert: buffer alloc failed\n");
        png->close();
        png->~PNG();
        Frens::f_free(png_mem);
        g_png = nullptr;
        return ConvStatus::Transient;
    }

    char tmp_444[FF_MAX_LFN + 1];
    char tmp_555[FF_MAX_LFN + 1];
    if (!setup_output_streams(dir, base,
                              tmp_444, sizeof(tmp_444),
                              tmp_555, sizeof(tmp_555))) {
        (void)finalize_outputs(false, dir, base, tmp_444, tmp_555);
        free_conv_buffers();
        png->close();
        png->~PNG();
        Frens::f_free(png_mem);
        g_png = nullptr;
        return ConvStatus::Transient;
    }

    g_st.ok = true;
    uint32_t t_dec_start = Frens::time_ms();
    rc = png->decode(nullptr, 0);
    png->close();
    png->~PNG();
    Frens::f_free(png_mem);
    g_png = nullptr;

    bool ok = g_st.ok && (rc == PNG_SUCCESS);
    if (ok) {
        // Flush any not-yet-emitted output rows.
        ok = advance_to_row(g_st.dst_h);
        // Bottom letterbox padding.
        if (ok) {
            for (int y = g_st.off_y + g_st.dst_h; y < g_st.canvas_h; y++) {
                if (!write_black_row()) { ok = false; break; }
            }
        }
    } else if (rc != PNG_SUCCESS) {
        printf("[bootLoader] image_convert: %s PNG decode err=%d\n", src_path, rc);
    }

    printf("[bootLoader] image_convert: %s: decode+write %u ms\n",
           src_path, (unsigned)(Frens::time_ms() - t_dec_start));

    bool committed = finalize_outputs(ok, dir, base, tmp_444, tmp_555);
    free_conv_buffers();
    if (committed) return ConvStatus::Ok;
    // A decode error means corrupt/truncated PNG data -- the file will never
    // convert. Row-write / finalize failures are SD I/O and worth a retry.
    return (rc != PNG_SUCCESS) ? ConvStatus::Unsupported : ConvStatus::Transient;
}

// ---------------------------------------------------------------------------
// JPEG path.
// ---------------------------------------------------------------------------
ConvStatus convert_jpg(const char *src_path, const char *dir, const char *base,
                       uint16_t max_w, uint16_t max_h, bool letterbox)
{
    void *jpg_mem = Frens::f_malloc(sizeof(JPEGDEC));
    if (!jpg_mem) {
        printf("[bootLoader] image_convert: JPEG state alloc %u B failed\n",
               (unsigned)sizeof(JPEGDEC));
        Frens::dumpHeapStats("jpeg state alloc failed");
        return ConvStatus::Transient;
    }
    JPEGDEC *jpg = new (jpg_mem) JPEGDEC();

    int rc = jpg->open(src_path, jpg_open_cb, jpg_close_cb,
                       jpg_read_cb, jpg_seek_cb, jpg_draw_cb_fn);
    if (rc == 0) {
        int le = jpg->getLastError();
        printf("[bootLoader] image_convert: %s JPEG open failed (err=%d)\n",
               src_path, le);
        // lastError == JPEG_SUCCESS means the open CALLBACK failed (heap /
        // SD; src_open_cb logged the cause) -- retryable. A parse error
        // condemns the file itself.
        ConvStatus st = (le == JPEG_SUCCESS) ? ConvStatus::Transient
                                             : ConvStatus::Unsupported;
        jpg->close();   // frees the FIL when open() failed after the file opened
        jpg->~JPEGDEC();
        Frens::f_free(jpg_mem);
        return st;
    }
    if (jpg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
        printf("[bootLoader] image_convert: %s is progressive JPEG (not supported), skipped\n",
               src_path);
        jpg->close();
        jpg->~JPEGDEC();
        Frens::f_free(jpg_mem);
        return ConvStatus::Unsupported;
    }

    int src_w = jpg->getWidth();
    int src_h = jpg->getHeight();
    int geo   = compute_geometry(src_w, src_h, max_w, max_h, letterbox);
    if (geo != 0) {
        if (geo == 1) {
            printf("[bootLoader] image_convert: %s too large (%dx%d, max %ux%u)\n",
                   src_path, src_w, src_h, (unsigned)kMaxSrcW, (unsigned)kMaxSrcH);
        } else {
            printf("[bootLoader] image_convert: %s bad dims %dx%d\n",
                   src_path, src_w, src_h);
        }
        jpg->close();
        jpg->~JPEGDEC();
        Frens::f_free(jpg_mem);
        return ConvStatus::Unsupported;
    }

    printf("[bootLoader] image_convert: %s: %dx%d JPEG -> %dx%d %s\n",
           src_path, src_w, src_h, g_st.canvas_w, g_st.canvas_h,
           letterbox ? "letterboxed" : "unpadded");

    if (!alloc_conv_buffers()) {
        printf("[bootLoader] image_convert: buffer alloc failed\n");
        jpg->close();
        jpg->~JPEGDEC();
        Frens::f_free(jpg_mem);
        return ConvStatus::Transient;
    }

    jpg->setPixelType(RGB8888);

    char tmp_444[FF_MAX_LFN + 1];
    char tmp_555[FF_MAX_LFN + 1];
    if (!setup_output_streams(dir, base,
                              tmp_444, sizeof(tmp_444),
                              tmp_555, sizeof(tmp_555))) {
        (void)finalize_outputs(false, dir, base, tmp_444, tmp_555);
        free_conv_buffers();
        jpg->close();
        jpg->~JPEGDEC();
        Frens::f_free(jpg_mem);
        return ConvStatus::Transient;
    }

    g_st.ok = true;
    uint32_t t_dec_start = Frens::time_ms();
    rc = jpg->decode(0, 0, 0);
    jpg->close();
    jpg->~JPEGDEC();
    Frens::f_free(jpg_mem);

    bool ok = g_st.ok && (rc != 0);
    if (ok) {
        ok = advance_to_row(g_st.dst_h);
        if (ok) {
            for (int y = g_st.off_y + g_st.dst_h; y < g_st.canvas_h; y++) {
                if (!write_black_row()) { ok = false; break; }
            }
        }
    }

    printf("[bootLoader] image_convert: %s: decode+write %u ms\n",
           src_path, (unsigned)(Frens::time_ms() - t_dec_start));

    bool committed = finalize_outputs(ok, dir, base, tmp_444, tmp_555);
    free_conv_buffers();
    if (committed) return ConvStatus::Ok;
    // A decode error means corrupt/truncated JPEG data -- the file will
    // never convert. Row-write / finalize failures are SD I/O and worth a
    // retry.
    return (rc == 0) ? ConvStatus::Unsupported : ConvStatus::Transient;
}

// ---------------------------------------------------------------------------
// Batch-conversion progress UI. Draws directly into the active framebuffer
// (HSTX or PicoDVI); returns NULL when neither is available (text-only
// PicoDVI runs), in which case the batch runs silently.
// ---------------------------------------------------------------------------
uint16_t *ic_progress_fb()
{
#if HSTX
    return (uint16_t *)hstx_getframebuffer();
#else
  #if FRAMEBUFFERISPOSSIBLE
    if (Frens::isFrameBufferUsed()) return Frens::framebuffer;
  #endif
    return nullptr;
#endif
}

void ic_fb_draw_text_centered(uint16_t *fb, int y_top, const char *text,
                              uint16_t fg, uint16_t bg)
{
    if (!fb || !text) return;
    int len = (int)strlen(text);
    if (len == 0) return;
    int max_chars = SCREENWIDTH / FONT_CHAR_WIDTH;
    if (len > max_chars) len = max_chars;
    int text_w  = len * FONT_CHAR_WIDTH;
    int x_start = (SCREENWIDTH - text_w) / 2;
    if (x_start < 0) x_start = 0;
    for (int row = 0; row < FONT_CHAR_HEIGHT; row++) {
        int y = y_top + row;
        if (y < 0 || y >= SCREENHEIGHT) continue;
        uint16_t *dst = fb + y * SCREENWIDTH + x_start;
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)text[i];
            if (c < FONT_FIRST_ASCII || c >= FONT_FIRST_ASCII + FONT_N_CHARS) c = '?';
            char slice = getcharslicefrom8x8font((char)c, row);
            for (int bit = 0; bit < 8; bit++) {
                dst[bit] = (slice & 1) ? fg : bg;
                slice >>= 1;
            }
            dst += FONT_CHAR_WIDTH;
        }
    }
}

// FNV-1a 64-bit over the lowercased basename. FAT names are case-insensitive,
// so hash them case-folded to match f_stat's semantics.
uint64_t ic_name_hash(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool ic_hash_contains(const uint64_t *arr, int n, uint64_t h)
{
    for (int i = 0; i < n; i++) {
        if (arr[i] == h) return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------------

// Head...tail elision so a long filename fits a fixed-width text column. Used
// by the conversion progress screen below and by the picker's error screen.
void ic_truncate_for_display(const char *name, char *out, int max_chars)
{
    int n = (int)strlen(name);
    if (n <= max_chars) { memcpy(out, name, n); out[n] = '\0'; return; }
    int head = (max_chars - 3) / 2;
    int tail = max_chars - 3 - head;
    memcpy(out, name, head);
    memcpy(out + head, "...", 3);
    memcpy(out + head + 3, name + n - tail, tail);
    out[max_chars] = '\0';
}

int image_convert_batch_dir(const char *dir, uint16_t max_w, uint16_t max_h,
                            bool letterbox, const char *ui_title)
{
    // Practical cap on user-supplied files -- above this we log + stop rather
    // than allocate unbounded scratch.
    const int MAX_QUEUE = 100;
    const size_t name_slot = (size_t)(FF_MAX_LFN + 1);
    char (*queue)[FF_MAX_LFN + 1] = (char (*)[FF_MAX_LFN + 1])
        Frens::f_malloc((size_t)MAX_QUEUE * name_slot);
    if (!queue) {
        Frens::dumpHeapStats("batch queue alloc failed");
        return -1;
    }

    // Cache presence is matched in memory rather than f_stat-probing per
    // source: f_stat re-walks the directory from its first sector on every
    // call, which turns the scan into O(n^2) SD reads -- seconds per boot on
    // a well-filled screensaver dir even with nothing left to convert. Two
    // sequential f_readdir passes read the directory data once each instead.
    const int MAX_CACHE_HASHES = 512;
    uint64_t *h444 = (uint64_t *)Frens::f_malloc(
        2u * MAX_CACHE_HASHES * sizeof(uint64_t));
    if (!h444) {
        Frens::dumpHeapStats("batch hash alloc failed");
        Frens::f_free(queue);
        return -1;
    }
    uint64_t *h555 = h444 + MAX_CACHE_HASHES;

    DIR d;
    if (f_opendir(&d, dir) != FR_OK) {
        Frens::f_free(h444);
        Frens::f_free(queue);
        return -1;
    }

    // Pass 1: record which .444 / .555 cache basenames exist.
    int  n444 = 0, n555 = 0;
    bool hash_overflow = false;
    FILINFO fno;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        const char *name = fno.fname;
        size_t nn = strlen(name);
        if (nn <= 4) continue;
        if (strcasecmp(name + nn - 4, ".444") == 0) {
            if (n444 >= MAX_CACHE_HASHES) { hash_overflow = true; continue; }
            h444[n444++] = ic_name_hash(name, nn - 4);
        } else if (strcasecmp(name + nn - 4, ".555") == 0) {
            if (n555 >= MAX_CACHE_HASHES) { hash_overflow = true; continue; }
            h555[n555++] = ic_name_hash(name, nn - 4);
        }
    }

    // Pass 2: queue every source that lacks one of its caches.
    f_readdir(&d, nullptr);   // rewind
    int nqueue = 0;
    while (f_readdir(&d, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR) continue;
        const char *name = fno.fname;
        size_t nn = strlen(name);
        size_t ext_len = 0;
        if      (nn > 4 && strcasecmp(name + nn - 4, ".png")  == 0) ext_len = 4;
        else if (nn > 4 && strcasecmp(name + nn - 4, ".jpg")  == 0) ext_len = 4;
        else if (nn > 5 && strcasecmp(name + nn - 5, ".jpeg") == 0) ext_len = 5;
        else continue;

        size_t base_len = nn - ext_len;
        if (base_len == 0 || base_len >= sizeof(queue[0])) continue;

        uint64_t h = ic_name_hash(name, base_len);
        bool has_444 = ic_hash_contains(h444, n444, h);
        bool has_555 = ic_hash_contains(h555, n555, h);
        if ((!has_444 || !has_555) && hash_overflow) {
            // The hash index is partial (dir has > MAX_CACHE_HASHES cache
            // files); confirm with f_stat before queueing spuriously.
            char base[FF_MAX_LFN + 1];
            memcpy(base, name, base_len);
            base[base_len] = '\0';
            char probe[FF_MAX_LFN + 1];
            FILINFO fi;
            if (!has_444) {
                snprintf(probe, sizeof(probe), "%s/%s.444", dir, base);
                has_444 = f_stat(probe, &fi) == FR_OK;
            }
            if (!has_555) {
                snprintf(probe, sizeof(probe), "%s/%s.555", dir, base);
                has_555 = f_stat(probe, &fi) == FR_OK;
            }
        }
        if (has_444 && has_555) continue;

        if (nqueue >= MAX_QUEUE) {
            printf("[bootLoader] image_convert: batch queue full (>%d), skipping rest\n",
                   MAX_QUEUE);
            break;
        }
        memcpy(queue[nqueue], name, base_len);
        queue[nqueue][base_len] = '\0';
        nqueue++;
    }
    f_closedir(&d);
    Frens::f_free(h444);

    if (nqueue == 0) {
        printf("[bootLoader] image_convert: no PNG/JPG images need conversion in %s\n", dir);
        Frens::f_free(queue);
        return 0;
    }

    printf("[bootLoader] image_convert: %d image(s) need conversion in %s (max %ux%u)\n",
           nqueue, dir, (unsigned)max_w, (unsigned)max_h);
    uint32_t t_batch_start = Frens::time_ms();

    // Progress-bar colours on either backend. Values mirror those the
    // flashing overlay uses in main.cpp.
#if HSTX
    const uint16_t COL_FG     = 0x7FFFu;
    const uint16_t COL_BG     = 0x0000u;
    const uint16_t COL_FILL   = 0x03E0u;
    const uint16_t COL_EMPTY  = 0x2108u;
    const uint16_t COL_BORDER = 0x7FFFu;
#else
    const uint16_t COL_FG     = 0x0FFFu;
    const uint16_t COL_BG     = 0x0000u;
    const uint16_t COL_FILL   = 0x00F0u;
    const uint16_t COL_EMPTY  = 0x0333u;
    const uint16_t COL_BORDER = 0x0FFFu;
#endif

    uint16_t *fb = ic_progress_fb();
    if (fb) {
        for (int i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++) fb[i] = COL_BG;
        ic_fb_draw_text_centered(fb, 100, ui_title, COL_FG, COL_BG);
    }

    int n_ok = 0;
    for (int i = 0; i < nqueue; i++) {
        if (fb) {
            // Band starts at 190: the progress bar's percentage label occupies
            // 182..189, so clearing any higher would clip its bottom rows.
            for (int y = 190; y < 202; y++) {
                uint16_t *row = fb + y * SCREENWIDTH;
                for (int x = 0; x < SCREENWIDTH; x++) row[x] = COL_BG;
            }
            char shown[FF_MAX_LFN + 1];
            ic_truncate_for_display(queue[i], shown, SCREENWIDTH / FONT_CHAR_WIDTH);
            ic_fb_draw_text_centered(fb, 192, shown, COL_FG, COL_BG);
            progress_bar_draw((uint32_t)i, (uint32_t)nqueue,
                              COL_FILL, COL_EMPTY, COL_BORDER);
        }

        printf("[bootLoader] image_convert: [%d/%d] converting %s\n", i + 1, nqueue, queue[i]);
        bool ok = image_convert_ensure(dir, queue[i], max_w, max_h, letterbox);
        if (ok) n_ok++;
        else    printf("[bootLoader] image_convert: [%d/%d] %s FAILED\n", i + 1, nqueue, queue[i]);

        if (fb) {
            progress_bar_draw((uint32_t)(i + 1), (uint32_t)nqueue,
                              COL_FILL, COL_EMPTY, COL_BORDER);
        }
    }

    printf("[bootLoader] image_convert: batch done in %u ms (%d ok, %d failed)\n",
           (unsigned)(Frens::time_ms() - t_batch_start), n_ok, nqueue - n_ok);

    Frens::f_free(queue);
    return n_ok;
}

bool image_convert_ensure(const char *dir, const char *basename,
                          uint16_t max_w, uint16_t max_h, bool letterbox)
{
    if (!dir || !basename || !*basename || max_w == 0 || max_h == 0) return false;

    // Fast path: both cache files already present.
    char path_444[FF_MAX_LFN + 1];
    char path_555[FF_MAX_LFN + 1];
    snprintf(path_444, sizeof(path_444), "%s/%s.444", dir, basename);
    snprintf(path_555, sizeof(path_555), "%s/%s.555", dir, basename);
    if (file_exists(path_444) && file_exists(path_555)) return true;

    // Find a source PNG / JPEG.
    char src_path[FF_MAX_LFN + 1];
    bool is_png = false;
    snprintf(src_path, sizeof(src_path), "%s/%s.png", dir, basename);
    if (file_exists(src_path)) { is_png = true; }
    else {
        snprintf(src_path, sizeof(src_path), "%s/%s.jpg", dir, basename);
        if (!file_exists(src_path)) {
            snprintf(src_path, sizeof(src_path), "%s/%s.jpeg", dir, basename);
            if (!file_exists(src_path)) return false;
        }
    }

    // Reset per-call transient state; leave buffers NULL so free_conv_buffers()
    // is safe on any early-return path above.
    g_st = ConvState{};

    uint32_t t_start = Frens::time_ms();
    ConvStatus st = is_png
        ? convert_png(src_path, dir, basename, max_w, max_h, letterbox)
        : convert_jpg(src_path, dir, basename, max_w, max_h, letterbox);

    if (st == ConvStatus::Ok) {
        printf("[bootLoader] image_convert: %s OK (total %u ms)\n",
               src_path, (unsigned)(Frens::time_ms() - t_start));
        return true;
    }
    printf("[bootLoader] image_convert: %s FAILED\n", src_path);
    if (st == ConvStatus::Unsupported) {
        // The file itself can never convert -- quarantine it so directory
        // scans stop retrying it on every boot. Transient failures (heap,
        // SD I/O) stay in place for a retry.
        move_to_unsupported(dir, src_path);
    }
    return false;
}
