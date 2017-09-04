# compsize

compsize takes a list of files (given as arguments) on a btrfs filesystem
and measures used compression types and effective compression ratio,
producing a report such as:

```
[~]$ compsize /usr/share
Processed 120101 files.
Type       Perc     Disk Usage   Uncompressed
Data        58%      1.1G         1.9G
none       100%      351M         351M
zlib        29%       41M         137M
lzo         51%      776M         1.4G
```

A directory has no extents but has a (recursive) list of files.  A
non-regular file is silently ignored.

As it makes no sense to talk about compression ratio of a partial extent,
every referenced extent is counted whole, exactly once -- no matter if you
use only a few bytes of a 1GB extent or reflink it a thousand times.  Thus,
the uncompressed size will not match the number given by **tar** or **du**.
On the other hand, the space _used_ should be accurate (although obviously
it can be shared with files outside our set).

The fields are:
 * Perc: disk usage/uncompressed -- ie, effective compression ratio
 * Disk Usage: blocks actually used on the disk
 * Uncompressed: extents before compression
 * Referenced: apparent size of files (minus holes)

The ioctl used requires root.
