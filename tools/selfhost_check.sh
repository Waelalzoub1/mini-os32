#!/usr/bin/env bash
# Self-hosting check for the in-OS C compiler.
#
#   tools/selfhost_check.sh          native chain via tools/host (fast)
#   tools/selfhost_check.sh --qemu   additionally boot build/disk.img and do it inside the OS
#
# Native chain:  gcc-built cc  --cc.c-->  cc2_ref
#                cc-built cc   --cc.c-->  cc3         (cc-built cc runs natively via tools/host/runner)
#                cc2_ref == cc3 byte for byte
# In-OS chain:   cc compiles src/cc.c -> cc2 ; run cc2 compiles src/cc.c -> cc3 ; both extracted from
#                the disk image and compared with each other and with cc2_ref.
set -euo pipefail
cd "$(dirname "$0")/.."
./build.sh >/dev/null
sh tools/host/build.sh >/dev/null
W=$(mktemp -d)
mkdir -p "$W/lib"; cp fs/lib/* "$W/lib/"; cp fs/src/cc.c "$W/"; cp tools/host/{hostcc,hostcc0,loader32,runner} "$W/"
( cd "$W"
  printf 'cc.c string.c path.c\ncc2_ref\n' | ./hostcc0 >/dev/null
  printf 'cc.c string.c path.c\ncc2h\n'    | ./hostcc  >/dev/null
  printf 'cc.c string.c path.c\ncc3\n'     | ./runner  >/dev/null
  cmp cc2_ref cc3 && echo "native: cc2_ref == cc3 ($(stat -c%s cc3) bytes, sha256 $(sha256sum cc3 | cut -c1-16)...)" )
if [ "${1:-}" = "--qemu" ]; then
  IMG="$W/disk.img"; cp build/disk.img "$IMG"
  python3 tools/qemu_drive.py "$IMG" --cmd "@cc" --cmd "@src/cc.c string.c path.c" --cmd "cc2" \
      --cmd "@run cc2" --cmd "@src/cc.c string.c path.c" --cmd "cc3" --timeout 1500 >"$W/transcript.txt"
  python3 - "$IMG" "$W" <<'PY'
import struct, sys, hashlib
img = open(sys.argv[1], 'rb').read(); w = sys.argv[2]
FS_DIR_LBA = 1 + 8 + 128; DIR_SECTORS = 64; ENTRY = 64          # must match build.sh / kernel.c
d = img[FS_DIR_LBA*512:(FS_DIR_LBA+DIR_SECTORS)*512]; found = {}
for i in range(len(d)//ENTRY):
    e = d[i*ENTRY:(i+1)*ENTRY]; name = e[:48].split(b'\0',1)[0].decode('latin-1')
    if name.upper() in ('CC2', 'CC3'):
        lba, size = struct.unpack_from('<II', e, 48); found[name.upper()] = img[lba*512:lba*512+size]
ref = open(w + '/cc2_ref', 'rb').read()
ok = found.get('CC2') == found.get('CC3') == ref and 'CC2' in found
print('in-OS: cc2 == cc3 == cc2_ref:', ok, '(%d bytes)' % len(ref))
sys.exit(0 if ok else 1)
PY
fi
rm -rf "$W"
