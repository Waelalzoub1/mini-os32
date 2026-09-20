ENTRY(start16)
SECTIONS
{
  /* @KERNEL_BASE@ is substituted by build.sh: 0x10000 for the BIOS path, where
   * the real-mode loader can reach it, and 0x200000 for UEFI.  The image is
   * over 1MB, so at 0x10000 its .bss would run through the legacy VGA aperture
   * and ROM area at 0xA0000-0xFFFFF, which is MMIO rather than RAM. */
  . = @KERNEL_BASE@;
  .text.boot : { *(.text.boot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  _kernel_end = .;
}
