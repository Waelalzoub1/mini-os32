#!/usr/bin/env python3
import os
import struct
import sys

ENTRY_SIZE = 32


def main():
    if len(sys.argv) < 5:
        print("usage: mkfs.py <fsdir> <out> <dir_sectors> <data_lba>")
        return 1
    fsdir = sys.argv[1]
    out = sys.argv[2]
    dir_sectors = int(sys.argv[3])
    data_lba = int(sys.argv[4])

    dir_size = dir_sectors * 512
    entries = dir_size // ENTRY_SIZE
    directory = bytearray(dir_size)
    data = bytearray()

    files = [f for f in sorted(os.listdir(fsdir)) if os.path.isfile(os.path.join(fsdir, f))]
    cur_lba = data_lba

    for i, name in enumerate(files[:entries]):
        path = os.path.join(fsdir, name)
        with open(path, 'rb') as f:
            blob = f.read()
        size = len(blob)
        sectors = (size + 511) // 512

        entry = bytearray(ENTRY_SIZE)
        enc = name.encode('ascii', 'ignore')[:15]
        entry[0:len(enc)] = enc
        struct.pack_into('<I', entry, 16, cur_lba)
        struct.pack_into('<I', entry, 20, size)
        struct.pack_into('<I', entry, 24, 0)

        directory[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = entry

        data.extend(blob)
        pad = (-len(data)) % 512
        if pad:
            data.extend(b'\x00' * pad)
        cur_lba += sectors

    with open(out, 'wb') as f:
        f.write(directory)
        f.write(data)

    return 0


if __name__ == '__main__':
    raise SystemExit(main())
