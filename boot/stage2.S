.code16
.globl stage2_start

.equ VBE_INFO_SEG, 0x9000
.equ VBE_INFO_OFF, 0x0000
.equ MODE_INFO_OFF, 0x0200
.equ BOOTINFO_ADDR, 0x5000
.equ FONT_ADDR, 0x6000
.equ VBE_LIST_ADDR, 0x7000
.equ VBE_LIST_MAX, 128
.equ VBE_SET_ON_BOOT, 1
.equ VBE_ENABLE, 1

stage2_start:
    cli
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov $0x7C00, %sp
    sti

    /* clear bootinfo magic by default (avoid stale VBE data) */
    xor %ax, %ax
    mov %ax, %es
    mov $BOOTINFO_ADDR, %di
    movw $0x0000, %es:(%di)
    movw $0x0000, %es:2(%di)

    mov %dl, boot_drive
    call enable_a20
    call boot_delay

    mov $VBE_ENABLE, %ax
    cmp $0x0000, %ax
    je skip_vbe
    call vbe_init
skip_vbe:
    call load_kernel
    jmp $0x1000, $0x0000

enable_a20:
    inb $0x92, %al
    orb $0x02, %al
    outb %al, $0x92
    ret

boot_delay:
    ret

vbe_init:
    /* clear bootinfo magic */
    xor %ax, %ax
    mov %ax, %es
    mov $BOOTINFO_ADDR, %di
    movw $0x0000, %es:(%di)
    movw $0x0000, %es:2(%di)
    movw $0x0000, vbe_list_count
    movw $0x0000, vbe_ok

    /* copy 8x16 BIOS font to FONT_ADDR */
    mov $0x1130, %ax
    mov $0x06, %bh          /* 8x16 font */
    int $0x10
    /* ES:BP -> font data */
    mov %es, %ax
    mov %ax, %ds
    mov %bp, %si
    mov $0x0600, %ax
    mov %ax, %es
    xor %di, %di
    mov $4096, %cx          /* 256 * 16 */
    rep movsb
    mov $0x0010, %ax
    mov %ax, font_h
    mov $0x0010, %ax
    mov %ax, font_stride

    /* VBE controller info */
    mov $VBE_INFO_SEG, %ax
    mov %ax, %es
    mov $VBE_INFO_OFF, %di
    mov $0x4F00, %ax
    int $0x10
    cmp $0x004F, %ax
    jne vbe_done
    xor %ax, %ax
    mov %ax, %ds

    mov $VBE_INFO_SEG, %ax
    mov %ax, %es
    movw %es:0x000E, %si /* mode list offset */
    movw %es:0x0010, %bx /* mode list segment */
    mov %bx, mode_list_seg
    mov %si, mode_list_off

    movw $0xFFFF, best_mode_min
    movw $0xFFFF, best_mode
    movw $0x0000, best_area_min_hi
    movw $0x0000, best_area_min_lo
    movw $0x0000, best_area_hi
    movw $0x0000, best_area_lo

    mov mode_list_seg, %ax
    mov %ax, %es
    mov mode_list_off, %si

