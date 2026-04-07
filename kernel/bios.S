.code32
.globl bios_vbe_get_mode_info
.globl bios_vbe_set_mode
.globl bios_vbe_get_controller_info
.globl bios_set_video_mode
.globl bios_int10
.extern gdt_descriptor

.equ KERNEL_BASE, 0x10000
.equ KERNEL_SEG,  0x1000
.equ RM_SEG,      0x0900
.equ RM_BUF,      0x00009000
.equ RM_AX,       0
.equ RM_BX,       2
.equ RM_CX,       4
.equ RM_DX,       6
.equ RM_SI,       8
.equ RM_DI,       10
.equ RM_ES,       12

.section .bss
.align 4
bios_pm_esp: .long 0
rm_far_ptr: .word 0, 0
idtr_saved: .word 0, 0, 0

.section .text
.code32
bios_vbe_get_mode_info:
    push %ebp
    mov %esp, %ebp
    mov 8(%ebp), %eax       /* mode */
    movw %ax, RM_BUF+RM_CX
    mov 12(%ebp), %eax      /* buf */
    mov %eax, %edx
    and $0xF, %edx          /* offset */
    shr $4, %eax            /* segment */
    movw %ax, RM_BUF+RM_ES
    movw %dx, RM_BUF+RM_DI
    movw $0x4F01, RM_BUF+RM_AX
    movw $0x0000, RM_BUF+RM_BX
    movw $0x0000, RM_BUF+RM_DX
    movw $0x0000, RM_BUF+RM_SI
    call bios_int10
    movzwl RM_BUF+RM_AX, %eax
    pop %ebp
    ret

bios_vbe_set_mode:
    push %ebp
    mov %esp, %ebp
    mov 8(%ebp), %eax       /* mode (already includes LFB bit) */
    movw %ax, RM_BUF+RM_BX
    movw $0x4F02, RM_BUF+RM_AX
    movw $0x0000, RM_BUF+RM_CX
    movw $0x0000, RM_BUF+RM_DX
    movw $0x0000, RM_BUF+RM_SI
    movw $0x0000, RM_BUF+RM_DI
    movw $0x0000, RM_BUF+RM_ES
    call bios_int10
    movzwl RM_BUF+RM_AX, %eax
    pop %ebp
    ret

bios_vbe_get_controller_info:
    push %ebp
    mov %esp, %ebp
    mov 8(%ebp), %eax       /* buf */
    mov %eax, %edx
    and $0xF, %edx          /* offset */
    shr $4, %eax            /* segment */
    movw %ax, RM_BUF+RM_ES
    movw %dx, RM_BUF+RM_DI
    movw $0x4F00, RM_BUF+RM_AX
    movw $0x0000, RM_BUF+RM_BX
    movw $0x0000, RM_BUF+RM_CX
    movw $0x0000, RM_BUF+RM_DX
    movw $0x0000, RM_BUF+RM_SI
    call bios_int10
    movzwl RM_BUF+RM_AX, %eax
    pop %ebp
    ret

bios_set_video_mode:
    push %ebp
    mov %esp, %ebp
    mov 8(%ebp), %eax       /* mode */
    movw %ax, RM_BUF+RM_AX
    movw $0x0000, RM_BUF+RM_BX
    movw $0x0000, RM_BUF+RM_CX
    movw $0x0000, RM_BUF+RM_DX
    movw $0x0000, RM_BUF+RM_SI
    movw $0x0000, RM_BUF+RM_DI
    movw $0x0000, RM_BUF+RM_ES
    call bios_int10
    movzwl RM_BUF+RM_AX, %eax
    pop %ebp
    ret

bios_int10:
    pushfl
    cli
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    sidt idtr_saved
    mov %esp, bios_pm_esp

    mov $real_mode_entry, %eax
    sub $KERNEL_BASE, %eax
    movw %ax, rm_far_ptr
    movw $KERNEL_SEG, rm_far_ptr+2
    mov %cr0, %eax
    and $0xFFFFFFFE, %eax
    mov %eax, %cr0
    ljmpw *rm_far_ptr

.code16
real_mode_entry:
    mov $RM_SEG, %ax
    mov %ax, %ds
    mov $0x9000, %ax
    mov %ax, %ss
    mov $0xFF00, %sp
    mov $0x0000, %ax
    mov %ax, %ds
    movw $0x03FF, 0x0500
    movl $0x00000000, 0x0502
    lidt 0x0500
    mov $RM_SEG, %ax
    mov %ax, %ds

    mov RM_AX, %ax
    mov RM_BX, %bx
    mov RM_CX, %cx
    mov RM_DX, %dx
    mov RM_SI, %si
    mov RM_DI, %di
    mov RM_ES, %ax
    mov %ax, %es

    int $0x10

    /* save BIOS return AX safely */
    mov %ax, %bx
    mov $RM_SEG, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %bx, RM_AX

    cli
    mov %cr0, %eax
    or $0x1, %eax
    mov %eax, %cr0
    ljmpl $0x08, $prot_mode_entry

.code32
prot_mode_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    lidt idtr_saved
    mov bios_pm_esp, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    popfl
    ret
