#include "themes.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "FrensHelpers.h"
#include "ff.h"
#include "image_convert.h"

// C++ rather than C so we can call image_convert_batch_dir(), whose header has
// no extern "C" wrapper. Same arrangement as gui.cpp / screensaver.cpp.

namespace {

// "<base>/assets" and "<base>/assets/themes". Defaults match the historical
// BASEDIR so the module is usable even if themes_init() is never called.
char     s_assets[72] = "/emu/assets";
char     s_themes[80] = "/emu/assets/themes";
uint32_t s_mask       = 0;
int      s_active     = 0;

// Extensions we are willing to relocate during migration. The .444/.555 cache
// files are in the list on purpose -- moving them along with their source
// keeps the conversion cache warm so nothing is needlessly re-converted.
bool is_asset_name(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".png")  == 0 ||
           strcasecmp(dot, ".jpg")  == 0 ||
           strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".444")  == 0 ||
           strcasecmp(dot, ".555")  == 0;
}

void marker_path(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/.migrating", s_themes);
}

// f_mkdir that treats "already there" as success.
bool ensure_dir(const char *path)
{
    FRESULT fr = f_mkdir(path);
    return fr == FR_OK || fr == FR_EXIST;
}

} // namespace

extern "C" {

void themes_init(const char *base_dir)
{
    if (!base_dir || !*base_dir) return;
    snprintf(s_assets, sizeof(s_assets), "%s/assets",        base_dir);
    snprintf(s_themes, sizeof(s_themes), "%s/assets/themes", base_dir);
}

void themes_dir_of(int n, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    if (n < 0 || n >= THEME_MAX) n = 0;
    snprintf(out, out_sz, "%s/%d", s_themes, n);
}

bool themes_migration_needed(void)
{
    FILINFO *fi = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    if (!fi) return false;   // can't tell; don't risk a half-done move

    bool needed = false;

    // An interrupted run leaves the marker behind. themes/0 is non-empty by
    // then, so without this check the remaining files would be stranded.
    char marker[96];
    marker_path(marker, sizeof(marker));
    if (f_stat(marker, fi) == FR_OK) {
        printf("[bootLoader] themes: .migrating marker present, resuming\n");
        Frens::f_free(fi);
        return true;
    }

    char t0[96];
    themes_dir_of(0, t0, sizeof(t0));

    DIR d;
    if (f_opendir(&d, t0) != FR_OK) {
        needed = true;                       // themes/0 does not exist
    } else {
        bool found = false;
        while (f_readdir(&d, fi) == FR_OK && fi->fname[0]) {
            if (!(fi->fattrib & AM_DIR)) { found = true; break; }
        }
        f_closedir(&d);
        needed = !found;                     // themes/0 exists but is empty
    }

    Frens::f_free(fi);
    return needed;
}

int themes_migrate_default(void)
{
    // One allocation, one free. queue[] is what lets us avoid renaming entries
    // out of a directory while f_readdir is iterating it -- FatFs gives no
    // guarantee there and can skip entries. Collect a batch, close the dir,
    // move the batch, repeat until a pass finds nothing left.
    struct MigScratch {
        FILINFO  fi;
        char     src[FF_MAX_LFN + 1];
        char     dst[FF_MAX_LFN + 1];
        char     queue[32][FF_MAX_LFN + 1];
        uint32_t qsize[32];              // carried alongside purely for the log
    };
    MigScratch *sc = (MigScratch *)Frens::f_malloc(sizeof(MigScratch));
    if (!sc) {
        printf("[bootLoader] themes: migration scratch alloc failed\n");
        return -1;
    }

    char t0[96];
    themes_dir_of(0, t0, sizeof(t0));
    if (!ensure_dir(s_themes) || !ensure_dir(t0)) {
        printf("[bootLoader] themes: cannot create %s\n", t0);
        Frens::f_free(sc);
        return -1;
    }

    char marker[96];
    marker_path(marker, sizeof(marker));
    {
        FIL m;
        if (f_open(&m, marker, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) f_close(&m);
    }

    printf("[bootLoader] themes: migrating %s -> %s\n", s_assets, t0);

    int      moved   = 0;
    int      failed  = 0;
    int      skipped = 0;
    uint32_t bytes   = 0;
    int      pass    = 0;

    for (;;) {
        DIR d;
        FRESULT fr = f_opendir(&d, s_assets);
        if (fr != FR_OK) {
            printf("[bootLoader] themes: cannot open %s (fr=%d)\n", s_assets, (int)fr);
            break;
        }
        pass++;

        int n = 0;
        while (n < 32 && f_readdir(&d, &sc->fi) == FR_OK && sc->fi.fname[0]) {
            if (sc->fi.fattrib & AM_DIR) {
                // screensaver/, unsupported/, themes/ -- reported once so the
                // log shows they were seen and deliberately left alone.
                if (pass == 1) {
                    printf("[bootLoader] themes:   skip dir  %s\n", sc->fi.fname);
                }
                continue;
            }
            if (!is_asset_name(sc->fi.fname)) {
                if (pass == 1) {
                    printf("[bootLoader] themes:   skip file %s (not an image)\n",
                           sc->fi.fname);
                    skipped++;
                }
                continue;
            }
            size_t len = strlen(sc->fi.fname);
            if (len >= sizeof(sc->queue[0])) {
                printf("[bootLoader] themes:   skip file (name too long, %u chars)\n",
                       (unsigned)len);
                skipped++;
                continue;
            }
            sc->qsize[n] = (uint32_t)sc->fi.fsize;
            memcpy(sc->queue[n], sc->fi.fname, len + 1);
            n++;
        }
        f_closedir(&d);
        if (n == 0) break;

        printf("[bootLoader] themes: pass %d, %d file(s) queued\n", pass, n);

        for (int i = 0; i < n; i++) {
            snprintf(sc->src, sizeof(sc->src), "%s/%s", s_assets, sc->queue[i]);
            snprintf(sc->dst, sizeof(sc->dst), "%s/%s", t0,       sc->queue[i]);
            f_unlink(sc->dst);   // a resumed run may find a half-moved duplicate
            FRESULT mv = f_rename(sc->src, sc->dst);
            if (mv == FR_OK) {
                moved++;
                bytes += sc->qsize[i];
                printf("[bootLoader] themes:   [%3d] moved %s (%u bytes)\n",
                       moved, sc->queue[i], (unsigned)sc->qsize[i]);
            } else {
                failed++;
                printf("[bootLoader] themes:   [ ! ] move %s FAILED (fr=%d)\n",
                       sc->queue[i], (int)mv);
            }
        }
    }

    f_unlink(marker);
    Frens::f_free(sc);
    printf("[bootLoader] themes: done -- %d moved (%u bytes), %d failed, %d skipped, %d pass(es)\n",
           moved, (unsigned)bytes, failed, skipped, pass);
    return moved;
}

uint32_t themes_scan(void)
{
    FILINFO *fi = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    if (!fi) { s_mask = 0; return 0; }

    s_mask = 0;
    DIR d;
    if (f_opendir(&d, s_themes) == FR_OK) {
        while (f_readdir(&d, fi) == FR_OK && fi->fname[0]) {
            // The AM_DIR gate also hides the .migrating marker from the scan.
            if (!(fi->fattrib & AM_DIR)) continue;
            if (fi->fname[1] == '\0' && fi->fname[0] >= '0' && fi->fname[0] <= '9') {
                s_mask |= 1u << (fi->fname[0] - '0');
            }
        }
        f_closedir(&d);
    }

    Frens::f_free(fi);
    return s_mask;
}

uint32_t themes_mask(void)   { return s_mask; }
bool     themes_exists(int n){ return n >= 0 && n < THEME_MAX && (s_mask & (1u << n)) != 0; }
int      themes_active(void) { return s_active; }

int themes_count(void)
{
    int c = 0;
    for (int n = 0; n < THEME_MAX; n++) if (s_mask & (1u << n)) c++;
    return c;
}

int themes_lowest(void)
{
    for (int n = 0; n < THEME_MAX; n++) if (s_mask & (1u << n)) return n;
    return -1;
}

int themes_next(int cur, int dir)
{
    if (themes_count() < 2) return cur;
    if (dir == 0) return cur;
    int step = (dir > 0) ? 1 : (THEME_MAX - 1);
    int n = cur;
    for (int i = 0; i < THEME_MAX; i++) {
        n = (n + step) % THEME_MAX;
        if (themes_exists(n)) return n;
    }
    return cur;
}

void themes_set_active(int n)
{
    if (themes_exists(n)) { s_active = n; return; }
    int low = themes_lowest();
    s_active = (low >= 0) ? low : 0;
}

void themes_convert_all(void)
{
    char dir[96];
    char title[32];
    for (int n = 0; n < THEME_MAX; n++) {
        if (!(s_mask & (1u << n))) continue;
        themes_dir_of(n, dir, sizeof(dir));
        snprintf(title, sizeof(title), "Converting theme %d", n);
        image_convert_batch_dir(dir, SCREENWIDTH, SCREENHEIGHT,
                                /*letterbox=*/true, title);
    }
}

} // extern "C"