mode_loop:
    mov %es:(%si), %cx
    cmp $0xFFFF, %cx
    je select_mode
    add $2, %si
    mov %cx, cur_mode

    mov $VBE_INFO_SEG, %ax
    mov %ax, %es
    mov $MODE_INFO_OFF, %di
    mov $0x4F01, %ax
    mov cur_mode, %cx
    int $0x10
    cmp $0x004F, %ax
    jne next_mode
    xor %ax, %ax
    mov %ax, %ds

    mov %es:MODE_INFO_OFF, %ax
    test $0x0001, %ax
    jz next_mode
    test $0x0080, %ax
    jz next_mode

    mov %es:MODE_INFO_OFF+0x19, %al
    mov %al, tmp_bpp
    mov %es:MODE_INFO_OFF+0x1B, %al
    mov %al, tmp_mem

    mov %es:MODE_INFO_OFF+0x12, %ax /* xres */
    mov %ax, tmp_w
    mov %es:MODE_INFO_OFF+0x14, %bx /* yres */
    mov %bx, tmp_h
    mov %es:MODE_INFO_OFF+0x10, %cx /* pitch */
    mov %cx, tmp_pitch
    mov %es:MODE_INFO_OFF+0x28, %dx /* lfb low */
    mov %dx, tmp_lfb_lo
    mov %es:MODE_INFO_OFF+0x2A, %dx /* lfb high */
    mov %dx, tmp_lfb_hi

    /* prefer 0x180 (1440x900x32) if available, else 0x107 */
    mov cur_mode, %ax
    cmp $0x0180, %ax
    jne check_107
    mov %ax, force_mode
    mov tmp_w, %ax
    mov %ax, force_w
    mov tmp_h, %ax
    mov %ax, force_h
    mov tmp_pitch, %ax
    mov %ax, force_pitch
    mov tmp_lfb_lo, %ax
    mov %ax, force_lfb_lo
    mov tmp_lfb_hi, %ax
    mov %ax, force_lfb_hi
    xor %ax, %ax
    mov tmp_bpp, %al
    mov %ax, force_bpp
    movw $0x0002, force_found
    jmp skip_force
check_107:
    cmp $0x0107, %ax
    jne skip_force
    cmpw $0x0000, force_found
    jne skip_force
    mov %ax, force_mode
    mov tmp_w, %ax
    mov %ax, force_w
    mov tmp_h, %ax
    mov %ax, force_h
    mov tmp_pitch, %ax
    mov %ax, force_pitch
    mov tmp_lfb_lo, %ax
    mov %ax, force_lfb_lo
    mov tmp_lfb_hi, %ax
    mov %ax, force_lfb_hi
    xor %ax, %ax
    mov tmp_bpp, %al
    mov %ax, force_bpp
    movw $0x0001, force_found
skip_force:

    /* store mode info into list (mode,w,h,bpp,pitch) */
    mov vbe_list_count, %bx
    cmp $VBE_LIST_MAX, %bx
    jae store_done
    mov %bx, %ax
    shl $3, %ax              /* 8 * count */
    mov %bx, %dx
    shl $1, %dx              /* 2 * count */
    add %dx, %ax             /* 10 * count */
    mov $VBE_LIST_ADDR, %di
    add %ax, %di
    /* mode */
    mov cur_mode, %ax
    mov %ax, (%di)
    /* w */
    mov tmp_w, %ax
    mov %ax, 2(%di)
    /* h */
    mov tmp_h, %ax
    mov %ax, 4(%di)
    /* bpp (zero-extend) */
    xor %ax, %ax
    mov tmp_bpp, %al
    mov %ax, 6(%di)
    /* pitch */
    mov tmp_pitch, %ax
    mov %ax, 8(%di)
    incw vbe_list_count
store_done:

    /* only use 8bpp packed pixel modes for boot console selection */
    mov tmp_bpp, %al
    cmp $8, %al
    jne next_mode
    mov tmp_mem, %al
    cmp $4, %al
    jne next_mode

    mov tmp_w, %ax
    mov tmp_h, %dx
    mul %dx
    mov %dx, tmp_area_hi
    mov %ax, tmp_area_lo

    mov tmp_w, %ax
    cmp $1280, %ax
    jb check_any
    mov tmp_h, %ax
    cmp $720, %ax
    jb check_any

    mov tmp_area_hi, %ax
    mov best_area_min_hi, %bx
    cmp %bx, %ax
    jb check_any
    ja update_min
    mov tmp_area_lo, %ax
    mov best_area_min_lo, %bx
    cmp %bx, %ax
    jbe check_any
