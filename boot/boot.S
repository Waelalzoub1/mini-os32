.code16
.globl _start

_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7c00, %sp
    sti

    /* force VGA text mode (80x25) */
    mov $0x0003, %ax
    int $0x10

    mov %dl, boot_drive

    /* load stage2 to 0x0000:0x7E00 */
    mov $dap, %si
    movb $0x10, 0(%si)
    movb $0x00, 1(%si)
    movw $STAGE2_SECTORS, 2(%si)
    movw $0x7E00, 4(%si)
    movw $0x0000, 6(%si)
    movl $0x00000001, 8(%si)
    movl $0x00000000, 12(%si)

    mov $0x4200, %ax
    mov $dap, %si
    mov boot_drive, %dl
    int $0x13
    jc disk_error

    jmp $0x0000, $0x7E00

disk_error:
    mov $err_msg, %si
1:
    lodsb
    test %al, %al
    jz 2f
    mov $0x0e, %ah
    mov $0x0007, %bx
    int $0x10
    jmp 1b
2:
    cli
    hlt
    jmp 2b

boot_drive:
    .byte 0

err_msg:
    .ascii "DISK"
    .byte 0

dap:
    .space 16

.org 510
.word 0xAA55
