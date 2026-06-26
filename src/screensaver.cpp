#include "screensaver.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "FrensHelpers.h"
#include "ff.h"

// ============================================================================
// Mode select
// ============================================================================
// Two visual modes share the same loader / lifecycle and just differ in the
// per-frame motion + draw step. Pick at compile time via SCREENSAVER_MODE:
//
//   SCREENSAVER_MODE_BOUNCE    (0, default): images float and bounce off the
//                              screen edges and each other.
//   SCREENSAVER_MODE_STARFIELD (1)         : images fly in from a distant
//                              point at screen centre, scaling from tiny to
//                              large as they approach the camera, then leave
//                              the screen and respawn far away.
//
// Override by editing the line below, or by passing
// -DSCREENSAVER_MODE=SCREENSAVER_MODE_STARFIELD on the compile line.
#define SCREENSAVER_MODE_BOUNCE    0
#define SCREENSAVER_MODE_STARFIELD 1

#ifndef SCREENSAVER_MODE
#define SCREENSAVER_MODE SCREENSAVER_MODE_STARFIELD
#endif

// ============================================================================
// Shared constants
// ============================================================================
// Two sprite caps; screensaver_init() picks one at runtime from
// Frens::isPsramEnabled(). SS_MAX_SPRITES is the compile-time ceiling --
// it sizes g_spr[] in BSS and the on-stack temporaries in the swap path
// and starfield draw(). To avoid paying the BSS / stack cost on builds
// that won't use PSRAM, override SS_MAX_SPRITES_PSRAM to <= the SRAM cap
// (e.g. -DSS_MAX_SPRITES_PSRAM=10): the ceiling collapses to that value
// and the runtime ternary picks the SRAM cap regardless.
#ifndef SS_MAX_SPRITES_PSRAM
#define SS_MAX_SPRITES_PSRAM   100
#endif
#ifndef SS_MAX_SPRITES_SRAM
#define SS_MAX_SPRITES_SRAM    8
#endif
#if SS_MAX_SPRITES_PSRAM > SS_MAX_SPRITES_SRAM
#define SS_MAX_SPRITES  SS_MAX_SPRITES_PSRAM
#else
#define SS_MAX_SPRITES  SS_MAX_SPRITES_SRAM
#endif

#define SS_MAX_W         55
#define SS_MAX_H         60
#define SS_DIR_PATH      "/emu/assets/screensaver"
#define SS_SWAP_FRAMES   (15 * 60)   /* re-pick the sprite set every 15 s @60fps */

#define Q              16
#define ONE_PX         (1 << Q)

// ============================================================================
// Mode-specific tuning
// ============================================================================
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
  #define MIN_SPEED_FP   (1 << (Q - 2))      /* 0.25 px / frame */
  #define SPEED_RANGE_FP (1 << (Q - 2))      /* extra 0..0.25 -> max ~0.5 px/f */
#elif SCREENSAVER_MODE == SCREENSAVER_MODE_STARFIELD
  // Pin-hole projection. With these constants a sprite at z = SS_FOCAL renders
  // at its source size; smaller z makes it bigger and pushes it further from
  // screen centre (radial outward drift -- the starfield effect).
  #define SS_FOCAL    256              /* pixel-space focal length */
  #define SS_Z_FAR    (SS_FOCAL * 20)  /* spawn depth -> scale ~1/20 (tiny)   */
  #define SS_Z_NEAR   (SS_FOCAL / 4)   /* despawn depth -> scale ~4   (huge)  */
  #define SS_X_RANGE  (SS_FOCAL)       /* world-XY range; bigger = more radial spread */
  #define SS_VZ_MIN   5                /* min |closing speed| per frame */
  #define SS_VZ_MAX   15               /* max |closing speed| per frame */
#else
  #error "SCREENSAVER_MODE must be SCREENSAVER_MODE_BOUNCE or SCREENSAVER_MODE_STARFIELD"
#endif

