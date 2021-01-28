#define _FILE_OFFSET_BITS 64
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
#include <linux/limits.h>
#include <getopt.h>
#include <signal.h>
#include "radix-tree.h"
#include "endianness.h"

#if defined(DEBUG)
    #define DPRINTF(fmt, args...) fprintf(stderr, fmt, ##args)
#else
    #define DPRINTF(fmt, args...)
#endif

// We recognize yet-unknown compression types (u8), plus token for prealloc.
#define MAX_ENTRIES (256+1)
#define PREALLOC 256

#ifndef SZ_16M
 // old kernel headers
 #define SZ_16M 16777216
#endif

struct btrfs_sv2_args
{
    struct btrfs_ioctl_search_key key;
    uint64_t buf_size;
    uint8_t  buf[65536]; // hardcoded kernel's limit is 16MB
};

struct workspace
{
        uint64_t disk[MAX_ENTRIES];
        uint64_t uncomp[MAX_ENTRIES];
        uint64_t refd[MAX_ENTRIES];
        uint64_t disk_all;
        uint64_t uncomp_all;
        uint64_t refd_all;
        uint64_t nfiles;
        uint64_t nextents, nrefs, ninline;
        struct radix_tree_root seen_extents;
};

static const char *comp_types[MAX_ENTRIES] = { "none", "zlib", "lzo", "zstd" };

static int opt_bytes = 0;
static int opt_one_fs = 0;
static int sig_stats = 0;

static int print_stats(struct workspace *ws);

static void die(const char *txt, ...) __attribute__((format (printf, 1, 2)));
static void die(const char *txt, ...)
{
    va_list ap;
    va_start(ap, txt);
    vfprintf(stderr, txt, ap);
    va_end(ap);

    exit(1);
}

static void sigusr1(int dummy)
{
    sig_stats = 1;
}

static uint64_t get_u64(const void *mem)
{
    typedef struct __attribute__((__packed__)) { uint64_t v; } u64_unal;
    uint64_t bad_endian = ((u64_unal*)mem)->v;
    return htole64(bad_endian);
}

static uint32_t get_u32(const void *mem)
{
    typedef struct __attribute__((__packed__)) { uint32_t v; } u32_unal;
    uint32_t bad_endian = ((u32_unal*)mem)->v;
    return htole32(bad_endian);
}

static void init_sv2_args(ino_t st_ino, struct btrfs_sv2_args *sv2_args)
{
        sv2_args->key.tree_id = 0;
        sv2_args->key.max_objectid = st_ino + 1;
        sv2_args->key.min_objectid = st_ino;
        sv2_args->key.min_offset = 0;
        sv2_args->key.max_offset = -1;
        sv2_args->key.min_transid = 0;
        sv2_args->key.max_transid = -1;
        // Only search for EXTENT_DATA_KEY
        sv2_args->key.min_type = BTRFS_EXTENT_DATA_KEY;
        sv2_args->key.max_type = BTRFS_EXTENT_DATA_KEY;
        sv2_args->key.nr_items = -1;
        sv2_args->buf_size = sizeof(sv2_args->buf);
}

static inline int is_hole(uint64_t disk_bytenr)
{
    return disk_bytenr == 0;
}

static void parse_file_extent_item(uint8_t *bp, uint32_t hlen,
                                   struct workspace *ws, const char *filename)
{
    struct btrfs_file_extent_item *ei;
    uint64_t disk_num_bytes, ram_bytes, disk_bytenr, num_bytes;
    uint32_t inline_header_sz;
    unsigned  comp_type;

    DPRINTF("len=%u\n", hlen);

    ei = (struct btrfs_file_extent_item *) bp;

    ram_bytes = get_u64(&ei->ram_bytes);
    comp_type = ei->compression;

    if (ei->type == BTRFS_FILE_EXTENT_INLINE)
    {
        inline_header_sz  = sizeof(*ei);
        inline_header_sz -= sizeof(ei->disk_bytenr);
        inline_header_sz -= sizeof(ei->disk_num_bytes);
        inline_header_sz -= sizeof(ei->offset);
        inline_header_sz -= sizeof(ei->num_bytes);

        disk_num_bytes = hlen-inline_header_sz;
        DPRINTF("inline: ram_bytes=%lu compression=%u disk_num_bytes=%lu\n",
             ram_bytes, comp_type, disk_num_bytes);
        ws->disk[comp_type] += disk_num_bytes;
        ws->uncomp[comp_type] += ram_bytes;
        ws->refd[comp_type] += ram_bytes;
        ws->ninline++;
        return;
    }

    if (ei->type == BTRFS_FILE_EXTENT_PREALLOC)
        comp_type = PREALLOC;

    if (hlen != sizeof(*ei))
        die("%s: Regular extent's header not 53 bytes (%u) long?!?\n", filename, hlen);

    disk_num_bytes = get_u64(&ei->disk_num_bytes);
    disk_bytenr = get_u64(&ei->disk_bytenr);
    num_bytes = get_u64(&ei->num_bytes);

