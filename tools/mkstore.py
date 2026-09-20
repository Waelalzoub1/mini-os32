#!/usr/bin/env python3
"""Format a partition as a mini-os32 persistent store.

    tools/mkstore.py <device-or-file> [fs.bin]

Writes a 512-byte header followed by the filesystem image.  The kernel looks
for that header when it scans the GPT: a partition without it is never written
to, so formatting is the one deliberate step that arms persistence.

This OVERWRITES the target.  Point it at a partition (/dev/nvme0n1p4), never
at a whole disk -- writing a store header over LBA 0 of a disk destroys its
partition table.  The script refuses a target that looks like a whole disk.
"""
import os
import re
import struct
import sys

SECTOR = 512
MAGIC0 = 0x494E494D          # "MINI"
MAGIC1 = 0x3233534F          # "OS32"
VERSION = 1
HEADER_SECTORS = 1
# The store has to cover the whole RAM disk, not just the bytes fs.bin happens
# to occupy: the kernel writes new files past the end of the initial image, and
# a sector with no home on disk is silently dropped.  Matches RAMDISK_BYTES in
# boot/uefi.c.
RAMDISK_SECTORS = 8 * 1024 * 1024 // SECTOR


def build_header(fs_sectors):
    h = bytearray(SECTOR)
    struct.pack_into('<IIII', h, 0, MAGIC0, MAGIC1, VERSION, fs_sectors)
    # Readable in a hex dump, and harmless to the kernel, which only reads the
    # four fields above.
    h[64:64 + 32] = b'mini-os32 persistent store\n\0\0\0\0\0\0'[:32]
    return bytes(h)


def looks_like_whole_disk(path):
    """nvme0n1 and sda are disks; nvme0n1p4 and sda1 are partitions."""
    name = os.path.basename(path)
    return bool(re.fullmatch(r'nvme\d+n\d+|mmcblk\d+|[svh]d[a-z]+', name))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    target = sys.argv[1]
    fs_path = sys.argv[2] if len(sys.argv) > 2 else 'build/fs.bin'

    if not os.path.exists(fs_path):
        raise SystemExit("%s not found -- run ./build.sh first" % fs_path)
    if looks_like_whole_disk(target):
        raise SystemExit("%s looks like a whole disk, not a partition -- refusing"
                         % target)

    fs = open(fs_path, 'rb').read()
    fs += b'\0' * (-len(fs) % SECTOR)
    if len(fs) > RAMDISK_SECTORS * SECTOR:
        raise SystemExit("filesystem image is larger than the RAM disk")
    content_sectors = len(fs) // SECTOR
    fs += b'\0' * (RAMDISK_SECTORS * SECTOR - len(fs))

    blob = build_header(RAMDISK_SECTORS) + fs

    if os.path.exists(target) and not os.path.isfile(target):
        # Block device: check it is big enough before touching anything.
        fd = os.open(target, os.O_RDONLY)
        try:
            size = os.lseek(fd, 0, os.SEEK_END)
        finally:
            os.close(fd)
        if size < len(blob):
            raise SystemExit("target is %d bytes, need %d" % (size, len(blob)))
        print("target %s: %d bytes" % (target, size))

    with open(target, 'r+b' if os.path.exists(target) else 'wb') as f:
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())

    print("wrote store header + %d sectors (%d MB) to %s; %d KB of it is filesystem"
          % (RAMDISK_SECTORS, RAMDISK_SECTORS // 2048, target, content_sectors // 2))


if __name__ == '__main__':
    main()