namespace {

struct Sprite {
    uint16_t *pixels;         // Frens::f_malloc'd, w*h*2 bytes
    int16_t   w, h;
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    int32_t   x, y;           // Q16.16 screen top-left
    int32_t   vx, vy;         // Q16.16 px/frame
#else
    int32_t   x3d, y3d;       // world X/Y, integer pixels at z = SS_FOCAL
    int32_t   z;              // depth, always > 0 (smaller = closer to camera)
    int32_t   vz;             // depth velocity, always < 0 (approaching camera)
#endif
};

Sprite   g_spr[SS_MAX_SPRITES];
int      g_spr_count         = 0;
uint32_t g_frames_since_load = 0;   // drives the 15 s sprite-set swap

// Of the sprites in g_spr[0..g_spr_count), the first g_n_owners own their
// pixel buffer and must be released with Frens::f_free. Slots in
// [g_n_owners, g_spr_count) are clones whose `pixels` aliases an owner's
// buffer -- freeing those would be a double-free. Cloning lets us fill the
// screen even when the user has fewer files than g_sprite_cap.
int      g_n_owners = 0;

// Effective cap chosen at screensaver_init() time based on PSRAM presence.
// Always <= SS_MAX_SPRITES so the static arrays are large enough either way.
int      g_sprite_cap = SS_MAX_SPRITES_SRAM;

// Scratch buffer for reservoir-sampled file names. f_malloc'd at
// screensaver_init() sized to g_sprite_cap (not SS_MAX_SPRITES) so an
// SRAM-only run doesn't pay for a PSRAM-sized buffer (e.g. cap=8 -> 2 KB
// vs cap=100 -> 25 KB). Released in screensaver_free(). Can't live on the
// stack: the picker thread runs on a 3 KB stack (PICO_STACK_SIZE in
// CMakeLists.txt).
char   (*g_chosen)[FF_MAX_LFN + 1] = nullptr;

// Per-mode draw / swap scratch. One f_malloc at screensaver_init() backs
// all of the cap-sized arrays the hot path needs; previously these lived
// on the stack and exceeded the 3 KB picker stack at large caps. Sized to
// g_sprite_cap (runtime), not SS_MAX_SPRITES.
struct Scratch {
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    // Used by the 15 s sprite-set swap to stage new sprites before
    // installing them.
    Sprite *new_arr;
#else // STARFIELD
    // Per-sprite caches recomputed every frame in draw().
    int     *dx, *dy, *sw, *sh;     // projected screen rect
    int32_t *step_x_q, *step_y_q;   // DDA steps for nearest-neighbour scaling
    bool    *skip;                  // sprite fully off-screen this frame
    int     *order;                 // depth-sorted draw order, far -> near
#endif
};
Scratch  g_scratch    = {};
void    *g_scratch_buf = nullptr;   // base of the single backing allocation

// ---------------------------------------------------------------------------
// SD enumeration + file loading (mode-independent)
// ---------------------------------------------------------------------------

// One-shot allocator for the per-mode scratch struct. All cap-sized arrays
// live in a single f_malloc'd block so failure / cleanup is one pointer.
bool alloc_scratch(int cap)
{
    if (cap <= 0) return false;
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    size_t total = sizeof(Sprite) * (size_t)cap;
    g_scratch_buf = Frens::f_malloc(total);
    if (!g_scratch_buf) return false;
    g_scratch.new_arr = (Sprite *)g_scratch_buf;
#else
    // Five int arrays, two int32_t arrays, one bool array. ints and int32_t
    // share alignment so they can sit back-to-back; bool goes last.
    const size_t int_arr = sizeof(int)     * (size_t)cap;
    const size_t i32_arr = sizeof(int32_t) * (size_t)cap;
    const size_t b_arr   = sizeof(bool)    * (size_t)cap;
    const size_t total   = int_arr * 5 + i32_arr * 2 + b_arr;
    char *p = (char *)Frens::f_malloc(total);
    if (!p) return false;
    g_scratch_buf      = p;
    g_scratch.dx       = (int *)    (p + 0 * int_arr);
    g_scratch.dy       = (int *)    (p + 1 * int_arr);
    g_scratch.sw       = (int *)    (p + 2 * int_arr);
    g_scratch.sh       = (int *)    (p + 3 * int_arr);
    g_scratch.order    = (int *)    (p + 4 * int_arr);
    g_scratch.step_x_q = (int32_t *)(p + 5 * int_arr);
    g_scratch.step_y_q = (int32_t *)(p + 5 * int_arr + 1 * i32_arr);
    g_scratch.skip     = (bool *)   (p + 5 * int_arr + 2 * i32_arr);
#endif
    return true;
}

void free_scratch()
{
    if (g_scratch_buf) {
        Frens::f_free(g_scratch_buf);
        g_scratch_buf = nullptr;
    }
    g_scratch = Scratch{};
}

// Case-insensitive endswith on FILEXTFORSEARCH (".555" or ".444").
bool name_has_ext(const char *name)
{
    const char *ext = FILEXTFORSEARCH;
    size_t nn = strlen(name);
    size_t ne = strlen(ext);
    if (nn <= ne) return false;
    return strcasecmp(name + nn - ne, ext) == 0;
}

// Pick the chosen list of file names via reservoir sampling so we walk the
// directory exactly once and pick at most g_sprite_cap. Results land in the
// file-scope g_chosen[] buffer; the return value is the count.
int gather_random_names()
{
    if (!g_chosen) return 0;
    DIR dir;
    if (f_opendir(&dir, SS_DIR_PATH) != FR_OK) return 0;

    const int cap = g_sprite_cap;
    FILINFO fno;
    int seen = 0;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) {
        if (fno.fattrib & AM_DIR)        continue;
        if (!name_has_ext(fno.fname))    continue;

        if (seen < cap) {
            strncpy(g_chosen[seen], fno.fname, FF_MAX_LFN);
            g_chosen[seen][FF_MAX_LFN] = '\0';
        } else {
            int j = (int)(rand() % (unsigned)(seen + 1));
            if (j < cap) {
                strncpy(g_chosen[j], fno.fname, FF_MAX_LFN);
                g_chosen[j][FF_MAX_LFN] = '\0';
            }
        }
        seen++;
    }
    f_closedir(&dir);

