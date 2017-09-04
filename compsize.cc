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
#include <stdio.h>
#include <inttypes.h>
#include <set>
#include "endianness.h"

#define DPRINTF(...) do;while(0)
#define MAX_ENTRIES 256

static void die(const char *txt, ...) __attribute__((format (printf, 1, 2)));
static void die(const char *txt, ...)
{
    va_list ap;
    va_start(ap, txt);
    vfprintf(stderr, txt, ap);
    va_end(ap);

    exit(1);
}

static struct btrfs_ioctl_ino_lookup_args ino_args;
static struct
{
    struct btrfs_ioctl_search_key key;
    uint64_t buf_size;
    uint8_t  buf[SZ_16M]; // hardcoded kernel's limit
} sv2_args;

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

static std::set<uint64_t> seen_extents;
static uint64_t disk[MAX_ENTRIES], total[MAX_ENTRIES], disk_all, total_all, nfiles;
static const char *comp_types[MAX_ENTRIES] = { "none", "zlib", "lzo", "zstd" };

static void do_file(const char *filename)
{
    int fd = open(filename, O_RDONLY|O_NOFOLLOW|O_NOCTTY);
    if (fd == -1 && errno == ELOOP)
        return;
    if (fd == -1)
        die("open(\"%s\"): %m\n", filename);
    DPRINTF("%s\n", filename);
    struct stat st;
    if (fstat(fd, &st))
        die("stat(\"%s\"): %m\n", filename);

    if ((st.st_mode & S_IFMT) == S_IFDIR)
    {
        DIR *dir = fdopendir(fd);
        if (!dir)
            die("opendir(\"%s\"): %m\n", filename);
        while (struct dirent *de = readdir(dir))
        {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char *fn;
            if (asprintf(&fn, "%s/%s", filename, de->d_name) == -1)
                die("Out of memory.\n");
            do_file(fn);
            free(fn);
        }
        closedir(dir);
    }

    if ((st.st_mode & S_IFMT) != S_IFREG)
    {
        close(fd);
        return;
    }
    DPRINTF("inode = %" PRIu64"\n", st.st_ino);
    nfiles++;

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
        DPRINTF("{ transid=%llu objectid=%llu offset=%llu type=%u len=%u }\n",
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
        if (head->type == BTRFS_EXTENT_DATA_KEY)
        {
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
                if (!seen_extents.count(disk_bytenr))
                {
                    // count every extent only once
                    seen_extents.insert(disk_bytenr);
                    disk[compression] += len;
                    total[compression] += ram_bytes;
                    disk_all += len;
                    total_all += ram_bytes;
                }
            }
            else
            {
                uint64_t len = hlen-21;
                DPRINTF("inline: ram_bytes=%lu compression=%u len=%u\n",
                         ram_bytes, compression, len);
                disk[compression] += len;
                total[compression] += ram_bytes;
                disk_all += len;
                total_all += ram_bytes;
            }
        }
        bp += hlen;
    }

    close(fd);
}

static void human_bytes(uint64_t x, char *output)
{
    static const char *units = "BKMGTPE";
    int u = 0;
    while (x >= 10240)
        u++, x>>=10;
    if (x >= 1024)
        snprintf(output, 16, " %lu.%lu%c", x>>10, x*10/1024%10, units[u+1]);
    else
        snprintf(output, 16, "%4lu%c", x, units[u]);
}

static void print_table(const char *type, const char *percentage, const char *disk_usage, const char *total_usage)
{
        printf("%-10s %-8s %-12s %-12s\n", type, percentage, disk_usage, total_usage);
}

int main(int argc, const char **argv)
{
    char perc[8], disk_usage[12], total_usage[12];
    uint32_t percentage;

    for (int i=0; i<MAX_ENTRIES; i++)
        disk[i]=0, total[i]=0;
    disk_all = total_all = nfiles = 0;

    if (argc <= 1)
    {
        fprintf(stderr, "Usage: compsize file-or-dir1 [file-or-dir2 ...]\n");
        return 1;
    }

    for (; argv[1]; argv++)
        do_file(argv[1]);

    if (!total_all)
    {
        fprintf(stderr, "No files.\n");
        return 1;
    }

    if (nfiles > 1)
        printf("Processed %lu files.\n", nfiles);

    print_table("Type", "Perc", "Disk Usage", "Total Usage");
    percentage = disk_all*100/total_all;
    snprintf(perc, 16, "%3u%%", percentage);
    human_bytes(disk_all, disk_usage);
    human_bytes(total_all, total_usage);
    print_table("Data", perc, disk_usage, total_usage);

    for (int t=0; t<MAX_ENTRIES; t++)
    {
        if (!total[t])
            continue;
        const char *ct = comp_types[t];
        percentage = disk[t]*100/total[t];
        snprintf(perc, 8, "%3u%%", percentage);
        human_bytes(disk[t], disk_usage);
        human_bytes(total[t], total_usage);
        print_table(ct?ct:"?????", perc, disk_usage, total_usage);
    }

    return 0;
}
