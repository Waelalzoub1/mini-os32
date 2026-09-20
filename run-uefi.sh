#!/bin/sh
# Boot build/minios-uefi.img under QEMU with real UEFI firmware, so the same
# path that runs on the laptop can be tested without rebooting it.
#
#   ./run-uefi.sh              boot a scratch copy of the image (default)
#   ./run-uefi.sh -p           boot the image itself, keeping writes
#   ./run-uefi.sh -b           build first, then boot
#   ./run-uefi.sh -k           use KVM (much faster; UEFI path only)
#   ./run-uefi.sh -- <args>    pass extra arguments straight to QEMU
#
# Writes go to build/run-uefi.img, a copy, unless -p is given.  The image is
# GPT with an ESP and a mini-os32 store partition, and the disk is attached as
# NVMe -- the same shape as the laptop -- so -p exercises real persistence.
set -e
cd "$(dirname "$0")"

CODE=/usr/share/qemu/edk2-x86_64-code.fd
VARS_TEMPLATE=/usr/share/qemu/edk2-i386-vars.fd   # x86_64 uses the i386 varstore
VARS=build/uefi-vars.fd
IMG=build/minios-uefi.img
RUN=build/run-uefi.img
MEM=512
PERSIST=0
BUILD=0
ACCEL=""      # KVM works here but not on the BIOS build, which hangs in a
              # real-mode VBE BIOS call; the UEFI path never leaves 32-bit
              # protected mode after the trampoline.

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--persist) PERSIST=1; shift ;;
        -b|--build)   BUILD=1; shift ;;
        -k|--kvm)     ACCEL="-accel kvm"; shift ;;
        -m)           MEM="$2"; shift 2 ;;
        --)           shift; break ;;
        -h|--help)    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)            echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

[ "$BUILD" = 1 ] && ./build.sh

if [ ! -f "$CODE" ]; then
    echo "UEFI firmware not found at $CODE" >&2
    echo "install the edk2 / OVMF firmware package for qemu" >&2
    exit 1
fi
[ -f "$IMG" ] || { echo "$IMG missing -- run ./build.sh" >&2; exit 1; }

# Each run needs its own writable copy of the variable store, or the firmware
# cannot record a boot entry and drops to the shell.
[ -f "$VARS" ] || cp "$VARS_TEMPLATE" "$VARS"

if [ "$PERSIST" = 1 ]; then
    DISK="$IMG"
else
    # Keep the previous scratch image: it may hold a store that was synced
    # during the last session, and overwriting it silently has cost work.
    [ -f "$RUN" ] && mv "$RUN" "$RUN.prev"
    cp "$IMG" "$RUN"
    DISK="$RUN"
fi

exec qemu-system-x86_64 \
    -machine q35 \
    -m "$MEM" \
    -drive if=pflash,format=raw,unit=0,readonly=on,file="$CODE" \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive file="$DISK",format=raw,if=none,id=osdisk \
    -device nvme,serial=minios0,drive=osdisk \
    -net none \
    $ACCEL \
    "$@"