    return seen < cap ? seen : cap;
}

// Read header + pixel buffer. Sets out.pixels/w/h on success; pose fields are
// the caller's job to initialise (mode-specific).
bool load_pixels(const char *name, Sprite &out)
{
    char path[FF_MAX_LFN + 1];
    int n = snprintf(path, sizeof(path), "%s/%s", SS_DIR_PATH, name);
    if (n <= 0 || n >= (int)sizeof(path)) return false;

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK) {
        printf("[emuLoader] ss: cannot open %s\n", path);
        return false;
    }

    uint16_t hdr[2];
    UINT br = 0;
    if (f_read(&fil, hdr, sizeof(hdr), &br) != FR_OK || br != sizeof(hdr)) {
        f_close(&fil);
        return false;
    }
    uint16_t w = hdr[0], h = hdr[1];
    if (w == 0 || h == 0 || w > SS_MAX_W || h > SS_MAX_H) {
        f_close(&fil);
        return false;
    }

    UINT want = (UINT)w * (UINT)h * sizeof(uint16_t);
    uint16_t *buf = (uint16_t *)Frens::f_malloc(want);
    if (!buf) {
        printf("[emuLoader] ss: f_malloc(%u) failed for %s\n", (unsigned)want, path);
        f_close(&fil);
        return false;
    }

    if (f_read(&fil, buf, want, &br) != FR_OK || br != want) {
        printf("[emuLoader] ss: short read on %s (%u/%u)\n",
               path, (unsigned)br, (unsigned)want);
        Frens::f_free(buf);
        f_close(&fil);
        return false;
    }
    f_close(&fil);

    out.pixels = buf;
    out.w      = (int16_t)w;
    out.h      = (int16_t)h;
    return true;
}

