#!/usr/bin/env python3
"""Build a bootable UEFI image: GPT + a FAT32 ESP + a mini-os32 store.

QEMU's `fat:rw:<dir>` driver corrupts trees of any size -- it fails to serve
even a known-good binary once a few hundred KB are present -- so testing the
UEFI path needs a genuine filesystem.  The same image is what gets written to a
USB stick, so what we test is what boots.

    tools/mkesp.py <src-dir> <out.img> [size-mb] [store-mb]

Every name must fit 8.3; no long-filename entries are generated.
"""
import os
import struct
import sys
import zlib


def guid(s):
    """GUID text -> the 16 on-disk bytes (first three fields little-endian)."""
    a, b, c, d, e = s.split('-')
    return (bytes.fromhex(a)[::-1] + bytes.fromhex(b)[::-1] +
            bytes.fromhex(c)[::-1] + bytes.fromhex(d) + bytes.fromhex(e))

SECTOR = 512
RESERVED = 32          # boot sector, FSInfo, backup boot at 6
NUM_FATS = 2
SEC_PER_CLUS = 1
PART_START = 2048      # 1MB in, the conventional alignment


class Fat32:
    def __init__(self, total_sectors, part_start):
        self.part_start = part_start
        self.total = total_sectors
        # FAT size and cluster count are mutually dependent; iterate to a fixed point
        clusters = total_sectors - RESERVED
        for _ in range(8):
            fat_sectors = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            clusters = total_sectors - RESERVED - NUM_FATS * fat_sectors
        self.fat_sectors = fat_sectors
        self.clusters = clusters
        if clusters < 65525:
            raise SystemExit("too small to be FAT32 (%d clusters)" % clusters)
        self.data_start = RESERVED + NUM_FATS * fat_sectors
        self.fat = [0] * (clusters + 2)
        self.fat[0], self.fat[1] = 0x0FFFFFF8, 0x0FFFFFFF
        self.data = bytearray(clusters * SEC_PER_CLUS * SECTOR)
        self.next_free = 2

    def alloc_chain(self, nbytes):
        """Reserve a cluster chain and return its first cluster."""
        csize = SEC_PER_CLUS * SECTOR
        n = max(1, (nbytes + csize - 1) // csize)
        first = self.next_free
        for i in range(n):
            c = first + i
            self.fat[c] = 0x0FFFFFFF if i == n - 1 else c + 1
        self.next_free += n
        return first

    def write_cluster_data(self, first, blob):
        off = (first - 2) * SEC_PER_CLUS * SECTOR
        self.data[off:off + len(blob)] = blob


def dirent(name, ext, attr, cluster, size):
    e = bytearray(32)
    e[0:8] = name.upper().ljust(8)[:8].encode()
    e[8:11] = ext.upper().ljust(3)[:3].encode()
    e[11] = attr
    struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into('<H', e, 26, cluster & 0xFFFF)
    struct.pack_into('<I', e, 28, size)
    # a fixed timestamp keeps the image byte-reproducible
    struct.pack_into('<H', e, 22, 0)
    struct.pack_into('<H', e, 24, (2024 - 1980) << 9 | 1 << 5 | 1)
    return bytes(e)


def split83(name):
    if '.' in name:
        base, ext = name.rsplit('.', 1)
    else:
        base, ext = name, ''
    if len(base) > 8 or len(ext) > 3:
        raise SystemExit("name does not fit 8.3: %s" % name)
    return base, ext


def build_dir(fs, path, is_root, self_clus, parent_clus):
    """Recursively lay out a directory, returning its cluster and entries."""
    entries = []
    if not is_root:
        entries.append(dirent('.', '', 0x10, self_clus, 0))
        entries.append(dirent('..', '', 0x10, parent_clus if parent_clus != 2 else 0, 0))

    children = sorted(os.listdir(path))
    subdirs = []
    for name in children:
        full = os.path.join(path, name)
        base, ext = split83(name)
        if os.path.isdir(full):
            # size the child first so we know how many clusters it needs
            clus = fs.alloc_chain(SEC_PER_CLUS * SECTOR)
            entries.append(dirent(base, ext, 0x10, clus, 0))
            subdirs.append((full, clus, self_clus))
        else:
            blob = open(full, 'rb').read()
            clus = fs.alloc_chain(len(blob)) if blob else 0
            if blob:
                fs.write_cluster_data(clus, blob)
            entries.append(dirent(base, ext, 0x20, clus, len(blob)))

    blob = b''.join(entries)
    csize = SEC_PER_CLUS * SECTOR
    if len(blob) > csize:
        raise SystemExit("directory %s needs more than one cluster" % path)
    fs.write_cluster_data(self_clus, blob)

    for sub, clus, parent in subdirs:
        build_dir(fs, sub, False, clus, parent)


ESP_TYPE_GUID = guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
# The kernel identifies its store by the magic in the partition's first sector,
# not by this type, so the value only has to be distinct from anything real.
STORE_TYPE_GUID = guid("4D494E49-4F53-3332-5354-4F5245303031")

# Matches RAMDISK_BYTES in boot/uefi.c: the store must cover every sector the
# RAM disk can hold, or writes past the initial image go nowhere.
RAMDISK_SECTORS = 8 * 1024 * 1024 // SECTOR

GPT_ENTRIES = 128
GPT_ENTRY_SIZE = 128
GPT_TABLE_SECTORS = GPT_ENTRIES * GPT_ENTRY_SIZE // SECTOR   # 32


def store_header(fs_sectors):
    """Must stay in step with store_hdr_t in kernel/kernel.c."""
    h = bytearray(SECTOR)
    struct.pack_into('<IIII', h, 0, 0x494E494D, 0x3233534F, 1, fs_sectors)
    return bytes(h)


def gpt_entry(type_guid, part_guid, first, last, name):
    e = bytearray(GPT_ENTRY_SIZE)
    e[0:16] = type_guid
    e[16:32] = part_guid
    struct.pack_into('<QQQ', e, 32, first, last, 0)
    enc = name.encode('utf-16-le')[:72]
    e[56:56 + len(enc)] = enc
    return bytes(e)


def gpt_header(my_lba, alt_lba, first_usable, last_usable, entry_lba,
               entries_crc, disk_guid):
    h = bytearray(92)
    h[0:8] = b'EFI PART'
    struct.pack_into('<IIII', h, 8, 0x00010000, 92, 0, 0)   # rev, size, crc=0, rsvd
    struct.pack_into('<QQQQ', h, 24, my_lba, alt_lba, first_usable, last_usable)
    h[56:72] = disk_guid
    struct.pack_into('<QIII', h, 72, entry_lba, GPT_ENTRIES, GPT_ENTRY_SIZE,
                     entries_crc)
    struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h) + b'\0' * (SECTOR - 92)


