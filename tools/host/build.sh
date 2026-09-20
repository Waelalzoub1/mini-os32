#!/bin/sh
# Host test harness for the in-OS C compiler.  Builds:
#
#   hostcc   -- user/cc.c linked against a Linux-syscall shim, so the OS
#               compiler runs natively.  Output ELFs are linked at
#               LINK_BASE=0x20000000 so the runner can map them flat.
#   hostcc0  -- same, but LINK_BASE=0 (normal OS binaries), for producing
#               reference output to compare against.
#   loader32 -- maps 24MB at 0x20000000, loads "cc2h" from the cwd, and
#               jumps in under PTRACE_TRACEME.
#   runner   -- supervises loader32, emulating the OS syscalls
#               (write/read/load/save/exit) at each int 0x80.  On a fault
#               it prints registers, the nearest symbol from syms.txt, and
#               a return-address stack scan.
#
# The self-hosting fixpoint test (run from a directory with the fs/ sources):
#
#   printf 'cc.c string.c path.c\ncc2_ref\n' | hostcc0        # reference
#   printf 'cc.c string.c path.c\ncc2h\n'    | hostcc         # runnable cc2
#   printf 'cc.c string.c path.c\ncc3\n'     | ./runner       # cc2 compiles cc
#   cmp cc2_ref cc3                                           # must be identical
#
# Build hostcc with -DHOST_DEBUG for extra output: SYM/FUNC symbol dumps
# (feed "grep ^SYM > syms.txt" to the runner) and a BADSYM invariant check
# that every exported function symbol points at a prologue.
set -e
cd "$(dirname "$0")"
R=../..
CFLAGS="-m32 -ffreestanding -fno-pie -fno-pic -fno-builtin -fno-stack-protector -I$R/user"

gcc $CFLAGS -DLINK_BASE=0x20000000 -DHOST_DEBUG -c $R/user/cc.c -o cc_host.o
gcc $CFLAGS -c shim.c -o shim.o
ld -m elf_i386 -static -e _start -o hostcc cc_host.o shim.o

gcc $CFLAGS -c $R/user/cc.c -o cc_host0.o
ld -m elf_i386 -static -e _start -o hostcc0 cc_host0.o shim.o

gcc $CFLAGS -c loader32.c -o loader32.o
ld -m elf_i386 -static -e _start -o loader32 loader32.o

gcc -O1 -o runner runner.c

echo "built hostcc hostcc0 loader32 runner"