update_min:
    mov tmp_area_hi, %ax
    mov %ax, best_area_min_hi
    mov tmp_area_lo, %ax
    mov %ax, best_area_min_lo
    mov cur_mode, %ax
    mov %ax, best_mode_min
    mov tmp_w, %ax
    mov %ax, best_w_min
    mov tmp_h, %ax
    mov %ax, best_h_min
    mov tmp_pitch, %ax
    mov %ax, best_pitch_min
    mov tmp_lfb_lo, %ax
    mov %ax, best_lfb_lo_min
    mov tmp_lfb_hi, %ax
    mov %ax, best_lfb_hi_min
    jmp next_mode

check_any:
    mov tmp_area_hi, %ax
    mov best_area_hi, %bx
    cmp %bx, %ax
    jb next_mode
    ja update_any
    mov tmp_area_lo, %ax
    mov best_area_lo, %bx
    cmp %bx, %ax
    jbe next_mode
update_any:
    mov tmp_area_hi, %ax
    mov %ax, best_area_hi
    mov tmp_area_lo, %ax
    mov %ax, best_area_lo
    mov cur_mode, %ax
    mov %ax, best_mode
    mov tmp_w, %ax
    mov %ax, best_w
    mov tmp_h, %ax
    mov %ax, best_h
    mov tmp_pitch, %ax
    mov %ax, best_pitch
    mov tmp_lfb_lo, %ax
    mov %ax, best_lfb_lo
    mov tmp_lfb_hi, %ax
    mov %ax, best_lfb_hi
    jmp next_mode

next_mode:
    mov mode_list_seg, %ax
    mov %ax, %es
    jmp mode_loop

select_mode:
    mov force_found, %ax
    cmp $0x0000, %ax
    je select_auto
    mov force_mode, %ax
    mov %ax, sel_mode
    mov force_w, %ax
    mov %ax, sel_w
    mov force_h, %ax
    mov %ax, sel_h
    mov force_pitch, %ax
    mov %ax, sel_pitch
    mov force_lfb_lo, %ax
    mov %ax, sel_lfb_lo
    mov force_lfb_hi, %ax
    mov %ax, sel_lfb_hi
    mov force_bpp, %ax
    mov %ax, sel_bpp
    jmp maybe_set_mode
select_auto:
    mov best_mode_min, %ax
    cmp $0xFFFF, %ax
    jne use_min
    mov best_mode, %ax
    cmp $0xFFFF, %ax
    je vbe_done
    mov %ax, sel_mode
    mov best_w, %ax
    mov %ax, sel_w
    mov best_h, %ax
    mov %ax, sel_h
    mov best_pitch, %ax
    mov %ax, sel_pitch
    mov best_lfb_lo, %ax
    mov %ax, sel_lfb_lo
    mov best_lfb_hi, %ax
    mov %ax, sel_lfb_hi
    movw $8, sel_bpp
    jmp maybe_set_mode
use_min:
    mov %ax, sel_mode
    mov best_w_min, %ax
    mov %ax, sel_w
    mov best_h_min, %ax
    mov %ax, sel_h
    mov best_pitch_min, %ax
    mov %ax, sel_pitch
    mov best_lfb_lo_min, %ax
    mov %ax, sel_lfb_lo
    mov best_lfb_hi_min, %ax
    mov %ax, sel_lfb_hi
    movw $8, sel_bpp

maybe_set_mode:
    mov $VBE_SET_ON_BOOT, %ax
    cmp $0x0000, %ax
    je write_bootinfo
    mov $0x4F02, %ax
    mov sel_mode, %bx
    or $0x4000, %bx
    int $0x10
    cmp $0x004F, %ax
    jne vbe_done
    xor %ax, %ax
    mov %ax, %ds

