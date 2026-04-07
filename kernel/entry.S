.section .text.boot
.code16
.globl start16
.globl gdt_descriptor
.globl kernel_stack_top

start16:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x9000, %sp

    inb $0x92, %al
    orb $0x02, %al
    outb %al, $0x92
    addr32 lgdt gdt_descriptor
    mov %cr0, %eax
    orl $0x1, %eax
    mov %eax, %cr0
    ljmpl $0x08, $pm_start

.code32
pm_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $kernel_stack_top, %esp
    call kmain
1:
    hlt
    jmp 1b

.section .bss
.align 16
kernel_stack:
    .space 4096
kernel_stack_top:

.section .text.boot
.align 8
.globl gdt
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    /* user code/data: base=0x00400000, limit=16MB-1 */
    .quad 0x00C0FA4000000FFF
    .quad 0x00C0F24000000FFF
    .quad 0x0000890000000000

gdt_descriptor:
    .word (gdt_descriptor - gdt - 1)
    .long gdt
