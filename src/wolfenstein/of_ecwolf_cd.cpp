#define _GNU_SOURCE

#if defined(OF_ECWOLF_OPENFPGA) && !defined(OF_PC)

#include "of_ecwolf_cd.h"

#include "of_file.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
static const uint32_t BIN_SLOT = 15;
static const uint32_t ISO_SECTOR_SIZE = 2048;
static const uint32_t MAX_CD_FILES = 512;
static const uint32_t MAX_CD_NAME = 128;

struct CdEntry
{
    char name[MAX_CD_NAME];
    char path[MAX_CD_NAME * 2];
    uint32_t lba;
    uint32_t size;
};

struct CdCookie
{
    CdEntry entry;
    uint32_t pos;
};

static CdEntry cd_entries[MAX_CD_FILES];
static uint32_t cd_entry_count;
static bool cd_init_attempted;
static bool cd_ready;
static uint32_t cd_sector_size;
static uint32_t cd_data_offset;

static volatile int async_done;
static volatile int async_result;

static void cd_read_done(int, int result)
{
    async_result = result;
    async_done = 1;
}

static int read_slot_raw(uint32_t offset, void *dest, uint32_t len)
{
    uint8_t *out = static_cast<uint8_t *>(dest);
    uint32_t max_read = of_file_async_max_read();
    if (max_read == 0 || max_read > 32768)
        max_read = 32768;

    while (len > 0)
    {
        uint32_t chunk = len < max_read ? len : max_read;
        async_done = 0;
        async_result = -1;

        int token = of_file_read_async(BIN_SLOT, offset, out, chunk, cd_read_done);
        if (token < 0)
            return token;

        while (!async_done)
        {
            of_file_async_poll();
            if (!of_file_async_busy())
                break;
            usleep(1000);
        }
        if (!async_done || async_result < 0)
            return async_done ? async_result : -1;

        offset += chunk;
        out += chunk;
        len -= chunk;
    }
    return 0;
}