// ---------------------------------------------------------------------------
// Mode-specific: initial pose, per-frame physics, draw
// ---------------------------------------------------------------------------

#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE

int32_t rand_velocity()
{
    int32_t mag = MIN_SPEED_FP + (int32_t)(rand() & (SPEED_RANGE_FP - 1));
    return (rand() & 1) ? mag : -mag;
}

void init_pose(Sprite &s)
{
    int x_range = SCREENWIDTH  - s.w + 1;
    int y_range = SCREENHEIGHT - s.h + 1;
    s.x  = (int32_t)(rand() % (unsigned)x_range) << Q;
    s.y  = (int32_t)(rand() % (unsigned)y_range) << Q;
    s.vx = rand_velocity();
    s.vy = rand_velocity();
}

void step_one(Sprite &s)
{
    s.x += s.vx;
    s.y += s.vy;

    int xpx = s.x >> Q;
    int ypx = s.y >> Q;

    if (xpx < 0)                                 { s.x = 0;
        if (s.vx < 0) s.vx = -s.vx; }
    else if (xpx + s.w > SCREENWIDTH)            { s.x = (int32_t)(SCREENWIDTH - s.w) << Q;
        if (s.vx > 0) s.vx = -s.vx; }
    if (ypx < 0)                                 { s.y = 0;
        if (s.vy < 0) s.vy = -s.vy; }
    else if (ypx + s.h > SCREENHEIGHT)           { s.y = (int32_t)(SCREENHEIGHT - s.h) << Q;
        if (s.vy > 0) s.vy = -s.vy; }
}

// Bounce-mode pair resolution: AABB overlap -> reverse on the axis of least
// penetration and push the pair apart by half the overlap each.
void resolve_pair(Sprite &a, Sprite &b)
{
    int axp = a.x >> Q, ayp = a.y >> Q;
    int bxp = b.x >> Q, byp = b.y >> Q;
    if (axp >= bxp + b.w || bxp >= axp + a.w) return;
    if (ayp >= byp + b.h || byp >= ayp + a.h) return;

    int ox = (axp < bxp) ? (axp + a.w - bxp) : (bxp + b.w - axp);
    int oy = (ayp < byp) ? (ayp + a.h - byp) : (byp + b.h - ayp);

    if (ox <= oy) {
        if (axp < bxp) { a.x -= (int32_t)ox << (Q - 1); b.x += (int32_t)ox << (Q - 1); }
        else           { a.x += (int32_t)ox << (Q - 1); b.x -= (int32_t)ox << (Q - 1); }
        a.vx = -a.vx; b.vx = -b.vx;
    } else {
        if (ayp < byp) { a.y -= (int32_t)oy << (Q - 1); b.y += (int32_t)oy << (Q - 1); }
        else           { a.y += (int32_t)oy << (Q - 1); b.y -= (int32_t)oy << (Q - 1); }
        a.vy = -a.vy; b.vy = -b.vy;
    }
}

void draw()
{
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

        memset(dst, 0, SCREENWIDTH * sizeof(uint16_t));

        for (int i = 0; i < g_spr_count; i++) {
            const Sprite &s = g_spr[i];
            int sy = s.y >> Q;
            if (y < sy || y >= sy + s.h) continue;

            int sx = s.x >> Q;
            int src_x = 0, dst_x = sx, copy_w = s.w;
            if (dst_x < 0)                  { src_x = -dst_x; copy_w += dst_x; dst_x = 0; }
            if (dst_x + copy_w > SCREENWIDTH) copy_w = SCREENWIDTH - dst_x;
            if (copy_w <= 0) continue;

            const uint16_t *row = s.pixels + (y - sy) * s.w + src_x;
            memcpy(dst + dst_x, row, (size_t)copy_w * sizeof(uint16_t));
        }

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
        if (!Frens::isFrameBufferUsed())
#endif
            dvi_->setLineBuffer(y, lb);
#endif
    }
}

