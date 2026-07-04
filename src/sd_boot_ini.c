#include "sd_boot_ini.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ff.h"

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
    out->screensaver = SS_MODE_STARFIELD;

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
    out->screensaver = SS_MODE_BLOCKS;

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
        } else {
            snprintf_err(err, err_sz, "unknown key '%s' at line %d", key, lineno);
            f_close(&fil);
            return SD_BOOT_INI_ERROR;
        }
    }
    f_close(&fil);

    printf("[bootLoader] boot_ini: %s parsed OK (BASEDIR=%s INDEX=%s SCREENSAVER=%s)\n",
           path, out->base_dir, out->index_file,
           out->screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS");
    return SD_BOOT_INI_OK;
}
