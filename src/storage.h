/*
 * storage.h - Minimal read-only filesystem abstraction.
 *
 * The bootloader only ever needs to (1) list the *.uf2 files in the root
 * directory and (2) stream one of them in 512-byte chunks. Keeping that behind
 * this small interface means the FatFs/SD-card backend (storage_fatfs.c) can be
 * swapped for something else (USB MSC, a test stub, a different FS) without
 * touching the loader or the menu.
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#define STORAGE_MAX_NAME 256

typedef struct {
    char     name[STORAGE_MAX_NAME];
    uint32_t size;
} storage_entry_t;

/* Mount the volume. Returns true on success. */
bool storage_mount(void);
void storage_unmount(void);

/* List up to max *.uf2 files in the root directory. Writes the number found to
 * *count. Returns true on success (even if zero files were found). */
bool storage_list_uf2(storage_entry_t *out, int max, int *count);

/* Open a file in the root directory for reading. One file open at a time. */
bool storage_open(const char *name);
/* Read up to len bytes; *read receives the actual count (0 at EOF). */
bool storage_read(void *buf, uint32_t len, uint32_t *read);
/* Seek back to the start of the currently open file. */
bool storage_rewind(void);
/* Seek to an absolute byte offset within the currently open file. */
bool storage_seek(uint32_t offset);
void storage_close(void);

#endif /* STORAGE_H */
