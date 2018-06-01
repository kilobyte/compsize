# compsize

compsize takes a list of files (given as arguments) on a btrfs filesystem
and measures used compression types and effective compression ratio,
producing a report such as:

```
[~]$ compsize /home
Processed 140058 files, 133128 regular extents (196786 refs), 80886 inline.
Type       Perc     Disk Usage   Uncompressed Referenced
TOTAL       93%       14G          15G          12G
none       100%       13G          13G          10G
zlib        41%      628M         1.4G         1.4G
zstd        28%       42M         148M         148M
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
 * Type: compression algorithm used
 * Perc: disk usage/uncompressed -- ie, effective compression ratio
 * Disk Usage: blocks actually used on the disk
 * Uncompressed: extents before compression
 * Referenced: apparent size of files (minus holes)

The ioctl used requires root.

# Installation:

Besides regular C toolchain, you need btrfs userspace headers.  On Debian
(incl. derivatives like Ubuntu) they're in libbtrfs-dev, SuSE ships them
inside libbtrfs-devel, they used to come with btrfs-progs before.
Required kernel: 3.16, btrfs-progs: 3.18 (untested!).
