// For asprintf()
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <btrfs/ioctl.h>
#include <btrfs/ctree.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include "endianness.h"
#include "radix-tree.h"

#if defined(DEBUG)
    #define DPRINTF(fmt, args...) fprintf(stderr, fmt, ##args)
#else
    #define DPRINTF(fmt, args...)
#endif

#define MAX_ENTRIES 256

struct btrfs_sv2_args
{
    struct btrfs_ioctl_search_key key;
    uint64_t buf_size;
    uint8_t  buf[SZ_16M]; // hardcoded kernel's limit
};

struct workspace
{
        uint64_t disk[MAX_ENTRIES];
        uint64_t total[MAX_ENTRIES];
        uint64_t disk_all;
        uint64_t total_all;
        uint64_t nfiles;
        struct radix_tree_root seen_extents;
};

static const char *comp_types[MAX_ENTRIES] = { "none", "zlib", "lzo", "zstd" };

static void die(const char *txt, ...) __attribute__((format (printf, 1, 2)));
static void die(const char *txt, ...)
{
    va_list ap;
    va_start(ap, txt);
    vfprintf(stderr, txt, ap);
    va_end(ap);

    exit(1);
}

static uint64_t get_u64(const void *mem)
{
    typedef struct __attribute__((__packed__)) { uint64_t v; } u64_unal;
    uint64_t bad_endian = ((u64_unal*)mem)->v;
    return htole64(bad_endian);
}

static uint64_t get_u32(const void *mem)
{
    typedef struct __attribute__((__packed__)) { uint32_t v; } u32_unal;
    uint32_t bad_endian = ((u32_unal*)mem)->v;
    return htole32(bad_endian);
}

static void do_file(int fd, struct stat st, struct workspace *ws)
{
    static struct btrfs_ioctl_ino_lookup_args ino_args;
    static struct btrfs_sv2_args sv2_args;

    DPRINTF("inode = %" PRIu64"\n", st.st_ino);
    ws->nfiles++;

    ino_args.treeid   = 0;
    ino_args.objectid = BTRFS_FIRST_FREE_OBJECTID;
    if (ioctl(fd, BTRFS_IOC_INO_LOOKUP, &ino_args))
        die("INO_LOOKUP: %m\n");
    DPRINTF("tree = %llu\n", ino_args.treeid);

    memset(&sv2_args.key, 0, sizeof(sv2_args.key));
    sv2_args.key.tree_id = ino_args.treeid;
    sv2_args.key.min_objectid = sv2_args.key.max_objectid = st.st_ino;
    sv2_args.key.min_offset = sv2_args.key.min_transid = 0;
    sv2_args.key.max_offset = sv2_args.key.max_transid = -1;
    sv2_args.key.min_type = 0;
    sv2_args.key.max_type = -1;
    sv2_args.key.nr_items = -1;
    sv2_args.buf_size = sizeof(sv2_args.buf);

    if (ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, &sv2_args))
        die("SEARCH_V2: %m\n");
    DPRINTF("nr_items = %u\n", sv2_args.key.nr_items);

    uint8_t *bp = sv2_args.buf;
    while (sv2_args.key.nr_items--)
    {
        struct btrfs_ioctl_search_header *head = (struct btrfs_ioctl_search_header*)bp;
        uint32_t hlen = get_u32(&head->len);
        DPRINTF("{ transid=%lu objectid=%lu offset=%lu type=%u len=%u }\n",
                get_u32(&head->transid), get_u32(&head->objectid), get_u32(&head->offset),
                head->type, hlen);
        bp += sizeof(struct btrfs_ioctl_search_header);
/*
        printf("\e[0;30;1m");
        for (uint32_t i = 0; i < hlen; i++)
        {
            printf("%02x", bp[i]);
            if (i%8==7)
                printf(" ");
        }
        printf("\e[0m\n");
*/
        if (head->type != BTRFS_EXTENT_DATA_KEY) {
            bp += hlen;
            continue;
        }

        DPRINTF("len=%u\n", hlen);
        /*
            u64 generation
            u64 ram_bytes
            u8  compression
            u8  encryption
            u16 unused
            u8  type
        */
        uint64_t ram_bytes = get_u64(bp+8);
        uint8_t compression = bp[16];
        uint8_t type = bp[20];
        if (type)
        {
                /*
                ...
                u64 disk_bytenr
                u64 disk_num_bytes
                u64 offset
                u64 num_bytes
            */
            uint64_t len = get_u64(bp+29);
            uint64_t disk_bytenr = get_u64(bp+21);
            DPRINTF("regular: ram_bytes=%lu compression=%u len=%lu disk_bytenr=%lu\n",
                     ram_bytes, compression, len, disk_bytenr);
            radix_tree_preload(GFP_KERNEL);
            if (radix_tree_insert(&ws->seen_extents, disk_bytenr, (void *)disk_bytenr) == 0)
            {
                ws->disk[compression] += len;
                ws->total[compression] += ram_bytes;
                ws->disk_all += len;
                ws->total_all += ram_bytes;
            }
            radix_tree_preload_end();
        }
        else
        {
            uint64_t len = hlen-21;
            DPRINTF("inline: ram_bytes=%lu compression=%u len=%lu\n",
                 ram_bytes, compression, len);
            ws->disk[compression] += len;
            ws->total[compression] += ram_bytes;
            ws->disk_all += len;
            ws->total_all += ram_bytes;
        }
        bp += hlen;
    }
}

