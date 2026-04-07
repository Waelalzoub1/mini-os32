#!/usr/bin/env bash
set -euo pipefail

KERNEL_SECTORS=128
STAGE2_SECTORS=8
DIR_SECTORS=2
KERNEL_LBA=$((1 + STAGE2_SECTORS))
FS_DIR_LBA=$((KERNEL_LBA + KERNEL_SECTORS))
FS_DATA_LBA=$((FS_DIR_LBA + DIR_SECTORS))

mkdir -p build

as --32 -o build/boot.o --defsym STAGE2_SECTORS=$STAGE2_SECTORS boot/boot.S
ld -m elf_i386 -Ttext 0x7c00 --oformat binary -o build/boot.bin build/boot.o

as --32 -o build/stage2.o --defsym KERNEL_SECTORS=$KERNEL_SECTORS --defsym KERNEL_LBA=$KERNEL_LBA boot/stage2.S
ld -m elf_i386 -Ttext 0x7e00 -e stage2_start --oformat binary -o build/stage2.bin build/stage2.o

as --32 -o build/entry.o kernel/entry.S
as --32 -o build/syscall.o kernel/syscall.S
as --32 -o build/isr.o kernel/isr.S
as --32 -o build/bios.o kernel/bios.S

gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -nodefaultlibs -DKERNEL_SECTORS=$KERNEL_SECTORS -DSTAGE2_SECTORS=$STAGE2_SECTORS \
  -c kernel/kernel.c -o build/kernel.o

ld -m elf_i386 -T kernel/linker.ld -o build/kernel.elf \
  build/entry.o build/syscall.o build/isr.o build/bios.o build/kernel.o

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

gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -nodefaultlibs -c user/libc.c -o build/libc.o

gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -nodefaultlibs -c user/sh.c -o build/sh.o

gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -nodefaultlibs -c user/cc.c -o build/cc.o
gcc -m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector \
  -nostdlib -nostartfiles -nodefaultlibs -c user/vbeprobe.c -o build/vbeprobe.o

ld -m elf_i386 -T user/user.ld -o build/shell.elf build/crt0.o build/libc.o build/sh.o
ld -m elf_i386 -T user/user.ld -o build/cc.elf build/crt0.o build/libc.o build/cc.o
ld -m elf_i386 -T user/user.ld -o build/vbeprobe.elf build/crt0.o build/libc.o build/vbeprobe.o

cp build/shell.elf fs/shell
cp build/cc.elf fs/cc
cp build/vbeprobe.elf fs/vbeprobe

python3 tools/mkfs.py fs build/fs.bin $DIR_SECTORS $FS_DATA_LBA

# 16MB disk image
SECTORS=32768

dd if=/dev/zero of=build/disk.img bs=512 count=$SECTORS status=none

dd if=build/boot.bin of=build/disk.img conv=notrunc status=none

dd if=build/stage2.pad of=build/disk.img bs=512 seek=1 conv=notrunc status=none

dd if=build/kernel.pad of=build/disk.img bs=512 seek=$KERNEL_LBA conv=notrunc status=none

dd if=build/fs.bin of=build/disk.img bs=512 seek=$FS_DIR_LBA conv=notrunc status=none

echo "built build/disk.img"
