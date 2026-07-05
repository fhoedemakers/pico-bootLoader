#include "emulators_txt.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "ff.h"

#define MAX_ROWS         16
#define PROG_NAME_MAX    32
#define IMAGE_KEY_MAX    16
#define DISPLAY_NAME_MAX 40
#define AUX_UF2_MAX      64

typedef struct {
    char prog_name[PROG_NAME_MAX];
    char image_key[IMAGE_KEY_MAX];
    char display_name[DISPLAY_NAME_MAX];
    char aux_uf2[AUX_UF2_MAX];         /* empty if row has no 4th column */
} row_t;

static row_t s_rows[MAX_ROWS];
static int   s_row_count = 0;

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void copy_field(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    trim(dst);
}

static void parse_line(const char *line)
{
    if (s_row_count >= MAX_ROWS) return;

    char buf[256];
    size_t n = strlen(line);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';
    trim(buf);
    if (buf[0] == '\0' || buf[0] == '#') return;

    char *s1 = strchr(buf, ';');
    if (!s1) return;
    *s1++ = '\0';
    char *s2 = strchr(s1, ';');
    if (!s2) return;
    *s2++ = '\0';

    /* Optional 4th field: aux_uf2. If missing, split() leaves it empty. */
    char *s3 = strchr(s2, ';');
    if (s3) *s3++ = '\0';

    row_t *r = &s_rows[s_row_count];
    copy_field(r->prog_name,    sizeof(r->prog_name),    buf);
    copy_field(r->image_key,    sizeof(r->image_key),    s1);
    copy_field(r->display_name, sizeof(r->display_name), s2);
    copy_field(r->aux_uf2,      sizeof(r->aux_uf2),      s3 ? s3 : "");
    if (r->prog_name[0]) s_row_count++;
}

bool emulators_txt_load(const char *path)
{
    s_row_count = 0;

    FIL fil;
    FRESULT fr = f_open(&fil, path, FA_READ);
    if (fr != FR_OK) {
        printf("[bootLoader] emulators_txt: cannot open %s (fr=%d)\n", path, fr);
        return false;
    }

    char line[256];
    while (f_gets(line, sizeof(line), &fil)) {
        parse_line(line);
    }
    f_close(&fil);

    printf("[bootLoader] emulators_txt: %d row(s) loaded from %s\n", s_row_count, path);
    return s_row_count > 0;
}

bool emulators_txt_lookup(const char *prog_name,
                          char *image_key, size_t key_sz,
                          char *display_name, size_t name_sz,
                          char *aux_uf2, size_t aux_sz)
{
    if (image_key && key_sz)     image_key[0]    = '\0';
    if (display_name && name_sz) display_name[0] = '\0';
    if (aux_uf2 && aux_sz)       aux_uf2[0]      = '\0';

    if (!prog_name || !*prog_name) return false;

    for (int i = 0; i < s_row_count; i++) {
        if (strcasecmp(s_rows[i].prog_name, prog_name) == 0) {
            if (image_key && key_sz)     copy_field(image_key,    key_sz,  s_rows[i].image_key);
            if (display_name && name_sz) copy_field(display_name, name_sz, s_rows[i].display_name);
            if (aux_uf2 && aux_sz)       copy_field(aux_uf2,      aux_sz,  s_rows[i].aux_uf2);
            return true;
        }
    }
    return false;
}