    if (is_hole(disk_bytenr))
        return;

    DPRINTF("regular: ram_bytes=%lu compression=%u disk_num_bytes=%lu disk_bytenr=%lu\n",
         ram_bytes, comp_type, disk_num_bytes, disk_bytenr);

    if (!IS_ALIGNED(disk_bytenr, 1 << 12))
        die("%s: Extent not 4K-aligned at %"PRIu64"?!?\n", filename, disk_bytenr);

    disk_bytenr >>= 12;
    radix_tree_preload(GFP_KERNEL);
    if (radix_tree_insert(&ws->seen_extents, disk_bytenr, (void *)disk_bytenr) == 0)
    {
         ws->disk[comp_type] += disk_num_bytes;
         ws->uncomp[comp_type] += ram_bytes;
         ws->nextents++;
    }
    radix_tree_preload_end();
    ws->refd[comp_type] += num_bytes;
    ws->nrefs++;
}

static void do_file(int fd, ino_t st_ino, struct workspace *ws, const char *filename)
{
    static struct btrfs_sv2_args sv2_args;
    struct btrfs_ioctl_search_header *head;
    uint32_t nr_items, hlen;
    uint8_t *bp;

    DPRINTF("inode = %" PRIu64"\n", st_ino);
    ws->nfiles++;

    init_sv2_args(st_ino, &sv2_args);

again:
    if (ioctl(fd, BTRFS_IOC_TREE_SEARCH_V2, &sv2_args))
    {
        if (errno == ENOTTY)
            die("%s: Not btrfs (or SEARCH_V2 unsupported).\n", filename);
        else
            die("%s: SEARCH_V2: %m\n", filename);
    }

    nr_items = sv2_args.key.nr_items;
    if (!nr_items)
        return;
    DPRINTF("nr_items = %u\n", nr_items);

    bp = sv2_args.buf;
    for (; nr_items > 0; nr_items--, bp += hlen)
    {
        head = (struct btrfs_ioctl_search_header*)bp;

        if (head->objectid > st_ino)
            return;

        hlen = get_u32(&head->len);
        DPRINTF("{ transid=%lu objectid=%lu offset=%lu type=%u len=%u }\n",
                get_u64(&head->transid), get_u64(&head->objectid), get_u64(&head->offset),
                get_u32(&head->type), hlen);
        bp += sizeof(*head);

        parse_file_extent_item(bp, hlen, ws, filename);
    }

    // We exhausted buffer space, continue.
    sv2_args.key.nr_items = -1;
    sv2_args.key.min_offset = get_u64(&head->offset) + 1;
    goto again;
}

static void do_recursive_search(const char *path, struct workspace *ws, const dev_t *dev)
{
        int fd;
        int path_size;
        char *fn;
        DIR *dir;
        struct dirent *de;
        struct stat st;

        if (sig_stats)
        {
            sig_stats = 0;
            print_stats(ws);
        }

        fd = open(path, O_RDONLY|O_NOFOLLOW|O_NOCTTY|O_NONBLOCK);
        if (fd == -1)
        {
            if (errno == ELOOP    // symlink
             || errno == ENXIO    // some device nodes
             || errno == ENODEV   // /dev/ptmx
             || errno == ENOMEDIUM// more device nodes
             || errno == ENOENT)  // something just deleted
                return; // ignore, silently
            else if (errno == EACCES)
            {
                fprintf(stderr, "%s: %m\n", path);
                return; // warn
            }
            else
                die("open(\"%s\"): %m\n", path);
        }

        DPRINTF("%s\n", path);

        if (fstat(fd, &st))
            die("stat(\"%s\"): %m\n", path);

        if (opt_one_fs && dev != NULL && *dev != st.st_dev)
            return;

        if (S_ISDIR(st.st_mode))
        {
            dir = fdopendir(fd);
            if (!dir)
                die("opendir(\"%s\"): %m\n", path);
            path_size = 2; // slash + \0;
            path_size += strlen(path) + NAME_MAX;
            fn = (char *) malloc(path_size);
            if (!fn)
                die("Out of memory.\n");
            while(1)
            {
                    de = readdir(dir);
                    if (!de)
                        break;
                    if (de->d_type != DT_DIR
                     && de->d_type != DT_REG
                     && de->d_type != DT_UNKNOWN)
                    {
                        continue;
                    }
                    if (!strcmp(de->d_name, "."))
                        continue;
                    if (!strcmp(de->d_name, ".."))
                        continue;
                    const char *slash = strrchr(path, '/');
                    snprintf(fn, path_size, (slash && !slash[1]) ? "%s%s"
                        : "%s/%s", path, de->d_name);
                    do_recursive_search(fn, ws, &st.st_dev);
            }
            free(fn);
            closedir(dir);
        }

        if (S_ISREG(st.st_mode))
            do_file(fd, st.st_ino, ws, path);

        close(fd);
}

