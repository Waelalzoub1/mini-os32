#!/usr/bin/env python3
"""List or extract files from a mini-os32 store inside an image or partition.

    tools/lsstore.py <image-or-partition>              list files
    tools/lsstore.py <image-or-partition> --extract d  copy them into d/

Scans for the store header (mkstore.py's magic), then walks the flat
directory that follows it.  Works on build/run-uefi.img, its .prev backup,
build/minios-uefi.img, or a raw store partition -- so work that was synced
in a session can be pulled back out onto the host even after the VM is gone.
"""
import os
import struct
import sys

SECTOR = 512
MAGIC0 = 0x494E494D
MAGIC1 = 0x3233534F
ENTRY_SIZE = 64
NAME_MAX = 47
FS_DIR_LBA = 137          # RAM-disk LBA the directory lives at (build.sh)


def find_store(data):
    for off in range(0, len(data) - SECTOR, SECTOR):
        m0, m1, _ver, fs_sectors = struct.unpack_from('<IIII', data, off)
        if m0 == MAGIC0 and m1 == MAGIC1:
            return off + SECTOR, fs_sectors
    return None, 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    outdir = None
    if '--extract' in sys.argv:
        outdir = sys.argv[sys.argv.index('--extract') + 1]

    with open(path, 'rb') as f:
        data = f.read()

    blob_off, fs_sectors = find_store(data)
    if blob_off is None:
        print("no store header found in %s" % path)
        return 1

    blob = data[blob_off:blob_off + fs_sectors * SECTOR]
    count = 0
    i = 0
    while True:
        ent = blob[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
        if len(ent) < ENTRY_SIZE:
            break
        name = ent[0:NAME_MAX + 1].split(b'\0', 1)[0].decode('ascii', 'replace')
        start, size, _flags = struct.unpack_from('<III', ent, NAME_MAX + 1)
        i += 1
        if not name:
            # the directory region is contiguous; a hole just means a
            # deleted file, so keep scanning until entries stop looking sane
            if i * ENTRY_SIZE >= 64 * SECTOR:
                break
            continue
        if name.endswith('/'):
            print("%-48s <dir>" % name)
            if outdir:
                os.makedirs(os.path.join(outdir, name), exist_ok=True)
            count += 1
            continue
        print("%-48s %d" % (name, size))
        count += 1
        if outdir:
            off = (start - FS_DIR_LBA) * SECTOR
            body = blob[off:off + size]
            dest = os.path.join(outdir, name)
            os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
            with open(dest, 'wb') as out:
                out.write(body)
    print("%d entries" % count)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