#else // SCREENSAVER_MODE_STARFIELD

// Drop the sprite back to z_far with a fresh random world XY and closing speed.
void respawn(Sprite &s)
{
    s.x3d = (int32_t)(rand() % (unsigned)(2 * SS_X_RANGE + 1)) - SS_X_RANGE;
    s.y3d = (int32_t)(rand() % (unsigned)(2 * SS_X_RANGE + 1)) - SS_X_RANGE;
    s.z   = SS_Z_FAR;
    s.vz  = -(int32_t)(SS_VZ_MIN + (rand() % (unsigned)(SS_VZ_MAX - SS_VZ_MIN + 1)));
}

void init_pose(Sprite &s)
{
    // Same as respawn() but stagger the initial depth so the five sprites
    // don't all arrive at the camera in the same frame on first boot.
    respawn(s);
    int32_t zrange = SS_Z_FAR - SS_Z_NEAR;
    s.z = SS_Z_NEAR + (int32_t)(rand() % (unsigned)zrange);
}

// Projected on-screen rect (top-left + scaled w/h) at the current z.
void project(const Sprite &s, int &dx, int &dy, int &sw, int &sh)
{
    int32_t z = s.z > 1 ? s.z : 1;   // safety; never reached -- step_one clamps
    sw = (int)((int64_t)s.w * SS_FOCAL / z);
    sh = (int)((int64_t)s.h * SS_FOCAL / z);
    int ox = (int)((int64_t)s.x3d * SS_FOCAL / z);
    int oy = (int)((int64_t)s.y3d * SS_FOCAL / z);
    dx = SCREENWIDTH  / 2 + ox - sw / 2;
    dy = SCREENHEIGHT / 2 + oy - sh / 2;
}

// Returns true when the rect is entirely off-screen (or degenerate).
bool rect_offscreen(int dx, int dy, int sw, int sh)
{
    if (sw < 1 || sh < 1) return true;
    if (dx + sw <= 0 || dy + sh <= 0) return true;
    if (dx >= SCREENWIDTH || dy >= SCREENHEIGHT) return true;
    return false;
}

void step_one(Sprite &s)
{
    s.z += s.vz;
    if (s.z <= SS_Z_NEAR) {
        respawn(s);
        return;
    }
    // Also respawn if the sprite has already grown past the screen edges.
    // (A close-up sprite whose centre is off-axis can leave the viewport
    // before z reaches z_near.)
    int dx, dy, sw, sh;
    project(s, dx, dy, sw, sh);
    if (rect_offscreen(dx, dy, sw, sh) && s.z < SS_FOCAL) {
        respawn(s);
    }
}

