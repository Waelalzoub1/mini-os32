#!/usr/bin/env bash
set -euo pipefail

KERNEL_SECTORS=128
STAGE2_SECTORS=8
DIR_SECTORS=64   # 64*512/64 = 512 directory entries of 64 bytes (47-char names)
KERNEL_LBA=$((1 + STAGE2_SECTORS))
FS_DIR_LBA=$((KERNEL_LBA + KERNEL_SECTORS))
FS_DATA_LBA=$((FS_DIR_LBA + DIR_SECTORS))

mkdir -p build

# -mx86-used-note=no: the note lands at the default 0x08048000 base, which would
# stretch the --oformat binary output from 0x7c00 to 128MB of padding.
as --32 -mx86-used-note=no -o build/boot.o --defsym STAGE2_SECTORS=$STAGE2_SECTORS boot/boot.S
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o build/boot.bin build/boot.o

as --32 -mx86-used-note=no -o build/stage2.o --defsym KERNEL_SECTORS=$KERNEL_SECTORS --defsym KERNEL_LBA=$KERNEL_LBA boot/stage2.S
ld -m elf_i386 -Ttext 0x7e00 -e stage2_start --oformat binary -o build/stage2.bin build/stage2.o

as --32 -o build/entry.o --defsym BOOTINFO_ADDR=0x5000 kernel/entry.S
as --32 -o build/syscall.o kernel/syscall.S
as --32 -o build/isr.o kernel/isr.S
as --32 -o build/bios.o kernel/bios.S

# -mno-sse and friends: nothing enables CR4.OSFXSR, so any SSE instruction gcc
# emits (it likes to vectorise struct copies) faults with #UD at runtime.
CFLAGS="-m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector
  -nostdlib -nostartfiles -nodefaultlibs
  -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-avx"

gcc $CFLAGS -DKERNEL_SECTORS=$KERNEL_SECTORS -DSTAGE2_SECTORS=$STAGE2_SECTORS \
  -DDIR_SECTORS=$DIR_SECTORS \
  -c kernel/kernel.c -o build/kernel.o
gcc $CFLAGS -c kernel/nvme.c -o build/nvme.o

sed 's/@KERNEL_BASE@/0x10000/' kernel/linker.ld > build/linker-bios.ld
ld -m elf_i386 -T build/linker-bios.ld -o build/kernel.elf \
  build/entry.o build/syscall.o build/isr.o build/bios.o build/kernel.o build/nvme.o

objcopy -O binary build/kernel.elf build/kernel.bin

ksize=$(stat -c%s build/kernel.bin)
max=$((KERNEL_SECTORS * 512))
if [ "$ksize" -gt "$max" ]; then
  echo "kernel.bin too large: $ksize > $max"
  exit 1
fi

dd if=build/kernel.bin of=build/kernel.pad bs=512 count=$KERNEL_SECTORS conv=sync status=none
dd if=build/stage2.bin of=build/stage2.pad bs=512 count=$STAGE2_SECTORS conv=sync status=none

as --32 -o build/crt0.o user/crt0.S

gcc $CFLAGS -c user/libc.c -o build/libc.o

gcc $CFLAGS -c user/sh.c -o build/sh.o

gcc $CFLAGS -c user/cc.c -o build/cc.o
gcc $CFLAGS -c user/vbeprobe.c -o build/vbeprobe.o
gcc $CFLAGS -c user/as.c -o build/as.o

ld -m elf_i386 -T user/user.ld -o build/shell.elf build/crt0.o build/libc.o build/sh.o
ld -m elf_i386 -T user/user.ld -o build/cc.elf build/crt0.o build/libc.o build/cc.o
ld -m elf_i386 -T user/user.ld -o build/vbeprobe.elf build/crt0.o build/libc.o build/vbeprobe.o
ld -m elf_i386 -T user/user.ld -o build/as.elf build/crt0.o build/libc.o build/as.o