#define HB 24 /* size of buffers */
static void human_bytes(uint64_t x, char *output)
{
    static const char *units = "BKMGTPE";
    int u = 0;

    if (opt_bytes)
        return (void)snprintf(output, HB, "%"PRIu64, x);

    while (x >= 10240)
        u++, x>>=10;
    if (x >= 1024)
        snprintf(output, HB, " %"PRIu64".%"PRIu64"%c", x>>10, x*10/1024%10, units[u+1]);
    else
        snprintf(output, HB, "%4"PRIu64"%c", x, units[u]);
}

static void print_table(const char *type,
                        const char *percentage,
                        const char *disk_usage,
                        const char *uncomp_usage,
                        const char *refd_usage)
{
        printf("%-10s %-8s %-12s %-12s %-12s\n", type, percentage,
               disk_usage, uncomp_usage, refd_usage);
}

static void print_help(void)
{
        fprintf(stderr,
		"Usage: compsize [options] file-or-dir1 [file-or-dir2 ...]\n"
		"\n"
		"Compsize displays total space used by set of files, taking into account\n"
		"compression, reflinks, partially overwritten extents.\n"
		"\n"
		"Options:\n"
		"    -h, --help              print this help message and exit\n"
		"    -b, --bytes             display raw bytes instead of human-readable sizes\n"
		"    -x, --one-file-system   don't cross filesystem boundaries\n"
		"\n"
	);
}

static void parse_options(int argc, char **argv)
{
    static const char *short_options = "bxh";
    static struct option long_options[] =
    {
        {"bytes",                  0, 0, 'b'},
        {"one-file-system",        0, 0, 'x'},
        {"help",                   0, 0, 'h'},
        {0},
    };

    while (1)
    {
        switch (getopt_long(argc, argv, short_options, long_options, 0))
        {
        case 'b':
            opt_bytes = 1;
            break;
        case 'x':
            opt_one_fs = 1;
            break;
        case 'h':
            print_help();
            exit(0);
            break; // unreachable
        case -1:
            return;
        default:
            exit(1);
        }
    }
}

static int print_stats(struct workspace *ws)
{
    char perc[8], disk_usage[HB], uncomp_usage[HB], refd_usage[HB];
    uint32_t percentage;
    int t;

    ws->uncomp_all = ws->disk_all = ws->refd_all = 0;
    for (t=0; t<MAX_ENTRIES; t++)
    {
            ws->uncomp_all += ws->uncomp[t];
            ws->disk_all   += ws->disk[t];
            ws->refd_all   += ws->refd[t];
    }

    if (!ws->uncomp_all)
    {
        if (!ws->nfiles)
            fprintf(stderr, "No files.\n");
        else
            fprintf(stderr, "All empty or still-delalloced files.\n");
        return 1;
    }

    if (ws->nfiles > 1)
    {
        printf("Processed %"PRIu64" files, %"PRIu64" regular extents "
               "(%"PRIu64" refs), %"PRIu64" inline.\n",
               ws->nfiles, ws->nextents, ws->nrefs, ws->ninline);
    }

    print_table("Type", "Perc", "Disk Usage", "Uncompressed", "Referenced");
    percentage = ws->disk_all*100/ws->uncomp_all;
    snprintf(perc, sizeof(perc), "%3u%%", percentage);
    human_bytes(ws->disk_all, disk_usage);
    human_bytes(ws->uncomp_all, uncomp_usage);
    human_bytes(ws->refd_all, refd_usage);
    print_table("TOTAL", perc, disk_usage, uncomp_usage, refd_usage);

    for (t=0; t<MAX_ENTRIES; t++)
    {
        if (!ws->uncomp[t])
            continue;
        const char *ct = t==PREALLOC? "prealloc" : comp_types[t];
        char unkn_comp[12];
        percentage = ws->disk[t]*100/ws->uncomp[t];
        snprintf(perc, sizeof(perc), "%3u%%", percentage);
        human_bytes(ws->disk[t], disk_usage);
        human_bytes(ws->uncomp[t], uncomp_usage);
        human_bytes(ws->refd[t], refd_usage);
        if (!ct)
        {
            snprintf(unkn_comp, sizeof(unkn_comp), "?%u", t);
            ct = unkn_comp;
        }
        print_table(ct, perc, disk_usage, uncomp_usage, refd_usage);
    }

    return 0;
}

int main(int argc, char **argv)
{
    struct workspace *ws;

    ws = (struct workspace *) calloc(sizeof(*ws), 1);

    parse_options(argc, argv);

    if (optind >= argc)
    {
        print_help();
        return 1;
    }

    radix_tree_init();
    INIT_RADIX_TREE(&ws->seen_extents, 0);
    signal(SIGUSR1, sigusr1);

    for (; argv[optind]; optind++)
        do_recursive_search(argv[optind], ws, NULL);

    int ret = print_stats(ws);

    free(ws);

    return ret;
}
