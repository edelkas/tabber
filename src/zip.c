#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#define ZIP64_EXTRA_ID      0x0001       /* extra field holding the real sizes */
#define ZIP64_EXTRA_HEAD    4            /* its id and length fields           */
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

/* ZIP64 stores its sizes and offsets as 64-bit little-endian values. */
static unsigned long long rd64(const unsigned char *p)
{
    unsigned long long value = 0;
    int i;

    for (i = 7; i >= 0; i--)
        value = (value << 8) | p[i];
    return value;
}

/* Whether a 64-bit value can be held on this build at all. */
static int fits_size(unsigned long long value)
{
    return value <= (unsigned long long)(size_t)-1;
}

/* ---- Archive parsing ---------------------------------------------------- */

/*
 * Fills in the fields a ZIP64 entry leaves marked. The extra field is a run of
 * (id, size, payload) blocks; block 0x0001 carries the real values, in a fixed
 * order, and only for the fields that were marked in the first place.
 */
static int zip64_fields(zip_entry *entry, const unsigned char *extra, size_t len,
                        int want_uncomp, int want_comp, int want_offset,
                        char *err, size_t errsz)
{
    size_t at = 0;

    while (at + ZIP64_EXTRA_HEAD <= len) {
        unsigned id = rd16(extra + at);
        size_t size = rd16(extra + at + 2);
        const unsigned char *value = extra + at + ZIP64_EXTRA_HEAD;
        size_t left = size;

        if (at + ZIP64_EXTRA_HEAD + size > len)
            break;
        if (id != ZIP64_EXTRA_ID) {
            at += ZIP64_EXTRA_HEAD + size;
            continue;
        }

        /* The order is fixed: uncompressed size, compressed size, offset. */
        if (want_uncomp) {
            if (left < 8 || !fits_size(rd64(value)))
                goto bad;
            entry->uncomp_size = (size_t)rd64(value);
            value += 8;
            left -= 8;
        }
        if (want_comp) {
            if (left < 8 || !fits_size(rd64(value)))
                goto bad;
            entry->comp_size = (size_t)rd64(value);
            value += 8;
            left -= 8;
        }
        if (want_offset) {
            if (left < 8 || !fits_size(rd64(value)))
                goto bad;
            entry->local_offset = (size_t)rd64(value);
        }
        return 0;
    }

bad:
    err_set(err, errsz, "'%s': the ZIP64 header is missing or unusable", entry->name);
    return -1;
}

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
        entry = &zip->entries[zip->count];
        entry->name = str_fmt("%.*s", (int)name_len, (const char *)(p + CD_SIZE));
        entry->method = rd16(p + CD_METHOD);
        entry->crc32 = rd32(p + CD_CRC32);
        entry->comp_size = (size_t)rd32(p + CD_COMP_SIZE);
        entry->uncomp_size = (size_t)rd32(p + CD_UNCOMP_SIZE);
        entry->local_offset = (size_t)rd32(p + CD_LOCAL_OFFSET);
        entry->is_dir = name_len > 0 && entry->name[name_len - 1] == '/';
        zip->count++;

        /* Whatever is marked 0xFFFFFFFF really lives in the ZIP64 extra field.
         * Some writers (rubyzip, which made the savefile archives the previous
         * installers left behind) do this even for tiny entries. */
        if (rd32(p + CD_COMP_SIZE) == ZIP64_MARKER ||
            rd32(p + CD_UNCOMP_SIZE) == ZIP64_MARKER ||
            rd32(p + CD_LOCAL_OFFSET) == ZIP64_MARKER) {
            if (zip64_fields(entry, p + CD_SIZE + name_len, extra_len,
                             rd32(p + CD_UNCOMP_SIZE) == ZIP64_MARKER,
                             rd32(p + CD_COMP_SIZE) == ZIP64_MARKER,
                             rd32(p + CD_LOCAL_OFFSET) == ZIP64_MARKER,
                             err, errsz) != 0) {
                zip_close(zip);
                return -1;
            }
        }

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

