#include <stdlib.h>
#include <string.h>

#include "inflate.h"
#include "util.h"
#include "zip.h"

/* Record signatures. */
#define SIG_LOCAL           0x04034b50UL
#define SIG_CENTRAL         0x02014b50UL
#define SIG_EOCD            0x06054b50UL

/* End of central directory record: minimum size and field offsets. */
#define EOCD_SIZE           22
#define EOCD_ENTRY_COUNT    10
#define EOCD_CD_SIZE        12
#define EOCD_CD_OFFSET      16
#define EOCD_COMMENT_LEN    20

/* Central directory header: fixed size and field offsets. */
#define CD_SIZE             46
#define CD_FLAGS            8
#define CD_METHOD           10
#define CD_CRC32            16
#define CD_COMP_SIZE        20
#define CD_UNCOMP_SIZE      24
#define CD_NAME_LEN         28
#define CD_EXTRA_LEN        30
#define CD_COMMENT_LEN      32
#define CD_LOCAL_OFFSET     42

/* Local file header: fixed size and field offsets. */
#define LOCAL_SIZE          30
#define LOCAL_NAME_LEN      26
#define LOCAL_EXTRA_LEN     28

#define ZIP_MAX_COMMENT     65535        /* the comment length field is 16-bit */
#define ZIP_MAX_ENTRIES     100000       /* sanity cap for hostile archives    */
#define ZIP64_MARKER        0xFFFFFFFFUL /* "look in the zip64 extra field"    */
#define ZIP64_COUNT_MARKER  0xFFFF
#define ZIP_FLAG_ENCRYPTED  0x0001

/* ---- Little-endian readers --------------------------------------------- */

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- CRC-32 (IEEE 802.3, as used by ZIP) -------------------------------- */

#define CRC32_POLY 0xEDB88320UL

static unsigned long crc32_table[256];
static int crc32_ready = 0;

