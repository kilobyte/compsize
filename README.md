# compsize

compsize takes a list of files (given as arguments) on a btrfs filesystem
and measures used compression types and effective compression ratio,
producing a report such as:

```
[~]$ compsize /usr/share
90320 files.
all   79%  1.4G/ 1.8G
none 100%  1.0G/ 1.0G
lzo   53%  446M/ 833M
```

A directory has no extents but has a (recursive) list of files.  A
non-regular file is silently ignored.

As it makes no sense to talk about compression ratio of a partial extent,
every referenced extent is counted whole, exactly once -- no matter if you
use only a few bytes of a 1GB extent or reflink it a thousand times.  Thus,
the apparent size will not match the number given by **tar** or **du**.  On
the other hand, the space _used_ should be accurate (although obviously it
can be shared with files outside our set).
