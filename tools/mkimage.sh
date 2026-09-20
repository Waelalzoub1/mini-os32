#!/usr/bin/env bash
# mkimage.sh FSDIR OUT.img -- build a bootable BIOS disk image from an arbitrary
# filesystem directory, reusing the boot/kernel artifacts produced by build.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
FSDIR=$1; OUT=$2
KERNEL_SECTORS=128; STAGE2_SECTORS=8; DIR_SECTORS=64
KERNEL_LBA=$((1 + STAGE2_SECTORS)); FS_DIR_LBA=$((KERNEL_LBA + KERNEL_SECTORS)); FS_DATA_LBA=$((FS_DIR_LBA + DIR_SECTORS))
[ -f build/kernel.pad ] || ./build.sh >/dev/null
FSBIN=$(mktemp)
python3 tools/mkfs.py "$FSDIR" "$FSBIN" $DIR_SECTORS $FS_DATA_LBA
dd if=/dev/zero of="$OUT" bs=512 count=32768 status=none
dd if=build/boot.bin of="$OUT" conv=notrunc status=none
dd if=build/stage2.pad of="$OUT" bs=512 seek=1 conv=notrunc status=none
dd if=build/kernel.pad of="$OUT" bs=512 seek=$KERNEL_LBA conv=notrunc status=none
dd if="$FSBIN" of="$OUT" bs=512 seek=$FS_DIR_LBA conv=notrunc status=none
rm -f "$FSBIN"
echo "built $OUT"