static void do_recursive_search(const char *path, struct workspace *ws)
{
        int fd;
        DIR *dir;
        struct stat st;

        fd = open(path, O_RDONLY|O_NOFOLLOW|O_NOCTTY);
        if (fd == -1)
        {
            if (errno == ELOOP)
                return;
            else
                die("open(\"%s\"): %m\n", path);
        }

        DPRINTF("%s\n", path);

        if (fstat(fd, &st))
            die("stat(\"%s\"): %m\n", path);

        if ((st.st_mode & S_IFMT) == S_IFDIR)
        {
            struct dirent *de;
            dir = fdopendir(fd);
            if (!dir)
                die("opendir(\"%s\"): %m\n", path);
            while ((de = readdir(dir)))
            {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                    continue;
                char *fn;
                if (asprintf(&fn, "%s/%s", path, de->d_name) == -1)
                    die("Out of memory.\n");
                do_recursive_search(fn, ws);
                free(fn);
            }
            closedir(dir);
        }

        if ((st.st_mode & S_IFMT) != S_IFREG)
        {
            close(fd);
            return;
        }

        do_file(fd, st, ws);

        close(fd);
}

static void human_bytes(uint64_t x, char *output)
{
    static const char *units = "BKMGTPE";
    int u = 0;
    while (x >= 10240)
        u++, x>>=10;
    if (x >= 1024)
        snprintf(output, 12, " %lu.%lu%c", x>>10, x*10/1024%10, units[u+1]);
    else
        snprintf(output, 12, "%4lu%c", x, units[u]);
}

static void print_table(const char *type, const char *percentage, const char *disk_usage, const char *total_usage)
{
        printf("%-10s %-8s %-12s %-12s\n", type, percentage, disk_usage, total_usage);
}

int main(int argc, const char **argv)
{
    char perc[8], disk_usage[12], total_usage[12];
    struct workspace ws;
    uint32_t percentage;

    if (argc <= 1)
    {
        fprintf(stderr, "Usage: compsize file-or-dir1 [file-or-dir2 ...]\n");
        return 1;
    }

    memset(&ws, 0, sizeof(ws));

    INIT_RADIX_TREE(&ws.seen_extents, 0);

    for (; argv[1]; argv++)
        do_recursive_search(argv[1], &ws);

    if (!ws.total_all)
    {
        fprintf(stderr, "No files.\n");
        return 1;
    }

    if (ws.nfiles > 1)
        printf("Processed %lu files.\n", ws.nfiles);

    print_table("Type", "Perc", "Disk Usage", "Total Usage");
    percentage = ws.disk_all*100/ws.total_all;
    snprintf(perc, 16, "%3u%%", percentage);
    human_bytes(ws.disk_all, disk_usage);
    human_bytes(ws.total_all, total_usage);
    print_table("Data", perc, disk_usage, total_usage);

    for (int t=0; t<MAX_ENTRIES; t++)
    {
        if (!ws.total[t])
            continue;
        const char *ct = comp_types[t];
        percentage = ws.disk[t]*100/ws.total[t];
        snprintf(perc, 8, "%3u%%", percentage);
        human_bytes(ws.disk[t], disk_usage);
        human_bytes(ws.total[t], total_usage);
        print_table(ct?ct:"?????", perc, disk_usage, total_usage);
    }

    return 0;
}