def write_gpt(img, total_sectors, parts):
    """parts: list of (type_guid, first_lba, last_lba, name)."""
    table = bytearray(GPT_ENTRIES * GPT_ENTRY_SIZE)
    for i, (tguid, first, last, name) in enumerate(parts):
        pguid = guid("00000000-0000-4000-8000-%012X" % (i + 1))
        table[i * GPT_ENTRY_SIZE:(i + 1) * GPT_ENTRY_SIZE] = \
            gpt_entry(tguid, pguid, first, last, name)
    crc = zlib.crc32(bytes(table)) & 0xFFFFFFFF
    disk_guid = guid("4D494E49-4F53-3332-4449-534B00000001")

    primary_entries = 2
    backup_entries = total_sectors - 1 - GPT_TABLE_SECTORS
    first_usable = primary_entries + GPT_TABLE_SECTORS
    last_usable = backup_entries - 1

    img[1 * SECTOR:2 * SECTOR] = gpt_header(
        1, total_sectors - 1, first_usable, last_usable,
        primary_entries, crc, disk_guid)
    img[primary_entries * SECTOR:primary_entries * SECTOR + len(table)] = table

    img[(total_sectors - 1) * SECTOR:total_sectors * SECTOR] = gpt_header(
        total_sectors - 1, 1, first_usable, last_usable,
        backup_entries, crc, disk_guid)
    img[backup_entries * SECTOR:backup_entries * SECTOR + len(table)] = table

    # Protective MBR: one 0xEE partition covering the disk, so anything that
    # only understands MBR sees the space as taken rather than empty.
    struct.pack_into('<B', img, 446, 0x00)
    img[446 + 1:446 + 4] = b'\x00\x02\x00'
    img[446 + 4] = 0xEE
    img[446 + 5:446 + 8] = b'\xFF\xFF\xFF'
    struct.pack_into('<I', img, 446 + 8, 1)
    struct.pack_into('<I', img, 446 + 12,
                     min(total_sectors - 1, 0xFFFFFFFF))
    struct.pack_into('<H', img, 510, 0xAA55)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    size_mb = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    store_mb = int(sys.argv[4]) if len(sys.argv) > 4 else 16

    fs_bin_path = os.path.join(os.path.dirname(out), 'fs.bin')
    fs_bin = open(fs_bin_path, 'rb').read() if os.path.exists(fs_bin_path) else b''
    fs_bin += b'\0' * (-len(fs_bin) % SECTOR)

    total_sectors = size_mb * 1024 * 1024 // SECTOR
    store_sectors = store_mb * 1024 * 1024 // SECTOR
    # Backup GPT needs the tail; align the store down to a 1MB boundary.
    store_start = (total_sectors - 1 - GPT_TABLE_SECTORS - store_sectors) & ~2047
    part_sectors = store_start - PART_START
    if part_sectors < 2048 or store_start <= PART_START:
        raise SystemExit("image too small for an ESP plus a %dMB store" % store_mb)
    if (1 + RAMDISK_SECTORS) * SECTOR > store_sectors * SECTOR:
        raise SystemExit("store partition is smaller than the RAM disk")
    if len(fs_bin) > RAMDISK_SECTORS * SECTOR:
        raise SystemExit("filesystem image is larger than the RAM disk")

    fs = Fat32(part_sectors, PART_START)

    root = fs.alloc_chain(SEC_PER_CLUS * SECTOR)   # cluster 2
    assert root == 2
    build_dir(fs, src, True, root, 0)

    # --- boot sector -------------------------------------------------------
    bs = bytearray(SECTOR)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<H', bs, 11, SECTOR)
    bs[13] = SEC_PER_CLUS
    struct.pack_into('<H', bs, 14, RESERVED)
    bs[16] = NUM_FATS
    struct.pack_into('<H', bs, 17, 0)              # root entries: 0 on FAT32
    struct.pack_into('<H', bs, 19, 0)              # total16: 0, use total32
    bs[21] = 0xF8
    struct.pack_into('<H', bs, 22, 0)              # FAT size 16: 0 on FAT32
    struct.pack_into('<H', bs, 24, 32)             # sectors per track
    struct.pack_into('<H', bs, 26, 8)              # heads
    struct.pack_into('<I', bs, 28, PART_START)     # hidden sectors
    struct.pack_into('<I', bs, 32, part_sectors)
    struct.pack_into('<I', bs, 36, fs.fat_sectors)
    struct.pack_into('<H', bs, 40, 0)              # ext flags
    struct.pack_into('<H', bs, 42, 0)              # version
    struct.pack_into('<I', bs, 44, 2)              # root cluster
    struct.pack_into('<H', bs, 48, 1)              # FSInfo sector
    struct.pack_into('<H', bs, 50, 6)              # backup boot sector
    bs[64] = 0x80
    bs[66] = 0x29
    struct.pack_into('<I', bs, 67, 0x4D494E49)
    bs[71:82] = b'MINIOS     '
    bs[82:90] = b'FAT32   '
    struct.pack_into('<H', bs, 510, 0xAA55)

    # --- FSInfo ------------------------------------------------------------
    fsi = bytearray(SECTOR)
    struct.pack_into('<I', fsi, 0, 0x41615252)
    struct.pack_into('<I', fsi, 484, 0x61417272)
    struct.pack_into('<I', fsi, 488, 0xFFFFFFFF)
    struct.pack_into('<I', fsi, 492, fs.next_free)
    struct.pack_into('<H', fsi, 510, 0xAA55)

    # --- assemble the partition -------------------------------------------
    part = bytearray(part_sectors * SECTOR)
    part[0:SECTOR] = bs
    part[SECTOR:2 * SECTOR] = fsi
    part[6 * SECTOR:7 * SECTOR] = bs               # backup boot sector

    fat_blob = b''.join(struct.pack('<I', e) for e in fs.fat)
    fat_blob += b'\0' * (fs.fat_sectors * SECTOR - len(fat_blob))
    for i in range(NUM_FATS):
        off = (RESERVED + i * fs.fat_sectors) * SECTOR
        part[off:off + len(fat_blob)] = fat_blob

    doff = fs.data_start * SECTOR
    part[doff:doff + len(fs.data)] = fs.data

    # --- GPT ---------------------------------------------------------------
    # The laptop is GPT, and the kernel finds its store by walking the GPT, so
    # the test image has to be GPT too or none of that code gets exercised.
    total_sectors = size_mb * 1024 * 1024 // SECTOR
    img = bytearray(total_sectors * SECTOR)
    img[PART_START * SECTOR:(PART_START + part_sectors) * SECTOR] = part

    parts = [(ESP_TYPE_GUID, PART_START, PART_START + part_sectors - 1, "EFI System"),
             (STORE_TYPE_GUID, store_start, store_start + store_sectors - 1, "minios store")]
    write_gpt(img, total_sectors, parts)

    # A store the kernel will actually adopt: header plus the same image the
    # ESP carries, so a fresh QEMU run comes up persistent.
    store_blob = store_header(RAMDISK_SECTORS) + fs_bin
    store_blob += b'\0' * ((1 + RAMDISK_SECTORS) * SECTOR - len(store_blob))
    img[store_start * SECTOR:store_start * SECTOR + len(store_blob)] = store_blob

    open(out, 'wb').write(bytes(img))
    print("built %s: %dMB GPT, ESP at LBA %d (%d clusters), store at LBA %d (%d MB)"
          % (out, size_mb, PART_START, fs.next_free - 2,
             store_start, store_sectors // 2048))


if __name__ == '__main__':
    main()