void draw()
{
    // Project once per frame; cache the rect and per-axis DDA steps so the
    // per-pixel work is just a shift + memory write + add. Cap-sized arrays
    // live in g_scratch (heap-backed, allocated once at screensaver_init)
    // so the 3 KB picker stack isn't blown at large caps.
    int     *dx       = g_scratch.dx;
    int     *dy       = g_scratch.dy;
    int     *sw       = g_scratch.sw;
    int     *sh       = g_scratch.sh;
    int32_t *step_x_q = g_scratch.step_x_q;
    int32_t *step_y_q = g_scratch.step_y_q;
    bool    *skip     = g_scratch.skip;
    int     *order    = g_scratch.order;
    // Zero the full cap-sized array (not just g_spr_count) so the compiler
    // can prove the memset can't overrun -- g_spr_count is bounded by
    // g_sprite_cap but it can't see that across translation.
    memset(skip, 0, sizeof(bool) * (size_t)g_sprite_cap);

    for (int i = 0; i < g_spr_count; i++) {
        order[i] = i;
        project(g_spr[i], dx[i], dy[i], sw[i], sh[i]);
        if (rect_offscreen(dx[i], dy[i], sw[i], sh[i])) { skip[i] = true; continue; }
        // src step per output pixel, Q16.16:
        //   step_x_q = (src_w << Q) / dst_w  (>0). Same for y.
        step_x_q[i] = ((int32_t)g_spr[i].w << Q) / sw[i];
        step_y_q[i] = ((int32_t)g_spr[i].h << Q) / sh[i];
    }

    // Sort sprite indices by depth, farthest first. The per-scanline loop
    // walks them in this order so the closer (smaller-z, larger-on-screen)
    // sprites paint over the more distant ones -- the correct occlusion for
    // a forward-moving camera.
    for (int i = 1; i < g_spr_count; i++) {
        int k = order[i];
        int32_t zk = g_spr[k].z;
        int j = i;
        while (j > 0 && g_spr[order[j - 1]].z < zk) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = k;
    }

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

        memset(dst, 0, SCREENWIDTH * sizeof(uint16_t));

        for (int idx = 0; idx < g_spr_count; idx++) {
            int i = order[idx];
            if (skip[i]) continue;
            int sy0 = dy[i], sy1 = dy[i] + sh[i];
            if (y < sy0 || y >= sy1) continue;

            // Nearest-neighbour source row for this output scanline.
            int32_t row_q = (int32_t)(y - sy0) * step_y_q[i];
            int src_row = row_q >> Q;
            if (src_row < 0) src_row = 0;
            else if (src_row >= g_spr[i].h) src_row = g_spr[i].h - 1;
            const uint16_t *src = g_spr[i].pixels + (int)src_row * g_spr[i].w;

            // Horizontal clip and DDA setup.
            int x0 = dx[i];
            int x1 = dx[i] + sw[i];
            int sub_start = 0;                // skipped output cols on the left
            if (x0 < 0)              { sub_start = -x0; x0 = 0; }
            if (x1 > SCREENWIDTH)    x1 = SCREENWIDTH;
            if (x1 <= x0) continue;

            int32_t col_q = (int32_t)sub_start * step_x_q[i];
            int32_t step  = step_x_q[i];
            int max_src   = g_spr[i].w - 1;
            for (int x = x0; x < x1; x++) {
                int src_col = col_q >> Q;
                if (src_col > max_src) src_col = max_src;
                dst[x] = src[src_col];
                col_q += step;
            }
        }

#if !HSTX
#if FRAMEBUFFERISPOSSIBLE
        if (!Frens::isFrameBufferUsed())
#endif
            dvi_->setLineBuffer(y, lb);
#endif
    }
}

#endif // SCREENSAVER_MODE

// ---------------------------------------------------------------------------
// Shared lifecycle helpers
// ---------------------------------------------------------------------------

bool load_one_sprite(const char *name, Sprite &out)
{
    if (!load_pixels(name, out)) return false;
    init_pose(out);
    return true;
}

// Free pixel buffers for the first `n_owners` slots (the only ones that
// own their buffer) and zero every slot up to `total`.
void free_sprites(Sprite *arr, int n_owners, int total)
{
    for (int i = 0; i < n_owners; i++) {
        if (arr[i].pixels) Frens::f_free(arr[i].pixels);
    }
    for (int i = 0; i < total; i++) arr[i] = Sprite{};
}

// Clone an existing sprite into a fresh slot: same pixel buffer (aliased,
// not copied) and a freshly randomised pose. Clones must live in slots
// past the owner range so free_sprites() doesn't double-free.
void make_clone(const Sprite &src, Sprite &dst)
{
    dst = Sprite{};
    dst.pixels = src.pixels;
    dst.w      = src.w;
    dst.h      = src.h;
    init_pose(dst);
}

