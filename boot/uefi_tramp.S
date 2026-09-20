/* Leave long mode and hand control to the 32-bit kernel.
 *
 * Called after ExitBootServices, so nothing here may touch firmware services.
 * The Intel-documented order for leaving IA-32e is: enter compatibility mode
 * first (a CS with L=0), then clear CR0.PG, then EFER.LME, then CR4.PAE.  The
 * code doing that must be identity-mapped, which UEFI guarantees for the
 * loader image.
 *
 * R8-R15 do not exist once we are in compatibility mode, so every value we
 * still need is parked in a legacy register before the far jump.  ESI/EBP/EBX
 * are chosen because none of them is clobbered by the CR/MSR work that follows.
 *
 *   void tramp_boot(void *kernel_src, uint32_t size, uint32_t bootinfo);
 *   MS x64 ABI: rcx = kernel_src, rdx = size, r8 = bootinfo
 */
    .text
    .globl tramp_boot
    .code64
tramp_boot:
    cli
    mov  %rcx, %rsi              /* src      -> esi */
    mov  %edx, %ebp              /* size     -> ebp */
    mov  %r8d, %ebx              /* bootinfo -> ebx */

    /* Build the GDT pointer at run time rather than storing an absolute
     * address: an absolute .quad would emit a PE base relocation, and an EFI
     * image with no relocations at all is both simpler for firmware to accept
     * and correct at whatever base we happen to be loaded at. */
    lea  gdt32(%rip), %rax
    mov  %rax, gdt32_ptr + 2(%rip)
    lgdt gdt32_ptr(%rip)

    /* patch the far pointer with the linear address of compat32, then take a
     * far jump through the 32-bit code descriptor to reach compatibility mode */
    lea  compat32(%rip), %rax
    mov  %eax, far_ptr(%rip)
    ljmpl *far_ptr(%rip)

    .code32
compat32:
    mov  $0x10, %eax
    mov  %eax, %ds
    mov  %eax, %es
    mov  %eax, %ss
    mov  %eax, %fs
    mov  %eax, %gs

    mov  %cr0, %eax              /* paging off -> drops out of IA-32e */
    and  $0x7FFFFFFF, %eax
    mov  %eax, %cr0

    mov  $0xC0000080, %ecx       /* EFER.LME = 0 */
    rdmsr
    and  $0xFFFFFEFF, %eax
    wrmsr

    mov  %cr4, %eax              /* CR4.PAE = 0 */
    and  $0xFFFFFFDF, %eax
    mov  %eax, %cr4

    /* Paint a green bar to show the switch out of long mode worked -- on real
     * hardware this is the only evidence available.  %ebx still points at the
     * bootinfo, whose second field is the framebuffer and fifth the pitch. */
    mov  52(%ebx), %eax          /* lfb_high: a framebuffer above 4GB is out */
    test %eax, %eax              /* of reach here -- paging is off and flat */
    jnz  2f
    mov  4(%ebx), %edi           /* lfb */
    test %edi, %edi
    jz   2f
    mov  16(%ebx), %eax          /* pitch, bytes per scanline */
    imul $12, %eax, %eax         /* start 12 rows down, under the blue bar */
    add  %eax, %edi
    mov  $2048, %ecx
    mov  $0x0000FF00, %eax
1:  mov  %eax, (%edi)
    add  $4, %edi
    dec  %ecx
    jnz  1b
2:

    /* copy the kernel image to its link address and enter it.  Both constants
     * come from build.sh via --defsym so they stay in step with bootinfo.h. */
    mov  %ebp, %ecx
    mov  $KERNEL_LOAD_ADDR, %edi
    cld
    rep  movsb

    mov  $KERNEL_LOAD_ADDR + KERNEL_ENTRY32, %eax
    jmp  *%eax

    .data
    .align 8
far_ptr:
    .long 0                      /* offset, patched at run time */
    .word 0x08                   /* selector: 32-bit code */

    .align 8
gdt32:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF     /* 0x08: code32, base 0, limit 4G, L=0 D=1 */
    .quad 0x00CF92000000FFFF     /* 0x10: data32, base 0, limit 4G */
gdt32_end:

gdt32_ptr:
    .word gdt32_end - gdt32 - 1
    .quad 0                      /* base, filled in above */