static int read_iso_payload(uint32_t logical_offset, void *dest, uint32_t len)
{
    uint8_t *out = static_cast<uint8_t *>(dest);

    while (len > 0)
    {
        uint32_t sector = logical_offset / ISO_SECTOR_SIZE;
        uint32_t in_sector = logical_offset % ISO_SECTOR_SIZE;
        uint32_t chunk = ISO_SECTOR_SIZE - in_sector;
        if (chunk > len)
            chunk = len;

        uint32_t raw_offset = sector * cd_sector_size + cd_data_offset + in_sector;
        int rc = read_slot_raw(raw_offset, out, chunk);
        if (rc < 0)
            return rc;

        logical_offset += chunk;
        out += chunk;
        len -= chunk;
    }
    return 0;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void clean_iso_name(char *out, size_t out_size, const uint8_t *name, uint8_t name_len)
{
    size_t n = 0;
    for (uint8_t i = 0; i < name_len && n + 1 < out_size; i++)
    {
        char ch = static_cast<char>(name[i]);
        if (ch == ';')
            break;
        out[n++] = ch;
    }
    while (n > 0 && out[n - 1] == '.')
        n--;
    out[n] = 0;
}

static const char *path_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static bool is_cd_root(const char *path)
{
    return strcmp(path, "/cd") == 0 || strcmp(path, "/cd/") == 0;
}

static bool is_cd_path(const char *path)
{
    return is_cd_root(path) || strncmp(path, "/cd/", 4) == 0;
}

static int find_entry(const char *path)
{
    const char *base = path_basename(path);
    for (uint32_t i = 0; i < cd_entry_count; i++)
    {
        if (strcasecmp(base, cd_entries[i].name) == 0 ||
            strcasecmp(path, cd_entries[i].path) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

static void copy_cstr(char *dest, size_t dest_size, const char *src)
{
    if (dest_size == 0)
        return;

    size_t i = 0;
    while (i + 1 < dest_size && src[i] != 0)
    {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
}

static void add_entry(const char *name, const char *path, uint32_t lba, uint32_t size)
{
    if (name[0] == 0 || cd_entry_count >= MAX_CD_FILES || find_entry(name) >= 0)
        return;

    CdEntry &entry = cd_entries[cd_entry_count++];
    copy_cstr(entry.name, sizeof(entry.name), name);
    copy_cstr(entry.path, sizeof(entry.path), path);
    entry.lba = lba;
    entry.size = size;
}

static void parse_directory(uint32_t lba, uint32_t size, const char *parent, int depth)
{
    if (depth > 8 || size == 0 || size > 256 * 1024)
        return;

    uint8_t *dir = static_cast<uint8_t *>(malloc(size));
    if (!dir)
        return;
    if (read_iso_payload(lba * ISO_SECTOR_SIZE, dir, size) < 0)
    {
        free(dir);
        return;
    }

    uint32_t off = 0;
    while (off < size)
    {
        uint8_t rec_len = dir[off];
        if (rec_len == 0)
        {
            off = (off + ISO_SECTOR_SIZE) & ~(ISO_SECTOR_SIZE - 1);
            continue;
        }
        if (rec_len < 34 || off + rec_len > size)
            break;

        const uint8_t *rec = dir + off;
        uint32_t child_lba = rd32le(rec + 2);
        uint32_t child_size = rd32le(rec + 10);
        bool is_dir = (rec[25] & 0x02) != 0;
        uint8_t name_len = rec[32];
        const uint8_t *raw_name = rec + 33;

        if (!(name_len == 1 && (raw_name[0] == 0 || raw_name[0] == 1)))
        {
            char name[MAX_CD_NAME];
            char full[MAX_CD_NAME * 2];
            clean_iso_name(name, sizeof(name), raw_name, name_len);
            if (parent[0])
                snprintf(full, sizeof(full), "%s/%s", parent, name);
            else
                snprintf(full, sizeof(full), "%s", name);

            if (is_dir)
                parse_directory(child_lba, child_size, full, depth + 1);
            else
                add_entry(name, full, child_lba, child_size);
        }

        off += rec_len;
    }

    free(dir);
}

static bool try_pvd(uint32_t sector_size, uint32_t data_offset, uint8_t *pvd)
{
    uint32_t offset = 16u * sector_size + data_offset;
    if (read_slot_raw(offset, pvd, ISO_SECTOR_SIZE) < 0)
        return false;
    return pvd[0] == 1 && memcmp(pvd + 1, "CD001", 5) == 0;
}

static bool init_cd()
{
    if (cd_init_attempted)
        return cd_ready;
    cd_init_attempted = true;

    uint8_t *pvd = static_cast<uint8_t *>(malloc(ISO_SECTOR_SIZE));
    if (!pvd)
        return false;

    if (try_pvd(ISO_SECTOR_SIZE, 0, pvd))
    {
        cd_sector_size = ISO_SECTOR_SIZE;
        cd_data_offset = 0;
    }
    else if (try_pvd(2352, 16, pvd))
    {
        cd_sector_size = 2352;
        cd_data_offset = 16;
    }
    else
    {
        printf("CD: slot 15 is not a supported ISO/BIN image.\n");
        free(pvd);
        return false;
    }

    const uint8_t *root = pvd + 156;
    uint32_t root_lba = rd32le(root + 2);
    uint32_t root_size = rd32le(root + 10);
    parse_directory(root_lba, root_size, "", 0);

    cd_ready = cd_entry_count > 0;
    printf("CD: indexed %lu file%s from slot 15 (%lu-byte sectors).\n",
           static_cast<unsigned long>(cd_entry_count),
           cd_entry_count == 1 ? "" : "s",
           static_cast<unsigned long>(cd_sector_size));

    free(pvd);
    return cd_ready;
}

static ssize_t cd_cookie_read(void *cookie_ptr, char *buf, size_t size)
{
    CdCookie *cookie = static_cast<CdCookie *>(cookie_ptr);
    if (cookie->pos >= cookie->entry.size)
        return 0;

    uint32_t avail = cookie->entry.size - cookie->pos;
    uint32_t count = size < avail ? static_cast<uint32_t>(size) : avail;
    if (read_iso_payload(cookie->entry.lba * ISO_SECTOR_SIZE + cookie->pos, buf, count) < 0)
        return -1;

    cookie->pos += count;
    return count;
}

static int cd_cookie_seek(void *cookie_ptr, off_t *offset, int whence)
{
    CdCookie *cookie = static_cast<CdCookie *>(cookie_ptr);
    int64_t pos = *offset;

    if (whence == SEEK_CUR)
        pos += cookie->pos;
    else if (whence == SEEK_END)
        pos += cookie->entry.size;
    else if (whence != SEEK_SET)
        return -1;

    if (pos < 0)
        pos = 0;
    if (pos > static_cast<int64_t>(cookie->entry.size))
        pos = cookie->entry.size;

    cookie->pos = static_cast<uint32_t>(pos);
    *offset = cookie->pos;
    return 0;
}

static int cd_cookie_close(void *cookie_ptr)
{
    free(cookie_ptr);
    return 0;
}
}

bool OF_ECWolfCD_GetInfo(const char *path, bool *is_dir)
{
    if (!is_cd_path(path))
        return false;
    if (!init_cd())
        return false;
    if (is_cd_root(path))
    {
        if (is_dir)
            *is_dir = true;
        return true;
    }
    if (find_entry(path) >= 0)
    {
        if (is_dir)
            *is_dir = false;
        return true;
    }
    return false;
}

void OF_ECWolfCD_List(const char *path, TArray<FString> &files)
{
    if (!is_cd_root(path) || !init_cd())
        return;
    for (uint32_t i = 0; i < cd_entry_count; i++)
        files.Push(cd_entries[i].name);
}

FILE *OF_ECWolfCD_Open(const char *path, const char *mode)
{
    if (!mode || mode[0] != 'r' || !is_cd_path(path) || !init_cd())
        return NULL;

    int index = find_entry(path);
    if (index < 0)
        return NULL;

    CdCookie *cookie = static_cast<CdCookie *>(calloc(1, sizeof(CdCookie)));
    if (!cookie)
        return NULL;
    cookie->entry = cd_entries[index];

    cookie_io_functions_t funcs;
    memset(&funcs, 0, sizeof(funcs));
    funcs.read = cd_cookie_read;
    funcs.seek = cd_cookie_seek;
    funcs.close = cd_cookie_close;
    return fopencookie(cookie, mode, funcs);
}

#endif