// Pick a fresh random sprite set into the caller-provided array. Returns the
// total slot count (owners + clones). *n_owners_out gets the owner count;
// clones live in slots [*n_owners_out, return).
//
// When the user has fewer files than g_sprite_cap we keep cloning random
// owners until the cap is hit -- that way a small assets/ directory still
// fills the screen.
int load_random_set(Sprite *out, int *n_owners_out)
{
    int nchosen = gather_random_names();
    int n_owners = 0;
    for (int i = 0; i < nchosen; i++) {
        Sprite s = {};
        if (load_one_sprite(g_chosen[i], s)) {
            out[n_owners++] = s;
        }
    }
    *n_owners_out = n_owners;

    int total = n_owners;
    while (total < g_sprite_cap && n_owners > 0) {
        int src = (int)(rand() % (unsigned)n_owners);
        make_clone(out[src], out[total++]);
    }
    return total;
}

// Carry the previous sprite's pose/trajectory into the replacement so the
// 15 s swap looks like an in-place transform rather than a teleport.
//   bounce   : preserve screen top-left (recentred) + velocity.
//   starfield: preserve world XY + depth + closing speed.
void inherit_pose(const Sprite &from, Sprite &to)
{
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    int ocx = (from.x >> Q) + from.w / 2;
    int ocy = (from.y >> Q) + from.h / 2;
    int nx  = ocx - to.w / 2;
    int ny  = ocy - to.h / 2;
    if (nx < 0)                       nx = 0;
    if (nx > SCREENWIDTH  - to.w)     nx = SCREENWIDTH  - to.w;
    if (ny < 0)                       ny = 0;
    if (ny > SCREENHEIGHT - to.h)     ny = SCREENHEIGHT - to.h;
    to.x  = (int32_t)nx << Q;
    to.y  = (int32_t)ny << Q;
    to.vx = from.vx;
    to.vy = from.vy;
#else
    to.x3d = from.x3d;
    to.y3d = from.y3d;
    to.z   = from.z;
    to.vz  = from.vz;
#endif
}

} // namespace

