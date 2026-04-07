# mini-os32 OS + C Compiler Guide

This OS ships with a tiny in-OS C compiler (`cc`). It is **not full C**, but it is consistent and predictable once you know the rules. This document covers how to build/boot, how to use the compiler, the supported language subset, library usage, graphics, syscalls, and current limits.

## Requirements

A Linux host (or WSL) with:

- `gcc` with 32-bit support (`gcc-multilib` on Debian/Ubuntu)
- `binutils` (`as`, `ld`, `objcopy`)
- `python3`
- `qemu-system-i386` to run it

On Debian/Ubuntu:
```bash
sudo apt install build-essential gcc-multilib python3 qemu-system-x86
```

**Quick Start**
1. Build and boot:
```bash
./build.sh
qemu-system-i386 -hda build/disk.img
```
2. In the OS shell, compile:
```
cc
source files: hello.c
output file: hello
```
3. Run:
```
run hello
```

**Compiler Flow**
- `cc` asks for a **space‑separated list** of source files, then an output file name.
- It automatically **adds `stdio.c`** if you did not list it.
- For other libraries (`string.c`, `stdlib.c`, `gfx.c`) you must list the `.c` file yourself.
- Output files have **no extension**.

Example:
```
cc
source files: app.c string.c stdlib.c
output file: app
```

## Language Summary

**Types**
- `int`, `unsigned int`
- `char`, `unsigned char`
- `float`
- `bool` / `_Bool`
- `void`
- pointers (`int *p`)
- arrays (`int a[4]`)
- `struct`, `enum`, `typedef`
- `union`
- storage/qualifiers: `static`, `extern`, `const`, `volatile`

**Literals**
- Decimal and hex integers: `123`, `0x7B`
- Floats: `3.14`
- Char: `'a'`, `'\n'`, `'\t'`
- Strings: `"hello"` (only `\n` and `\t` escapes are supported)

**Operators**
- Arithmetic: `+ - * / %`
- Bitwise: `& | ^ ~ << >>`
- Comparison: `== != < <= > >=`
- Logical: `&& || !` (short‑circuit)
- Assignment: `=`
- Address/deref: `&x`, `*p`
- Member: `.` and `->`
- `sizeof` for types and expressions

**Statements**
- Block: `{ ... }`
- `if / else`
- `while`
- `for` (init/cond/post are supported, but keep init as an expression, not a declaration)
- `switch / case / default / break`
- `continue`
- `return`
- Declarations inside blocks (but avoid mixing new declarations after many statements to stay safe)

**Functions**
- Up to **8 parameters**.
- Call by value only.
- Varargs supported via `va_start`, `va_arg`, `va_end` (see `stdarg.h`).

**Structs**
- Field access with `.` and `->`
- `sizeof(struct X)` works
- **No** struct assignment or return by value
- **No** passing structs by value

**Enums**
- `enum { A=1, B=2 }`
- Enums are `int`

**Pointers & Arrays**
- Pointer arithmetic works (`p + 1`, `p - 1`)
- Array indexing works (`a[i]`)

## Preprocessor (`#`)

Supported directives:
- `#include "file.h"` or `#include <file.h>` (both load a file from the OS root)
- `#define NAME value`
- `#define F(x,y) ...` (function‑like macros, up to 8 params)
- `#undef NAME`
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
- `__LINE__`, `__FILE__`

Notes:
- Include depth limit: **9**
- Macro count limit: **256**
- Macro body size limit: **256**
- No token‑pasting (`##`) or stringizing (`#`)
- Preprocessor is **line‑based**, so very long lines are truncated (see Limits)

## Standard Library (Available in FS)

**`stdio`**
- Header: `#include <stdio.h>`
- Functions: `putc`, `puts`, `readline`, `printf`, `snprintf`, `vsnprintf`, `scanf`
- `puts` does **not** append a newline. Use `puts("\n")` or `printf("\n")`.
- `printf` formats: `%d %u %x %X %c %s %%`
- `scanf` formats: `%d %u %x %X %c %s`
- No float formatting or float scanf.

**`string`**
- Header: `#include <string.h>`
- Compile with: `string.c`
- Functions: `memcpy`, `memset`, `memmove`, `memcmp`, `strlen`, `strcmp`, `strncmp`, `strcpy`, `strncpy`

**`stdlib`**
- Header: `#include <stdlib.h>`
- Compile with: `stdlib.c`
- Functions: `malloc`, `free`, `calloc`, `realloc`, `atoi`, `strtol`

**`errno`**
- Header: `#include <errno.h>`
- Globals: `errno`
- Codes: `ENOMEM`, `EINVAL`

**`gfx`**
- Header: `#include <gfx.h>`
- Compile with: `gfx.c`
- Functions: `gfx_mode`, `gfx_blit`, `gfx_set_palette`, `gfx_info`

**`stdarg`**
- Header: `#include <stdarg.h>`
- Provides `va_list` typedef only.
- You call `va_start`, `va_arg`, `va_end` directly (builtins).

## How To Include Files

Headers give you prototypes. The compiler only links code you **list in the source list**.