const zip_entry *zip_find_prefix(const zip_archive *zip, const char *prefix)
{
    size_t i, len = strlen(prefix);

    for (i = 0; i < zip->count; i++) {
        const zip_entry *entry = &zip->entries[i];
        size_t j;

        if (entry->is_dir || strchr(entry->name, '/'))
            continue;              /* only files at the archive's root */
        if (strlen(entry->name) < len)
            continue;
        for (j = 0; j < len; j++) {
            char a = entry->name[j], b = prefix[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b)
                break;
        }
        if (j == len)
            return entry;
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

/* ---- Writing ----------------------------------------------------------- */

/* Version fields: 2.0, MS-DOS. Stored entries need nothing newer. */
#define ZIP_VERSION         20
#define DOS_EPOCH_YEAR      1980

static void wr16(byte_buf *out, unsigned value)
{
    unsigned char b[2];

    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 8) & 0xFF);
    buf_append(out, b, sizeof b);
}

static void wr32(byte_buf *out, unsigned long value)
{
    unsigned char b[4];

    b[0] = (unsigned char)(value & 0xFF);
    b[1] = (unsigned char)((value >> 8) & 0xFF);
    b[2] = (unsigned char)((value >> 16) & 0xFF);
    b[3] = (unsigned char)((value >> 24) & 0xFF);
    buf_append(out, b, sizeof b);
}

/* Now, as the packed MS-DOS date and time ZIP records timestamps in. */
static void dos_now(unsigned *date, unsigned *time_out)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    if (!tm || tm->tm_year + 1900 < DOS_EPOCH_YEAR) {
        *date = (1u << 5) | 1u;        /* the epoch itself: 1980-01-01 */
        *time_out = 0;
        return;
    }
    *date = ((unsigned)(tm->tm_year + 1900 - DOS_EPOCH_YEAR) << 9) |
            ((unsigned)(tm->tm_mon + 1) << 5) | (unsigned)tm->tm_mday;
    *time_out = ((unsigned)tm->tm_hour << 11) | ((unsigned)tm->tm_min << 5) |
                ((unsigned)tm->tm_sec / 2);
}

unsigned char *zip_create_stored(const char *name, const void *data, size_t len,
                                 size_t *out_len)
{
    byte_buf out = {0};
    unsigned long crc = crc32_bytes(data, len);
    size_t name_len = strlen(name);
    size_t cd_offset;
    unsigned date, time_of_day;

    dos_now(&date, &time_of_day);

    /* Local file header, then the bytes themselves. */
    wr32(&out, SIG_LOCAL);
    wr16(&out, ZIP_VERSION);            /* version needed          */
    wr16(&out, 0);                      /* flags                   */
    wr16(&out, ZIP_METHOD_STORE);
    wr16(&out, time_of_day);
    wr16(&out, date);
    wr32(&out, crc);
    wr32(&out, (unsigned long)len);     /* compressed size         */
    wr32(&out, (unsigned long)len);     /* uncompressed size       */
    wr16(&out, (unsigned)name_len);
    wr16(&out, 0);                      /* extra field length      */
    buf_append(&out, name, name_len);
    buf_append(&out, data, len);

    /* Central directory: one header describing that entry. */
    cd_offset = out.len;
    wr32(&out, SIG_CENTRAL);
    wr16(&out, ZIP_VERSION);            /* version made by         */
    wr16(&out, ZIP_VERSION);            /* version needed          */
    wr16(&out, 0);
    wr16(&out, ZIP_METHOD_STORE);
    wr16(&out, time_of_day);
    wr16(&out, date);
    wr32(&out, crc);
    wr32(&out, (unsigned long)len);
    wr32(&out, (unsigned long)len);
    wr16(&out, (unsigned)name_len);
    wr16(&out, 0);                      /* extra field length      */
    wr16(&out, 0);                      /* comment length          */
    wr16(&out, 0);                      /* disk number             */
    wr16(&out, 0);                      /* internal attributes     */
    wr32(&out, 0);                      /* external attributes     */
    wr32(&out, 0);                      /* offset of local header  */
    buf_append(&out, name, name_len);

    /* End of central directory. */
    wr32(&out, SIG_EOCD);
    wr16(&out, 0);                      /* this disk               */
    wr16(&out, 0);                      /* disk with the directory */
    wr16(&out, 1);                      /* entries on this disk    */
    wr16(&out, 1);                      /* entries in total        */
    wr32(&out, (unsigned long)(out.len - cd_offset));
    wr32(&out, (unsigned long)cd_offset);
    wr16(&out, 0);                      /* comment length          */

    return (unsigned char *)buf_finish(&out, out_len);
}