write_bootinfo:
    /* write bootinfo */
    xor %ax, %ax
    mov %ax, %es
    mov $BOOTINFO_ADDR, %di
    movw $0x3332, %es:(%di)  /* "23" */
    movw $0x4F53, %es:2(%di)  /* "SO" */
    mov sel_lfb_lo, %ax
    mov %ax, %es:4(%di)
    mov sel_lfb_hi, %ax
    mov %ax, %es:6(%di)
    mov sel_w, %ax
    mov %ax, %es:8(%di)
    mov $0x0000, %ax
    mov %ax, %es:10(%di)
    mov sel_h, %ax
    mov %ax, %es:12(%di)
    mov $0x0000, %ax
    mov %ax, %es:14(%di)
    mov sel_pitch, %ax
    mov %ax, %es:16(%di)
    mov $0x0000, %ax
    mov %ax, %es:18(%di)
    mov sel_bpp, %ax
    mov %ax, %es:20(%di)
    mov $0x0000, %ax
    mov %ax, %es:22(%di)
    mov $FONT_ADDR, %ax
    mov %ax, %es:24(%di)
    mov $0x0000, %ax
    mov %ax, %es:26(%di)
    mov font_h, %ax
    mov %ax, %es:28(%di)
    mov font_stride, %ax
    mov %ax, %es:30(%di)
    mov sel_mode, %ax
    mov %ax, %es:32(%di)
    mov $0x0000, %ax
    mov %ax, %es:34(%di)
    /* vbe list pointer */
    mov $VBE_LIST_ADDR, %ax
    mov %ax, %es:36(%di)
    mov $0x0000, %ax
    mov %ax, %es:38(%di)
    /* vbe list count */
    mov vbe_list_count, %ax
    mov %ax, %es:40(%di)
    mov $0x0000, %ax
    mov %ax, %es:42(%di)
    movw $0x0001, vbe_ok

vbe_done:
    mov vbe_ok, %ax
    cmp $0x0001, %ax
    je 1f
    /* fallback to text mode if VBE setup failed */
    mov $0x0003, %ax
    int $0x10
1:
    xor %ax, %ax
    mov %ax, %ds
    mov %ax, %es
    ret

load_kernel:
    mov $dap, %si
    movb $0x10, 0(%si)
    movb $0x00, 1(%si)
    movw $KERNEL_SECTORS, 2(%si)
    movw $0x0000, 4(%si)
    movw $0x1000, 6(%si)
    movl $KERNEL_LBA, 8(%si)
    movl $0x00000000, 12(%si)

    mov $0x4200, %ax
    mov $dap, %si
    mov boot_drive, %dl
    int $0x13
    jc disk_error
    ret

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

mode_list_seg: .word 0
mode_list_off: .word 0
cur_mode: .word 0

tmp_w: .word 0
tmp_h: .word 0
tmp_pitch: .word 0
tmp_lfb_lo: .word 0
tmp_lfb_hi: .word 0
tmp_area_hi: .word 0
tmp_area_lo: .word 0
tmp_bpp: .byte 0
tmp_mem: .byte 0

best_mode_min: .word 0
best_area_min_hi: .word 0
best_area_min_lo: .word 0
best_w_min: .word 0
best_h_min: .word 0
best_pitch_min: .word 0
best_lfb_lo_min: .word 0
best_lfb_hi_min: .word 0

best_mode: .word 0
best_area_hi: .word 0
best_area_lo: .word 0
best_w: .word 0
best_h: .word 0
best_pitch: .word 0
best_lfb_lo: .word 0
best_lfb_hi: .word 0

sel_mode: .word 0
sel_w: .word 0
sel_h: .word 0
sel_pitch: .word 0
sel_lfb_lo: .word 0
sel_lfb_hi: .word 0
sel_bpp: .word 0

font_h: .word 16
font_stride: .word 16
vbe_list_count: .word 0
vbe_ok: .word 0
force_mode: .word 0
force_w: .word 0
force_h: .word 0
force_pitch: .word 0
force_lfb_lo: .word 0
force_lfb_hi: .word 0
force_bpp: .word 0
force_found: .word 0
boot_msg: .ascii "Press any key to boot mini-os32...\r\n\0"
