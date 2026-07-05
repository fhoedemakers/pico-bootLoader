#include "image_convert.h"

#include "FrensHelpers.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

#include "ff.h"

// stb_image vendored implementation. Routed through PSRAM (Frens::f_malloc)
// so 1280x960 RGB888 decode (~3.6 MB) doesn't touch SRAM.
//
// ic_realloc(): stb_image relies on the standard C behaviour where
// realloc(NULL, sz) == malloc(sz). Frens::f_realloc() instead returns NULL
// when its input is NULL (see pico_shared/FrensHelpers.cpp:871), which stb's
// PNG decoder misreads as "outofmem" on the very first IDAT chunk grow.
// The shim below emulates the standard semantics without touching the
// submodule.
static inline void *ic_realloc(void *p, size_t sz)
{
    if (sz == 0) { Frens::f_free(p); return nullptr; }
    if (!p)      return Frens::f_malloc(sz);
    return Frens::f_realloc(p, sz);
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_ASSERT(x)     ((void)0)
#define STBI_MALLOC(sz)    Frens::f_malloc(sz)
#define STBI_REALLOC(p,sz) ic_realloc((p),(sz))
#define STBI_FREE(p)       Frens::f_free(p)
#include "third_party/stb/stb_image.h"

namespace {

bool file_exists(const char *path)
{
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

uint8_t *read_whole_file(const char *path, size_t *out_size)
{
    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) return nullptr;
    FSIZE_t sz = f_size(&fil);
    if (sz == 0 || sz > 8u * 1024u * 1024u) { f_close(&fil); return nullptr; }
    uint8_t *buf = (uint8_t *)Frens::f_malloc((size_t)sz);
    if (!buf) { f_close(&fil); return nullptr; }
    UINT br = 0;
    FRESULT fr = f_read(&fil, buf, (UINT)sz, &br);
    f_close(&fil);
    if (fr != FR_OK || br != (UINT)sz) { Frens::f_free(buf); return nullptr; }
    *out_size = (size_t)sz;
    return buf;
}

bool write_pixel_file(const char *dir, const char *basename, const char *ext,
                      uint16_t w, uint16_t h, const uint16_t *pixels)
{
    char tmp_path[FF_MAX_LFN + 1];
    char final_path[FF_MAX_LFN + 1];
    snprintf(final_path, sizeof(final_path), "%s/%s%s",     dir, basename, ext);
    snprintf(tmp_path,   sizeof(tmp_path),   "%s/%s%s.tmp", dir, basename, ext);

    FIL fil;
    if (f_open(&fil, tmp_path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[bootLoader] image_convert: cannot open %s for write\n", tmp_path);
        return false;
    }
    uint16_t hdr[2] = { w, h };
    UINT bw = 0;
    if (f_write(&fil, hdr, sizeof(hdr), &bw) != FR_OK || bw != sizeof(hdr)) {
        f_close(&fil); f_unlink(tmp_path); return false;
    }
    UINT payload = (UINT)w * (UINT)h * (UINT)sizeof(uint16_t);
    if (f_write(&fil, pixels, payload, &bw) != FR_OK || bw != payload) {
        f_close(&fil); f_unlink(tmp_path); return false;
    }
    f_close(&fil);

    // f_rename fails if target exists; drop it first.
    f_unlink(final_path);
    if (f_rename(tmp_path, final_path) != FR_OK) {
        printf("[bootLoader] image_convert: rename %s -> %s failed\n", tmp_path, final_path);
        f_unlink(tmp_path);
        return false;
    }
    return true;
}

// Area-average downscaler. Copies (src_w x src_h) RGB888 into
// dst[(off_y..+dst_h) x (off_x..+dst_w)] of a canvas of stride canvas_stride.
// Since callers clamp scale to <= 1, (dst_w <= src_w) and (dst_h <= src_h)
// always, so count is >= 1 per output pixel.
void box_scale_rgb888(const uint8_t *src, int src_w, int src_h,
                      uint8_t *dst, int dst_w, int dst_h,
                      int canvas_stride, int off_x, int off_y)
{
    for (int y = 0; y < dst_h; y++) {
        int ys0 = (int)((int64_t)y       * src_h / dst_h);
        int ys1 = (int)((int64_t)(y + 1) * src_h / dst_h);
        if (ys1 <= ys0) ys1 = ys0 + 1;
        if (ys1 > src_h) ys1 = src_h;

        for (int x = 0; x < dst_w; x++) {
            int xs0 = (int)((int64_t)x       * src_w / dst_w);
            int xs1 = (int)((int64_t)(x + 1) * src_w / dst_w);
            if (xs1 <= xs0) xs1 = xs0 + 1;
            if (xs1 > src_w) xs1 = src_w;

            uint32_t sR = 0, sG = 0, sB = 0;
            uint32_t count = 0;
            for (int sy = ys0; sy < ys1; sy++) {
                const uint8_t *p = src + ((size_t)sy * src_w + xs0) * 3;
                for (int sx = xs0; sx < xs1; sx++) {
                    sR += p[0]; sG += p[1]; sB += p[2];
                    p += 3;
                    count++;
                }
            }
            uint8_t *d = dst + ((size_t)(y + off_y) * canvas_stride + (x + off_x)) * 3;
            d[0] = (uint8_t)(sR / count);
            d[1] = (uint8_t)(sG / count);
            d[2] = (uint8_t)(sB / count);
        }
    }
}

uint8_t *try_read_source(const char *dir, const char *basename, const char *ext,
                         size_t *out_size, char *found_path, size_t found_path_sz)
{
    snprintf(found_path, found_path_sz, "%s/%s%s", dir, basename, ext);
    if (!file_exists(found_path)) return nullptr;
    return read_whole_file(found_path, out_size);
}

} // namespace

bool image_convert_ensure(const char *dir, const char *basename,
                          uint16_t max_w, uint16_t max_h, bool letterbox)
{
    if (!dir || !basename || !*basename || max_w == 0 || max_h == 0) return false;

    // Fast path: both cache files already present.
    char path_444[FF_MAX_LFN + 1];
    char path_555[FF_MAX_LFN + 1];
    snprintf(path_444, sizeof(path_444), "%s/%s.444", dir, basename);
    snprintf(path_555, sizeof(path_555), "%s/%s.555", dir, basename);
    bool has_444 = file_exists(path_444);
    bool has_555 = file_exists(path_555);
    if (has_444 && has_555) return true;

    if (!Frens::isPsramEnabled()) {
        // Only log when a source PNG/JPG actually exists -- otherwise every
        // missing tile would spam the console.
        char probe[FF_MAX_LFN + 1];
        snprintf(probe, sizeof(probe), "%s/%s.png", dir, basename);
        bool has_src = file_exists(probe);
        if (!has_src) { snprintf(probe, sizeof(probe), "%s/%s.jpg",  dir, basename); has_src = file_exists(probe); }
        if (!has_src) { snprintf(probe, sizeof(probe), "%s/%s.jpeg", dir, basename); has_src = file_exists(probe); }
        if (has_src) {
            printf("[bootLoader] image_convert: %s needs PSRAM to convert, skipped\n", probe);
        }
        return false;
    }

    // Find a source file.
    char   src_path[FF_MAX_LFN + 1];
    size_t src_size = 0;
    uint8_t *src_data = try_read_source(dir, basename, ".png",  &src_size, src_path, sizeof(src_path));
    if (!src_data) src_data = try_read_source(dir, basename, ".jpg",  &src_size, src_path, sizeof(src_path));
    if (!src_data) src_data = try_read_source(dir, basename, ".jpeg", &src_size, src_path, sizeof(src_path));
    if (!src_data) return false;

    // Peek dimensions before committing to a decode buffer.
    int src_w = 0, src_h = 0, src_comp = 0;
    if (!stbi_info_from_memory(src_data, (int)src_size, &src_w, &src_h, &src_comp) ||
        src_w <= 0 || src_h <= 0) {
        printf("[bootLoader] image_convert: %s not a valid PNG/JPEG\n", src_path);
        Frens::f_free(src_data);
        return false;
    }
    if (src_w > 1280 || src_h > 960) {
        printf("[bootLoader] image_convert: %s too large (%dx%d, max 1280x960)\n",
               src_path, src_w, src_h);
        Frens::f_free(src_data);
        return false;
    }

    uint32_t t_start = Frens::time_ms();
    printf("[bootLoader] image_convert: %s: %dx%d, %u B, %d channels -> canvas max %ux%u %s\n",
           src_path, src_w, src_h, (unsigned)src_size, src_comp,
           (unsigned)max_w, (unsigned)max_h, letterbox ? "letterboxed" : "unpadded");

    printf("[bootLoader] image_convert: %s: decoding...\n", src_path);
    int decoded_w = 0, decoded_h = 0, decoded_c = 0;
    uint8_t *decoded = stbi_load_from_memory(src_data, (int)src_size,
                                             &decoded_w, &decoded_h, &decoded_c, 3);
    Frens::f_free(src_data);
    if (!decoded) {
        printf("[bootLoader] image_convert: decode failed for %s (%s)\n",
               src_path, stbi_failure_reason());
        return false;
    }
    uint32_t t_decoded = Frens::time_ms();
    printf("[bootLoader] image_convert: %s: decoded %dx%d in %u ms\n",
           src_path, decoded_w, decoded_h, (unsigned)(t_decoded - t_start));

    // Scale down only; smaller images stay 1:1 centred.
    float sx = (float)max_w / (float)decoded_w;
    float sy = (float)max_h / (float)decoded_h;
    float scale = sx < sy ? sx : sy;
    if (scale > 1.0f) scale = 1.0f;
    int dst_w = (int)((float)decoded_w * scale + 0.5f);
    int dst_h = (int)((float)decoded_h * scale + 0.5f);
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;
    if (dst_w > (int)max_w) dst_w = (int)max_w;
    if (dst_h > (int)max_h) dst_h = (int)max_h;

    int canvas_w, canvas_h, off_x, off_y;
    if (letterbox) {
        canvas_w = (int)max_w;
        canvas_h = (int)max_h;
        off_x    = (canvas_w - dst_w) / 2;
        off_y    = (canvas_h - dst_h) / 2;
    } else {
        canvas_w = dst_w;
        canvas_h = dst_h;
        off_x    = 0;
        off_y    = 0;
    }

    size_t canvas_bytes = (size_t)canvas_w * (size_t)canvas_h * 3u;
    uint8_t *canvas = (uint8_t *)Frens::f_malloc(canvas_bytes);
    if (!canvas) {
        printf("[bootLoader] image_convert: canvas alloc %u B failed\n", (unsigned)canvas_bytes);
        Frens::f_free(decoded);
        return false;
    }
    if (letterbox) memset(canvas, 0, canvas_bytes);

    printf("[bootLoader] image_convert: %s: scaling %dx%d -> %dx%d (canvas %dx%d, offset %d,%d)\n",
           src_path, decoded_w, decoded_h, dst_w, dst_h, canvas_w, canvas_h, off_x, off_y);
    uint32_t t_scale_start = Frens::time_ms();
    box_scale_rgb888(decoded, decoded_w, decoded_h,
                     canvas, dst_w, dst_h,
                     canvas_w, off_x, off_y);
    Frens::f_free(decoded);
    printf("[bootLoader] image_convert: %s: scale done in %u ms\n",
           src_path, (unsigned)(Frens::time_ms() - t_scale_start));

    size_t pixels = (size_t)canvas_w * (size_t)canvas_h;
    uint16_t *out_444 = (uint16_t *)Frens::f_malloc(pixels * sizeof(uint16_t));
    uint16_t *out_555 = (uint16_t *)Frens::f_malloc(pixels * sizeof(uint16_t));
    if (!out_444 || !out_555) {
        printf("[bootLoader] image_convert: pack buffer alloc failed\n");
        if (out_444) Frens::f_free(out_444);
        if (out_555) Frens::f_free(out_555);
        Frens::f_free(canvas);
        return false;
    }
    for (size_t i = 0; i < pixels; i++) {
        uint8_t R = canvas[i * 3 + 0];
        uint8_t G = canvas[i * 3 + 1];
        uint8_t B = canvas[i * 3 + 2];
        out_444[i] = (uint16_t)(((R >> 4) << 8) | ((G >> 4) << 4) |  (B >> 4));
        out_555[i] = (uint16_t)(((R >> 3) << 10)| ((G >> 3) << 5) |  (B >> 3));
    }
    Frens::f_free(canvas);

    printf("[bootLoader] image_convert: %s: writing %s/%s.444 and .555 (%d B each)...\n",
           src_path, dir, basename, canvas_w * canvas_h * 2 + 4);
    uint32_t t_write_start = Frens::time_ms();
    bool ok_444 = write_pixel_file(dir, basename, ".444",
                                   (uint16_t)canvas_w, (uint16_t)canvas_h, out_444);
    bool ok_555 = write_pixel_file(dir, basename, ".555",
                                   (uint16_t)canvas_w, (uint16_t)canvas_h, out_555);
    Frens::f_free(out_444);
    Frens::f_free(out_555);

    uint32_t t_end = Frens::time_ms();
    if (ok_444 && ok_555) {
        printf("[bootLoader] image_convert: %s: OK -> %dx%d (write %u ms, total %u ms)\n",
               src_path, canvas_w, canvas_h,
               (unsigned)(t_end - t_write_start), (unsigned)(t_end - t_start));
        return true;
    }
    printf("[bootLoader] image_convert: %s: write FAILED (444=%d, 555=%d)\n",
           src_path, (int)ok_444, (int)ok_555);
    return false;
}
