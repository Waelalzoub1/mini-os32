.code32
.globl _start
.extern main
.extern sys_exit

_start:
    call main
    push %eax
    call sys_exit
