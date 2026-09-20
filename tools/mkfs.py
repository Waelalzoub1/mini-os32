#!/usr/bin/env python3
"""Pack the fs/ tree into the flat mini-os32 filesystem image.

The OS filesystem is flat: "lib/stdio.c" is one literal 15-char key.  Host
subdirectories under fs/ become name prefixes, and each subdirectory also
gets a zero-size marker entry with a trailing slash ("lib/"), which is what
the shell's cd/ls recognise as a directory.
"""
import os
import struct
import sys

ENTRY_SIZE = 64
NAME_MAX = 47


def collect(fsdir):
    """Return [(key, path-or-None)] — None marks a directory entry."""
    items = []
    for root, dirs, files in os.walk(fsdir):
        dirs.sort()
        rel = os.path.relpath(root, fsdir)
        prefix = '' if rel == '.' else rel.replace(os.sep, '/') + '/'
        if prefix:
            items.append((prefix, None))
        for name in sorted(files):
            items.append((prefix + name, os.path.join(root, name)))
    for key, _ in items:
        if len(key) > NAME_MAX:
            sys.exit("mkfs: name too long for the 15-char FS limit: %r" % key)
    items.sort(key=lambda kv: kv[0])
    return items


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

    items = collect(fsdir)
    if len(items) > entries:
        sys.exit("mkfs: %d entries but the directory holds %d" % (len(items), entries))
    cur_lba = data_lba

    for i, (key, path) in enumerate(items):
        if path is None:
            blob = b''
        else:
            with open(path, 'rb') as f:
                blob = f.read()
        size = len(blob)
        sectors = (size + 511) // 512

        entry = bytearray(ENTRY_SIZE)
        enc = key.encode('ascii', 'ignore')[:NAME_MAX]
        entry[0:len(enc)] = enc
        struct.pack_into('<I', entry, 48, cur_lba)
        struct.pack_into('<I', entry, 52, size)
        struct.pack_into('<I', entry, 56, 0)

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
