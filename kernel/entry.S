/* Two ways in.
 *
 * Offset 0x00  start16 -- the legacy BIOS loader jumps here in real mode.
 * Offset 0x10  start32 -- the UEFI loader jumps here already in 32-bit
 *              protected mode with paging off and %ebx holding the bootinfo
 *              address.  It is pinned at a fixed offset (KERNEL_ENTRY32 in
 *              kernel/bootinfo.h) so the loader needs no symbol table.
 *
 * Both paths clear .bss before setting up a stack -- the stack lives in .bss,
 * so the clear itself must not touch memory.  The legacy path used to get a
 * zeroed .bss for free because the loader pads the kernel image with zeros;
 * the UEFI loader copies exactly the file, which contains no .bss at all.
 *
 * BOOTINFO_ADDR is supplied by build.sh via --defsym to keep it in step with
 * kernel/bootinfo.h.
 */
.section .text.boot
.globl start16
.globl start32
.globl gdt_descriptor
.globl kernel_stack_top

.code16
start16:
    jmp rm_entry
    .org 0x10                 /* pin start32 at KERNEL_ENTRY32 */

.code32
start32:
    cli
    lgdt gdt_descriptor
    ljmp $0x08, $pm32_flush
pm32_flush:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    /* Marker: yellow bar means the kernel was entered, so a hang past this
     * point is the kernel rather than the handover.  %ebx still holds the
     * bootinfo; its second field is the framebuffer, its fifth the pitch.
     * No stack exists yet, so this uses registers only. */
    mov 52(%ebx), %eax            /* lfb_high: nonzero means above 4GB, and */
    test %eax, %eax               /* nothing is mapped yet to reach it with */
    jnz 2f
    mov 4(%ebx), %edi
    test %edi, %edi
    jz 2f
    mov 16(%ebx), %eax
    imul $24, %eax, %eax          /* 24 rows down, below the loader's bars */
    add %eax, %edi
    mov $2048, %ecx
    mov $0x00FFFF00, %eax
1:  mov %eax, (%edi)
    add $4, %edi
    dec %ecx
    jnz 1b
2:
    /* clear .bss -- no stack yet, and %ebx must survive */
    mov $__bss_start, %edi
    mov $_kernel_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    cld
    rep stosb
    mov $kernel_stack_top, %esp
    mov %ebx, boot_info_ptr
    jmp enter_kmain

.code16
rm_entry:
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
    mov $__bss_start, %edi
    mov $_kernel_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    cld
    rep stosb
    mov $kernel_stack_top, %esp
    movl $BOOTINFO_ADDR, boot_info_ptr

enter_kmain:
    call kmain
1:
    hlt
    jmp 1b

.section .bss
.align 16
kernel_stack:
    .space 4096
kernel_stack_top:
.globl boot_info_ptr
boot_info_ptr:
    .space 4

.section .text.boot
.align 8
.globl gdt
gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF
    .quad 0x00CF92000000FFFF
    /* user code/data: base=0x00400000, limit=32MB-1 (must match
       USER_SPACE_SIZE in kernel.c).  G=1 so the limit field counts 4KB
       pages: 0x5FFF+1 = 0x6000 pages = 96MB.  Must match USER_SPACE_SIZE
       in kernel.c: the upper bands hold the framebuffer, device registers
       and DMA buffers, and a short limit makes those accesses fault. */
    .quad 0x00C0FA4000005FFF
    .quad 0x00C0F24000005FFF
    .quad 0x0000890000000000

gdt_descriptor:
    .word (gdt_descriptor - gdt - 1)
    .long gdt