# cc's own source, so the OS can rebuild its compiler.  Copied rather than
# kept by hand: a stale fs/src/cc.c that no longer matches user/cc.c would be
# a self-hosting test that proves nothing.
mkdir -p fs/src
cp user/cc.c fs/src/cc.c

cp build/shell.elf fs/shell
cp build/cc.elf fs/cc
cp build/vbeprobe.elf fs/vbeprobe
cp build/as.elf fs/as

python3 tools/mkfs.py fs build/fs.bin $DIR_SECTORS $FS_DATA_LBA

# 16MB disk image
SECTORS=32768

dd if=/dev/zero of=build/disk.img bs=512 count=$SECTORS status=none

dd if=build/boot.bin of=build/disk.img conv=notrunc status=none

dd if=build/stage2.pad of=build/disk.img bs=512 seek=1 conv=notrunc status=none

dd if=build/kernel.pad of=build/disk.img bs=512 seek=$KERNEL_LBA conv=notrunc status=none

dd if=build/fs.bin of=build/disk.img bs=512 seek=$FS_DIR_LBA conv=notrunc status=none

echo "built build/disk.img"

# ---------------------------------------------------------------------------
# UEFI target.  Modern laptops have no CSM, so the MBR path above cannot run
# there; this builds a PE32+ loader instead.  binutils alone is enough: compile
# to ELF, then link with the i386pep emulation and mark it subsystem 10.
# ---------------------------------------------------------------------------

# -fno-ident / -fcf-protection=none / -fno-asynchronous-unwind-tables and
# -mx86-used-note=no keep .comment, .note.gnu.property and .eh_frame out of the
# image, so the PE ends up with nothing but the sections firmware needs.
EFI_CFLAGS="-m64 -ffreestanding -fno-stack-protector -fno-pie -fno-pic
  -mno-red-zone -fshort-wchar -fno-ident -fcf-protection=none
  -fno-asynchronous-unwind-tables -Wall
  -mno-sse -mno-sse2 -mno-mmx -mno-3dnow -mno-avx"

# The UEFI kernel is linked at 2MB rather than 0x10000: the image is over 1MB,
# and at 0x10000 its .bss would run through the legacy VGA aperture and ROM
# area (0xA0000-0xFFFFF), which is MMIO, not RAM.
UEFI_KERNEL_BASE=0x200000

sed "s/@KERNEL_BASE@/$UEFI_KERNEL_BASE/" kernel/linker.ld > build/linker-uefi.ld
ld -m elf_i386 -T build/linker-uefi.ld -o build/kernel-uefi.elf \
  build/entry.o build/syscall.o build/isr.o build/bios.o build/kernel.o build/nvme.o

objcopy -O binary build/kernel-uefi.elf build/kernel-uefi.bin

gcc $EFI_CFLAGS -c boot/uefi.c -o build/uefi.o
as --64 -mx86-used-note=no \
  --defsym KERNEL_LOAD_ADDR=$UEFI_KERNEL_BASE --defsym KERNEL_ENTRY32=0x10 \
  -o build/uefi_tramp.o boot/uefi_tramp.S

ld -m i386pep --subsystem 10 -e efi_main --image-base 0x100000 \
  -o build/BOOTX64.EFI build/uefi.o build/uefi_tramp.o

# Staging tree for the EFI System Partition.  QEMU can boot this directly with
# -drive file=fat:rw:build/esp; for real hardware copy its contents onto a
# FAT32 USB stick.
rm -rf build/esp
mkdir -p build/esp/EFI/BOOT build/esp/minios
cp build/BOOTX64.EFI build/esp/EFI/BOOT/BOOTX64.EFI
cp build/kernel-uefi.bin build/esp/minios/kernel.bin
cp build/fs.bin      build/esp/minios/fs.bin

# A real FAT32 ESP.  QEMU's built-in `fat:rw:<dir>` driver corrupts trees of
# this size, and a genuine image is what gets written to a USB stick anyway --
# so what we test in QEMU is exactly what boots on hardware.
python3 tools/mkesp.py build/esp build/minios-uefi.img 64

echo "built build/minios-uefi.img (UEFI)"