extern "C" {

bool screensaver_init(void)
{
    screensaver_free();

#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    g_sprite_cap = 8;
#else 
    g_sprite_cap = Frens::isPsramEnabled() ? SS_MAX_SPRITES_PSRAM
                                           : SS_MAX_SPRITES_SRAM;
#endif
    // Heap-allocate the reservoir-sampling name buffer sized to the runtime
    // cap (not the compile-time ceiling) -- on an SRAM-only run with a high
    // PSRAM cap the difference is large (e.g. 8*256=2 KB vs 100*256=25 KB)
    // and the bigger buffer would OOM the SRAM heap. Freed in
    // screensaver_free().
    const size_t chosen_bytes =
        (size_t)g_sprite_cap * (size_t)(FF_MAX_LFN + 1);
    g_chosen = (char (*)[FF_MAX_LFN + 1])Frens::f_malloc(chosen_bytes);
    if (!g_chosen) {
        printf("[emuLoader] ss: chosen buffer alloc failed (%u B)\n",
               (unsigned)chosen_bytes);
        return false;
    }

    // Allocate the per-frame / per-swap scratch arrays on the heap too --
    // at cap=100 these would exceed the 3 KB picker stack.
    if (!alloc_scratch(g_sprite_cap)) {
        printf("[emuLoader] ss: scratch buffer alloc failed\n");
        Frens::f_free(g_chosen);
        g_chosen = nullptr;
        return false;
    }

    g_spr_count = load_random_set(g_spr, &g_n_owners);
    if (g_spr_count == 0) {
        printf("[emuLoader] ss: no usable images in %s\n", SS_DIR_PATH);
        Frens::f_free(g_chosen);
        g_chosen = nullptr;
        return false;
    }
    g_frames_since_load = 0;
    printf("[emuLoader] ss: %d sprite(s) loaded (%d owner+%d clone, mode=%d, cap=%d, psram=%d)\n",
           g_spr_count, g_n_owners, g_spr_count - g_n_owners,
           SCREENSAVER_MODE, g_sprite_cap, (int)Frens::isPsramEnabled());
    return true;
}

void screensaver_run_one_frame(void)
{
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    // Every 15 s swap the on-screen set for a fresh random pick. Each
    // replacement inherits the previous sprite's pose/trajectory so the
    // change looks like a morph rather than a teleport.
    //
    // Starfield mode skips this -- sprites already cycle naturally as
    // they cross z_near and respawn at z_far, so a forced reload would
    // just visibly snap their identity mid-flight.
    //
    // Memory: free each old slot just before loading its replacement so the
    // peak heap during a swap stays at max(old_count, new_count) sprites'
    // worth of pixel data instead of (old_count + new_count). Without this,
    // a 10-sprite swap on a 520 KB-SRAM no-PSRAM build OOMs in f_malloc.
    g_frames_since_load++;
    if (g_frames_since_load >= SS_SWAP_FRAMES) {
        int nchosen = gather_random_names();
        if (nchosen > 0) {
            // Only OWNER slots [0, g_n_owners) hold heap buffers we must
            // free. Cloned slots alias an owner's buffer so they're just
            // zeroed here -- they'll be regenerated from the new owners
            // after the load. Owners are swapped slot-by-slot to keep the
            // peak heap during the swap at max(old_owners, new_owners)
            // sprites' worth of pixel data instead of the sum (which OOMs
            // on the 520 KB SRAM-only build at cap=10).
            int     old_owners = g_n_owners;
            Sprite *new_arr    = g_scratch.new_arr;   // heap-backed scratch
            memset(new_arr, 0, sizeof(Sprite) * (size_t)g_sprite_cap);
            int     new_owners = 0;

            for (int i = 0; i < nchosen; i++) {
                Sprite saved = {};
                bool   have_old = (i < old_owners);
                if (have_old) {
                    saved = g_spr[i];
                    Frens::f_free(g_spr[i].pixels);
                    g_spr[i] = Sprite{};
                }
                Sprite s = {};
                if (load_one_sprite(g_chosen[i], s)) {
                    if (have_old) inherit_pose(saved, s);
                    new_arr[new_owners++] = s;
                }
            }

            // Free any old owners beyond the new owner count (downsizing).
            for (int i = nchosen; i < old_owners; i++) {
                if (g_spr[i].pixels) Frens::f_free(g_spr[i].pixels);
                g_spr[i] = Sprite{};
            }
            // Zero the old clones (no pixels to free).
            for (int i = old_owners; i < g_spr_count; i++) g_spr[i] = Sprite{};

            // Install new owners.
            for (int i = 0; i < new_owners; i++) g_spr[i] = new_arr[i];

            // Fill remaining slots with clones of the new owners so the
            // visible count stays at g_sprite_cap.
            int total = new_owners;
            while (total < g_sprite_cap && new_owners > 0) {
                int src = (int)(rand() % (unsigned)new_owners);
                make_clone(g_spr[src], g_spr[total++]);
            }
            for (int i = total; i < SS_MAX_SPRITES; i++) g_spr[i] = Sprite{};

            g_n_owners  = new_owners;
            g_spr_count = total;
        }
        g_frames_since_load = 0;
    }
#endif // SCREENSAVER_MODE_BOUNCE

    for (int i = 0; i < g_spr_count; i++) step_one(g_spr[i]);
#if SCREENSAVER_MODE == SCREENSAVER_MODE_BOUNCE
    for (int i = 0; i < g_spr_count; i++) {
        for (int j = i + 1; j < g_spr_count; j++) {
            resolve_pair(g_spr[i], g_spr[j]);
        }
    }
#endif
    draw();
}

void screensaver_free(void)
{
    free_sprites(g_spr, g_n_owners, g_spr_count);
    g_n_owners          = 0;
    g_spr_count         = 0;
    g_frames_since_load = 0;
    free_scratch();
    if (g_chosen) {
        Frens::f_free(g_chosen);
        g_chosen = nullptr;
    }
}

} // extern "C"