Example using `string`:
```c
// main.c
#include <stdio.h>
#include <string.h>

int main() {
    char buf[8];
    strcpy(buf, "hi");
    printf("%s\n", buf);
    return 0;
}
```
Compile:
```
cc
source files: main.c string.c
output file: main
```

## VESA / VGA Graphics

### Modes
There are two paths depending on how the OS booted:

1. **VESA mode (preferred)**  
   The bootloader selects the **best 8‑bpp VESA mode**, preferring `>= 1280x720` if available.  
   In this mode, user programs **cannot change resolution**. You render into the current framebuffer.

2. **VGA mode (fallback)**  
   If VESA is unavailable, you can use VGA mode 13 (`320x200x256`) and return to text.

### API
```c
#include <gfx.h>

gfx_mode(GFX_MODE_320x200x256);   // returns 0 on success, -1 on failure
gfx_mode(GFX_MODE_TEXT);          // return to text mode

gfx_info_t gi;
gfx_info(&gi);                    // fills w/h/pitch/bpp

gfx_set_palette(i, r, g, b);      // r/g/b in 0..255
gfx_blit(buf);                    // buf size = gi.w * gi.h (8bpp)
```

**Important details**
- Framebuffer is **8‑bpp indexed color**.
- `gfx_blit` copies **`w` bytes per row** into the real framebuffer, using the hardware pitch internally.  
  Your buffer must be **tightly packed**: `index = y * w + x`.
- `gfx_set_palette` expects 0‑255 RGB; VGA hardware uses 6‑bit internally.

### Demo / Test Programs
In the filesystem:
- `vesa_demo.c` shows a simple gradient.
- `gfx_test.c` draws a gradient + checkerboard + X to verify pitch/mode correctness.

Compile and run:
```
cc gfx_test.c gfx.c
gfx_test
```

## Syscalls

You can call syscalls via the wrappers in `user/libc.h` or the `syscall(n, ...)` builtin (1–4 args).

**Available syscalls**
- `sys_write(fd, buf, len)` write to FD (1 = stdout)
- `sys_read(fd, buf, len)` read from FD (0 = stdin)
- `sys_list(buf, max)` list FS entries (internal use)
- `sys_load(name, buf, max)` read file into buffer
- `sys_save(name, buf, size)` write file
- `sys_exec(name)` run program
- `sys_exit(code)` exit program
- `sys_sbrk(inc)` grow heap
- `sys_rename(old, new)` rename file
- `sys_delete(name)` delete file
- `sys_getkey()` wait for a keypress
- `sys_cls()` clear screen
- `sys_setcursor(x, y)` set cursor position
- `sys_vmode(mode)` set graphics/text mode
- `sys_blit(buf)` blit 8‑bpp buffer to screen
- `sys_palette(idx, rgb)` set palette entry (rgb = `0xRRGGBB`)
- `sys_open(name, mode)` open file
- `sys_fread(fd, buf, len)` read file handle
- `sys_fwrite(fd, buf, len)` write file handle
- `sys_close(fd)` close file handle
- `sys_seek(fd, pos)` seek file handle
- `sys_gfxinfo(buf, max)` get graphics info
- `sys_vbemodes(buf, max)` get list of supported 8‑bpp LFB VBE modes (returns count)

The syscall numbers are defined in `user/libc.h`.

## Shell Commands

Inside the OS shell:
- `ls` list files
- `type <file>` print file contents
- `edit <file>` open editor
- `run <file>` execute
- `cc` compile
- `mkdir <dir>` create directory
- `rmdir <dir>` remove empty directory
- `cd <dir>` change directory
- `rm <file>` delete file
- `help` show help
- `vbeprobe` list supported VBE modes (8‑bpp LFB)

Directories are simulated by file names with `/` in them (the FS is flat internally).

## Current Limits

- Max source files per build: **8**
- Max file size per source: **~192 KB**
- Max preprocessed size: **~384 KB**
- Max line length (preprocessor): **~1535 chars**
- Max string literal length: **191 chars**
- Max identifier length: **31 chars**
- Max function args: **8**
- Max struct fields: **64**
- File name length in FS: **15 chars**
- Code size: **64 KB**
- RODATA size: **~96 KB**
- DATA size: **32 KB**
- BSS size: **1 MB**

## Known Limitations / Unsupported Features

- `static`, `const`, `volatile`
- `++`, `--`, `+=`, `-=` and other compound assignments
- `?:` (ternary)
- `do { } while (...)`
- `goto`
- `union`, bitfields
- `long`, `short`, `double`
- struct assignment or return by value
- function pointers
- adjacent string literal concatenation (`"a" "b"`) is not supported

If you hit a limitation, rewrite to the supported subset (often with `while` loops and explicit `x = x + 1`).

## Diagnostics

Errors look like:
```
error: expected ';', got ')'
file.c:12:8:     for (;;) {
       ^
```
Warnings are issued for:
- Calling an undeclared function
- Missing `return` in a non‑`void` function (except `main`)

---

If you want to extend the compiler, start with:
1. Add `++/--` and compound assignments.
2. Improve `for` parsing reliability for declarations.
3. Add `const` and `static`.