static void crc32_init(void)
{
    unsigned long c;
    int i, bit;

    for (i = 0; i < 256; i++) {
        c = (unsigned long)i;
        for (bit = 0; bit < 8; bit++)
            c = (c & 1) ? (CRC32_POLY ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = 1;
}

static unsigned long crc32_bytes(const unsigned char *data, size_t len)
{
    unsigned long crc = 0xFFFFFFFFUL;
    size_t i;

    if (!crc32_ready)
        crc32_init();
    for (i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

/* ---- Archive parsing ---------------------------------------------------- */

/* Scans backwards for the end of central directory record. */
static const unsigned char *zip_find_eocd(const unsigned char *data, size_t size)
{
    size_t max_back = size < (size_t)(EOCD_SIZE + ZIP_MAX_COMMENT)
                    ? size : (size_t)(EOCD_SIZE + ZIP_MAX_COMMENT);
    size_t offset;

    if (size < EOCD_SIZE)
        return NULL;
    for (offset = EOCD_SIZE; offset <= max_back; offset++) {
        const unsigned char *p = data + size - offset;
        if (rd32(p) == SIG_EOCD)
            return p;
    }
    return NULL;
}

int zip_open(zip_archive *zip, const void *data, size_t size, char *err, size_t errsz)
{
    const unsigned char *bytes = data;
    const unsigned char *eocd, *p;
    unsigned long cd_size, cd_offset;
    size_t count, i;

    memset(zip, 0, sizeof(*zip));
    zip->data = bytes;
    zip->size = size;

    eocd = zip_find_eocd(bytes, size);
    if (!eocd) {
        err_set(err, errsz, "not a ZIP archive (no end of central directory)");
        return -1;
    }

    count     = rd16(eocd + EOCD_ENTRY_COUNT);
    cd_size   = rd32(eocd + EOCD_CD_SIZE);
    cd_offset = rd32(eocd + EOCD_CD_OFFSET);

    if (count == ZIP64_COUNT_MARKER || cd_size == ZIP64_MARKER || cd_offset == ZIP64_MARKER) {
        err_set(err, errsz, "ZIP64 archives are not supported");
        return -1;
    }
    if (count > ZIP_MAX_ENTRIES) {
        err_set(err, errsz, "archive holds more than %d entries", ZIP_MAX_ENTRIES);
        return -1;
    }
    if ((size_t)cd_offset > size || (size_t)cd_size > size - (size_t)cd_offset) {
        err_set(err, errsz, "central directory lies outside the archive");
        return -1;
    }

    zip->entries = count ? xmalloc(count * sizeof(*zip->entries)) : NULL;
    zip->count = 0;

    p = bytes + cd_offset;
    for (i = 0; i < count; i++) {
        zip_entry *entry;
        size_t name_len, extra_len, comment_len, header_len;

        if ((size_t)(bytes + size - p) < CD_SIZE || rd32(p) != SIG_CENTRAL) {
            err_set(err, errsz, "corrupt central directory at entry %u", (unsigned)i);
            zip_close(zip);
            return -1;
        }

        name_len    = rd16(p + CD_NAME_LEN);
        extra_len   = rd16(p + CD_EXTRA_LEN);
        comment_len = rd16(p + CD_COMMENT_LEN);
        header_len  = CD_SIZE + name_len + extra_len + comment_len;
        if ((size_t)(bytes + size - p) < header_len) {
            err_set(err, errsz, "truncated central directory at entry %u", (unsigned)i);
            zip_close(zip);
            return -1;
        }

        if (rd16(p + CD_FLAGS) & ZIP_FLAG_ENCRYPTED) {
            err_set(err, errsz, "encrypted archives are not supported");
            zip_close(zip);
            return -1;
        }
        if (rd32(p + CD_COMP_SIZE) == ZIP64_MARKER ||
            rd32(p + CD_UNCOMP_SIZE) == ZIP64_MARKER ||
            rd32(p + CD_LOCAL_OFFSET) == ZIP64_MARKER) {
            err_set(err, errsz, "ZIP64 entries are not supported");
            zip_close(zip);
            return -1;
        }

        entry = &zip->entries[zip->count];
        entry->name = str_fmt("%.*s", (int)name_len, (const char *)(p + CD_SIZE));
        entry->method = rd16(p + CD_METHOD);
        entry->crc32 = rd32(p + CD_CRC32);
        entry->comp_size = (size_t)rd32(p + CD_COMP_SIZE);
        entry->uncomp_size = (size_t)rd32(p + CD_UNCOMP_SIZE);
        entry->local_offset = (size_t)rd32(p + CD_LOCAL_OFFSET);
        entry->is_dir = name_len > 0 && entry->name[name_len - 1] == '/';
        zip->count++;

        p += header_len;
    }

    return 0;
}

void zip_close(zip_archive *zip)
{
    size_t i;

    for (i = 0; i < zip->count; i++)
        free(zip->entries[i].name);
    free(zip->entries);
    memset(zip, 0, sizeof(*zip));
}

const zip_entry *zip_find(const zip_archive *zip, const char *name)
{
    size_t i;

    for (i = 0; i < zip->count; i++) {
        if (str_ieq(zip->entries[i].name, name))
            return &zip->entries[i];
    }
    return NULL;
}

size_t zip_total_uncompressed(const zip_archive *zip)
{
    size_t total = 0, i;

    for (i = 0; i < zip->count; i++)
        total += zip->entries[i].uncomp_size;
    return total;
}

unsigned char *zip_read(const zip_archive *zip, const zip_entry *entry,
                        char *err, size_t errsz)
{
    const unsigned char *local;
    size_t data_offset;
    unsigned char *out;
    unsigned long crc;

    /* The local header repeats the name and extra fields, with its own
     * lengths, so the data offset can only be computed from it. */
    if (entry->local_offset > zip->size || zip->size - entry->local_offset < LOCAL_SIZE) {
        err_set(err, errsz, "'%s': local header outside the archive", entry->name);
        return NULL;
    }
    local = zip->data + entry->local_offset;
    if (rd32(local) != SIG_LOCAL) {
        err_set(err, errsz, "'%s': corrupt local header", entry->name);
        return NULL;
    }

    data_offset = entry->local_offset + LOCAL_SIZE +
                  rd16(local + LOCAL_NAME_LEN) + rd16(local + LOCAL_EXTRA_LEN);
    if (data_offset > zip->size || zip->size - data_offset < entry->comp_size) {
        err_set(err, errsz, "'%s': entry data outside the archive", entry->name);
        return NULL;
    }

    out = xmalloc(entry->uncomp_size + 1);   /* +1: keep text usable as a C string */

    switch (entry->method) {
        case ZIP_METHOD_STORE:
            if (entry->comp_size != entry->uncomp_size) {
                err_set(err, errsz, "'%s': inconsistent sizes for a stored entry", entry->name);
                free(out);
                return NULL;
            }
            memcpy(out, zip->data + data_offset, entry->uncomp_size);
            break;

        case ZIP_METHOD_DEFLATE: {
            char inflate_err[TB_ERR_LEN];
            if (inflate_raw(zip->data + data_offset, entry->comp_size,
                            out, entry->uncomp_size, inflate_err, sizeof inflate_err) != 0) {
                err_set(err, errsz, "'%s': %s", entry->name, inflate_err);
                free(out);
                return NULL;
            }
            break;
        }

        default:
            err_set(err, errsz, "'%s': unsupported compression method %u",
                    entry->name, entry->method);
            free(out);
            return NULL;
    }

    crc = crc32_bytes(out, entry->uncomp_size);
    if (crc != entry->crc32) {
        err_set(err, errsz, "'%s': CRC mismatch (expected %08lx, got %08lx)",
                entry->name, entry->crc32, crc);
        free(out);
        return NULL;
    }

    out[entry->uncomp_size] = '\0';
    return out;
}

int zip_name_is_safe(const char *name)
{
    const char *p = name;

    if (!name || !*name)
        return 0;
    if (name[0] == '/' || name[0] == '\\')
        return 0;                                   /* absolute path */
    if (name[1] == ':')
        return 0;                                   /* drive letter  */

    /* Walk the components, rejecting any that escape upwards. */
    while (*p) {
        const char *start = p;
        size_t len;

        while (*p && *p != '/' && *p != '\\')
            p++;
        len = (size_t)(p - start);
        if (len == 2 && start[0] == '.' && start[1] == '.')
            return 0;
        if (*p)
            p++;
    }
    return 1;
}
