#include "sd_boot_ini.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ff.h"

/* C-linkage shims over Frens::f_malloc / f_free (FrensHelpers.cpp). Declared
 * here rather than pulled in from a header because FrensHelpers.h is C++ only;
 * this is the same arrangement hstx_data_island_queue.c uses. */
extern void *frens_f_malloc(size_t size);
extern void  frens_f_free(void *ptr);

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void snprintf_err(char *err, size_t err_sz, const char *fmt, ...)
{
    if (!err || err_sz == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_sz, fmt, ap);
    va_end(ap);
}

sd_boot_ini_status_t sd_boot_ini_load(const char *path,
                                      sd_boot_ini_t *out,
                                      char *err, size_t err_sz)
{
    if (err && err_sz) err[0] = '\0';
    if (!out) return SD_BOOT_INI_ERROR;

    /* Fallback defaults (file absent). */
    strcpy(out->base_dir,   "/emu");
    strcpy(out->index_file, "emulators.txt");
    out->screensaver    = SS_MODE_STARFIELD;
    out->gui_graphical  = true;     /* same first-boot default .guimode had */
    out->theme          = 0;
    out->file_present   = false;
    out->seen_gui       = false;
    out->seen_theme     = false;

    /* Adopt a leftover .tmp when the real file is gone. sd_boot_ini_save()
     * commits with unlink+rename; a power cut in between leaves only the tmp,
     * which is always complete AND already re-parsed at that point. Without
     * this the config would silently revert to "absent" defaults. */
    {
        char tmp[80];
        FILINFO fi;
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        if (f_stat(path, &fi) != FR_OK && f_stat(tmp, &fi) == FR_OK) {
            if (f_rename(tmp, path) == FR_OK) {
                printf("[bootLoader] boot_ini: recovered %s from %s\n", path, tmp);
            }
        }
    }

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        printf("[bootLoader] boot_ini: %s absent, using defaults\n", path);
        return SD_BOOT_INI_OK;
    }
    if (fr != FR_OK) {
        snprintf_err(err, err_sz, "cannot open %s (fr=%d)", path, (int)fr);
        return SD_BOOT_INI_ERROR;
    }

    /* File present: flip screensaver default to BLOCKS per spec. */
    out->screensaver  = SS_MODE_BLOCKS;
    out->file_present = true;

    bool seen_basedir = false;
    bool seen_index   = false;
    bool seen_screen  = false;

    char line[256];
    int lineno = 0;
    while (f_gets(line, sizeof(line), &fil)) {
        lineno++;

        char buf[256];
        size_t n = strlen(line);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, line, n);
        buf[n] = '\0';
        trim(buf);

        if (buf[0] == '\0' || buf[0] == '#' || buf[0] == ';') continue;

        char *eq = strchr(buf, '=');
        if (!eq) {
            snprintf_err(err, err_sz, "missing '=' at line %d", lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
        *eq = '\0';
        char *key = buf;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (key[0] == '\0') {
            snprintf_err(err, err_sz, "empty key at line %d", lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
        if (val[0] == '\0') {
            snprintf_err(err, err_sz, "empty value for %s at line %d", key, lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }

        if (strcasecmp(key, "BASEDIR") == 0) {
            if (seen_basedir) {
                snprintf_err(err, err_sz, "duplicate key BASEDIR at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] != '/') {
                snprintf_err(err, err_sz, "BASEDIR must start with '/' at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl > 1 && val[vl - 1] == '/') { val[vl - 1] = '\0'; vl--; }
            if (vl >= sizeof(out->base_dir)) {
                snprintf_err(err, err_sz, "BASEDIR too long at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            memcpy(out->base_dir, val, vl + 1);
            seen_basedir = true;
        } else if (strcasecmp(key, "INDEX") == 0) {
            if (seen_index) {
                snprintf_err(err, err_sz, "duplicate key INDEX at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (strchr(val, '/') || strchr(val, '\\')) {
                snprintf_err(err, err_sz, "INDEX must be a bare filename at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            size_t vl = strlen(val);
            if (vl >= sizeof(out->index_file)) {
                snprintf_err(err, err_sz, "INDEX too long at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            memcpy(out->index_file, val, vl + 1);
            seen_index = true;
        } else if (strcasecmp(key, "SCREENSAVER") == 0) {
            if (seen_screen) {
                snprintf_err(err, err_sz, "duplicate key SCREENSAVER at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (strcasecmp(val, "BLOCKS") == 0) {
                out->screensaver = SS_MODE_BLOCKS;
            } else if (strcasecmp(val, "STARFIELD") == 0) {
                out->screensaver = SS_MODE_STARFIELD;
            } else {
                snprintf_err(err, err_sz, "invalid SCREENSAVER value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            seen_screen = true;
        } else if (strcasecmp(key, "GUI") == 0) {
            if (out->seen_gui) {
                snprintf_err(err, err_sz, "duplicate key GUI at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if ((val[0] != '0' && val[0] != '1') || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid GUI value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->gui_graphical = (val[0] == '1');
            out->seen_gui = true;
        } else if (strcasecmp(key, "THEME") == 0) {
            if (out->seen_theme) {
                snprintf_err(err, err_sz, "duplicate key THEME at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            if (val[0] < '0' || val[0] > '9' || val[1] != '\0') {
                snprintf_err(err, err_sz, "invalid THEME value at line %d", lineno);
                f_close(&fil);
                return SD_BOOT_INI_ERROR;
            }
            out->theme = (uint8_t)(val[0] - '0');
            out->seen_theme = true;
        } else {
            snprintf_err(err, err_sz, "unknown key '%s' at line %d", key, lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
    }
    f_close(&fil);

    printf("[bootLoader] boot_ini: %s parsed OK (BASEDIR=%s INDEX=%s SCREENSAVER=%s GUI=%d THEME=%u)\n",
           path, out->base_dir, out->index_file,
           out->screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS",
           (int)out->gui_graphical, (unsigned)out->theme);
    return SD_BOOT_INI_OK;
}

/* ------------------------------------------------------------------------
 * Writer
 * ---------------------------------------------------------------------- */

/* Every write goes through f_write. f_puts / f_printf are not referenced
 * anywhere else in this image and the build has no --gc-sections, so calling
 * them would newly link FatFs's own formatter for no benefit. snprintf is
 * already linked all over the place. */
static bool write_raw(FIL *f, const char *s, size_t n)
{
    UINT bw = 0;
    if (n == 0) return true;
    return f_write(f, s, (UINT)n, &bw) == FR_OK && bw == (UINT)n;
}

static bool write_str(FIL *f, const char *s)
{
    return write_raw(f, s, strlen(s));
}

/* 0 = not one of ours, 1 = GUI, 2 = THEME. Non-destructive: `line` is left
 * untouched so it can still be echoed verbatim. */
static int classify(const char *line, char *work, size_t work_sz)
{
    size_t n = 0;
    while (line[n] && line[n] != '=' && n + 1 < work_sz) {
        work[n] = line[n];
        n++;
    }
    if (line[n] != '=') return 0;      /* no '=' on this line */
    work[n] = '\0';
    trim(work);
    if (work[0] == '#' || work[0] == ';') return 0;
    if (strcasecmp(work, "GUI")   == 0) return 1;
    if (strcasecmp(work, "THEME") == 0) return 2;
    return 0;
}

bool sd_boot_ini_save(const char *path, const sd_boot_ini_t *ini)
{
    if (!path || !ini) return false;

    /* sizeof(FIL) is ~560 bytes here (FF_FS_TINY=0, FF_MAX_SS=512) and we hold
     * two of them open at once, which would not fit the 3 KB stack. One
     * allocation, one free on every exit path. */
    struct SaveScratch {
        FIL  fin;
        FIL  fout;
        char line[256];
        char work[64];
        char out[128];
    };
    struct SaveScratch *sc = (struct SaveScratch *)frens_f_malloc(sizeof(struct SaveScratch));
    if (!sc) {
        printf("[bootLoader] boot_ini: save scratch alloc failed\n");
        return false;
    }

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    if (f_open(&sc->fout, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        printf("[bootLoader] boot_ini: cannot create %s\n", tmp);
        frens_f_free(sc);
        return false;
    }

    bool ok           = true;
    bool wrote_gui    = false;
    bool wrote_theme  = false;
    bool last_had_eol = true;

    if (f_open(&sc->fin, path, FA_READ) == FR_OK) {
        /* File present: copy through, substituting only our two keys. */
        bool continuation = false;
        while (f_gets(sc->line, sizeof(sc->line), &sc->fin)) {
            size_t len     = strlen(sc->line);
            bool   has_eol = (len > 0 && sc->line[len - 1] == '\n');

            /* f_gets splits lines longer than the buffer. The tail chunk is
             * not a line start, so it must never be classified as a key. */
            int kind = continuation ? 0 : classify(sc->line, sc->work, sizeof(sc->work));
            continuation = !has_eol;

            if (kind == 1 && !wrote_gui) {
                snprintf(sc->out, sizeof(sc->out), "GUI=%d\n", ini->gui_graphical ? 1 : 0);
                ok = ok && write_str(&sc->fout, sc->out);
                wrote_gui = true;
            } else if (kind == 2 && !wrote_theme) {
                snprintf(sc->out, sizeof(sc->out), "THEME=%u\n", (unsigned)ini->theme);
                ok = ok && write_str(&sc->fout, sc->out);
                wrote_theme = true;
            } else if (kind != 0) {
                /* Duplicate key -- drop it. The parser rejects duplicates, so
                 * echoing it would produce a file that fails to load. */
            } else {
                ok = ok && write_raw(&sc->fout, sc->line, len);
            }
            last_had_eol = has_eol;
        }
        f_close(&sc->fin);
    } else {
        /* File absent: materialise every key at its EFFECTIVE value.
         *
         * SCREENSAVER must be written explicitly. sd_boot_ini_load() defaults
         * it to STARFIELD when /boot.txt is absent but to BLOCKS when the file
         * exists without the key, so creating the file while leaving the key
         * out would silently change the screensaver on the next boot. */
        ok = ok && write_str(&sc->fout,
                             "# pico-bootLoader configuration.\n"
                             "# Created by the bootloader. Edit freely -- comments and\n"
                             "# any keys you add are preserved; only GUI and THEME are\n"
                             "# rewritten when you change them from the menu.\n"
                             "# See boot.txt in the project repository for the full reference.\n\n");
        snprintf(sc->out, sizeof(sc->out), "BASEDIR=%s\n", ini->base_dir);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "INDEX=%s\n", ini->index_file);
        ok = ok && write_str(&sc->fout, sc->out);
        snprintf(sc->out, sizeof(sc->out), "SCREENSAVER=%s\n",
                 ini->screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS");
        ok = ok && write_str(&sc->fout, sc->out);
    }

    /* Don't glue an appended key onto a file that ended without a newline. */
    if (!last_had_eol) ok = ok && write_str(&sc->fout, "\n");

    if (!wrote_gui) {
        snprintf(sc->out, sizeof(sc->out),
                 "\n# 0 = text menu, 1 = graphical menu (SELECT toggles).\nGUI=%d\n",
                 ini->gui_graphical ? 1 : 0);
        ok = ok && write_str(&sc->fout, sc->out);
    }
    if (!wrote_theme) {
        snprintf(sc->out, sizeof(sc->out),
                 "\n# Artwork theme 0..9 (UP/DOWN in graphical mode).\nTHEME=%u\n",
                 (unsigned)ini->theme);
        ok = ok && write_str(&sc->fout, sc->out);
    }

    if (f_close(&sc->fout) != FR_OK) ok = false;

    /* Validate before committing: reusing the parser costs nothing and makes
     * it impossible to ship a file that would trip the fatal screen. */
    if (ok) {
        sd_boot_ini_t probe;
        char probe_err[64];
        if (sd_boot_ini_load(tmp, &probe, probe_err, sizeof(probe_err)) != SD_BOOT_INI_OK) {
            printf("[bootLoader] boot_ini: refusing to commit %s (%s)\n", tmp, probe_err);
            ok = false;
        }
    }

    if (!ok) {
        f_unlink(tmp);
        frens_f_free(sc);
        return false;
    }

    /* f_rename fails with FR_EXIST if the destination is there. */
    f_unlink(path);
    if (f_rename(tmp, path) != FR_OK) {
        printf("[bootLoader] boot_ini: rename %s -> %s failed\n", tmp, path);
        f_unlink(tmp);
        frens_f_free(sc);
        return false;
    }

    frens_f_free(sc);
    printf("[bootLoader] boot_ini: saved %s (GUI=%d THEME=%u)\n",
           path, (int)ini->gui_graphical, (unsigned)ini->theme);
    return true;
}
