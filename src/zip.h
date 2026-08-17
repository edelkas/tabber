/*
 * zip.h - Read-only ZIP archive reader working entirely in memory.
 *
 * Supports the two methods PKZIP writers use for this kind of content: stored
 * (0) and deflate (8). Every entry read is checked against its CRC-32, so a
 * corrupt archive is caught before anything reaches the disk. ZIP64 archives
 * are detected and rejected rather than mis-parsed.
 */
#ifndef TABBER_ZIP_H
#define TABBER_ZIP_H

#include <stddef.h>

/* Compression methods we understand. */
#define ZIP_METHOD_STORE    0
#define ZIP_METHOD_DEFLATE  8

typedef struct {
    char *name;            /* path inside the archive, '/' separated */
    unsigned long crc32;
    size_t comp_size;
    size_t uncomp_size;
    unsigned method;
    size_t local_offset;   /* offset of the entry's local header */
    int is_dir;            /* directory entry (name ends with '/') */
} zip_entry;

typedef struct {
    const unsigned char *data;   /* borrowed: the caller owns the archive bytes */
    size_t size;
    zip_entry *entries;
    size_t count;
} zip_archive;

/*
 * Parses the central directory of an in-memory archive. The buffer must stay
 * alive for as long as the archive is used. Returns 0 on success.
 */
int zip_open(zip_archive *zip, const void *data, size_t size, char *err, size_t errsz);

void zip_close(zip_archive *zip);

/* Entry with this exact path, matched case-insensitively. NULL when absent. */
const zip_entry *zip_find(const zip_archive *zip, const char *name);

/*
 * Decompresses one entry into a freshly allocated buffer of uncomp_size bytes
 * (plus a NUL terminator for safety) and checks its CRC-32. NULL on failure.
 */
unsigned char *zip_read(const zip_archive *zip, const zip_entry *entry,
                        char *err, size_t errsz);

/* Sum of the uncompressed sizes of every entry. */
size_t zip_total_uncompressed(const zip_archive *zip);

/*
 * Rejects entry names that must never be written to disk: absolute paths,
 * drive letters and any ".." component (the "zip slip" escape).
 */
int zip_name_is_safe(const char *name);

#endif /* TABBER_ZIP_H */
