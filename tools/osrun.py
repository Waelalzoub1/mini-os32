#!/usr/bin/env python3
"""Run a mini-os32 user ELF on the host under the Unicorn CPU emulator.

Emulates the kernel's int 0x80 ABI (eax = syscall number, ebx/ecx/edx/esi =
args) for the file/console syscalls a compiler or test program needs. The
"filesystem" is the current directory. This is a development convenience;
the QEMU run is the real thing.

    osrun.py PROGRAM [< stdin]        exit status = program's sys_exit code
"""
import os, struct, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_INTR, UcError
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_EIP)

# from kernel/kernel.c
USER_SPACE_SIZE = 16 * 1024 * 1024
USER_FB_SIZE = 8 * 1024 * 1024
USER_FB_OFFSET = USER_SPACE_SIZE - USER_FB_SIZE      # stack top / entry esp
USER_STACK_GUARD = USER_FB_OFFSET - 1024 * 1024 - 4096

SYS = dict(WRITE=0, READ=1, LIST=2, LOAD=3, SAVE=4, EXEC=5, EXIT=6, SBRK=7, RENAME=8,
           DELETE=9, GETKEY=10, CLS=11, SETCURSOR=12, VMODE=13, BLIT=14, PALETTE=15,
           OPEN=16, FREAD=17, FWRITE=18, CLOSE=19, SEEK=20, GFXINFO=21, VBEMODES=22,
           GETKEY_NB=23, TICKS=24, TCREATE=25, TEXIT=26, SLEEP=27, SLEEPF=28,
           GFX_FBINFO=29, KEYSTATE=30)
NAMES = {v: k for k, v in SYS.items()}


class Machine:
    def __init__(self, path):
        self.mu = Uc(UC_ARCH_X86, UC_MODE_32)
        self.mu.mem_map(0, USER_SPACE_SIZE)
        data = open(path, 'rb').read()
        if data[:4] != b'\x7fELF': raise SystemExit('not an ELF file')
        e_entry, e_phoff = struct.unpack_from('<II', data, 24)
        e_phentsize, e_phnum = struct.unpack_from('<HH', data, 42)
        brk = 0
        for i in range(e_phnum):
            ph = e_phoff + i * e_phentsize
            p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from('<IIIIII', data, ph)
            if p_type != 1: continue
            self.mu.mem_write(p_vaddr, data[p_offset:p_offset + p_filesz])
            brk = max(brk, p_vaddr + p_memsz)
        self.brk = brk
        self.entry = e_entry
        self.exit_code = None
        self.ticks = 0
        self.mu.hook_add(UC_HOOK_INTR, self.on_intr)

    def rd(self, addr, n): return bytes(self.mu.mem_read(addr, n))
    def cstr(self, addr, max=32):
        b = self.rd(addr, max)
        return b.split(b'\0', 1)[0].decode('latin-1')

    def on_intr(self, mu, intno, _):
        if intno != 0x80:
            sys.stderr.write('unexpected interrupt %d at eip=%#x\n' % (intno, mu.reg_read(UC_X86_REG_EIP)))
            self.exit_code = 139; mu.emu_stop(); return
        n = mu.reg_read(UC_X86_REG_EAX)
        a, b, c = mu.reg_read(UC_X86_REG_EBX), mu.reg_read(UC_X86_REG_ECX), mu.reg_read(UC_X86_REG_EDX)
        r = self.syscall(n, a, b, c)
        if r is None: return
        mu.reg_write(UC_X86_REG_EAX, r & 0xFFFFFFFF)

    def syscall(self, n, a, b, c):
        name = NAMES.get(n)
        if name == 'WRITE':
            if a in (1, 2):
                sys.stdout.buffer.write(self.rd(b, c)); sys.stdout.flush(); return c
            return -1
        if name == 'READ':
            if a != 0 or c == 0: return -1
            line = sys.stdin.buffer.readline().rstrip(b'\n')[:c - 1]
            self.mu.mem_write(b, line + b'\0')
            return len(line)
        if name == 'LOAD':
            fn = self.find(self.cstr(a))
            if fn is None: return -1
            blob = open(fn, 'rb').read()
            if len(blob) > c: return -1
            self.mu.mem_write(b, blob); return len(blob)
        if name == 'SAVE':
            fn = self.find(self.cstr(a)) or self.cstr(a)
            open(fn, 'wb').write(self.rd(b, c)); return 0
        if name == 'EXIT':
            self.exit_code = a; self.mu.emu_stop(); return None
        if name == 'SBRK':
            prev = self.brk
            if a > USER_STACK_GUARD - prev: return -1
            self.brk = prev + a; return prev
        if name == 'RENAME':
            old, new = self.find(self.cstr(a)), self.cstr(b)
            if old is None or self.find(new): return -1
            os.rename(old, new); return 0
        if name == 'DELETE':
            fn = self.find(self.cstr(a))
            if fn is None: return -1
            os.unlink(fn); return 0
        if name == 'TICKS':
            self.ticks += 1; return self.ticks
        if name in ('GETKEY', 'GETKEY_NB', 'KEYSTATE'): return -1
        if name in ('CLS', 'SETCURSOR', 'SLEEP', 'SLEEPF'): return 0
        return -1

    def find(self, name):
        """case-insensitive lookup relative to cwd (paths like lib/stdio.c allowed)"""
        d, base = os.path.split(name.lstrip('/'))
        d = d or '.'
        if not os.path.isdir(d): return None
        for fn in os.listdir(d):
            if fn.upper() == base.upper(): return os.path.join(d, fn) if d != '.' else fn
        return None

    def run(self):
        # crt0: call main; push eax; call sys_exit  -- so a fresh stack is enough
        self.mu.reg_write(UC_X86_REG_ESP, USER_FB_OFFSET - 16)
        try:
            self.mu.emu_start(self.entry, USER_SPACE_SIZE)
        except UcError as e:
            if self.exit_code is None:
                sys.stderr.write('fault: %s at eip=%#x\n' % (e, self.mu.reg_read(UC_X86_REG_EIP)))
                self.exit_code = 139
        return self.exit_code if self.exit_code is not None else 0

if __name__ == '__main__':
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    sys.exit(Machine(sys.argv[1]).run() & 0xFF)
