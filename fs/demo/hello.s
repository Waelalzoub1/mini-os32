# hello.s -- sample for the in-OS assembler.
#
#   as            source file: hello.s
#                 output file: hello
#   run hello
#
# There is no crt0 and no linker, so _start is the entry point and the
# program must exit itself with the exit syscall.
#
# Syscalls go through int $0x80: eax = number, then ebx, ecx, edx, esi.
# Pointers are plain addresses in this program's own image.

.equ SYS_WRITE, 0
.equ SYS_EXIT,  6
.equ STDOUT,    1

.text
.globl _start

_start:
    # write(STDOUT, msg, msg_len)
    mov  $SYS_WRITE, %eax
    mov  $STDOUT, %ebx
    mov  $msg, %ecx
    mov  $msg_len, %edx
    int  $0x80

    # count down from 5, printing a digit each time
    mov  $5, %esi
next:
    mov  %esi, %eax
    add  $'0', %eax
    mov  %al, digit

    push %esi
    mov  $SYS_WRITE, %eax
    mov  $STDOUT, %ebx
    mov  $digit, %ecx
    mov  $2, %edx
    int  $0x80
    pop  %esi

    dec  %esi
    cmp  $0, %esi
    jg   next

    call newline

    # exit(0)
    mov  $SYS_EXIT, %eax
    mov  $0, %ebx
    int  $0x80

newline:
    mov  $SYS_WRITE, %eax
    mov  $STDOUT, %ebx
    mov  $nl, %ecx
    mov  $1, %edx
    int  $0x80
    ret

.data
msg:
    .ascii "hello from as\n"
.equ msg_len, . - msg

digit:
    .byte '0'
    .byte ' '
nl:
    .byte '\n'
