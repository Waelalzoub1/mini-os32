ENTRY(start16)
SECTIONS
{
  . = 0x10000;
  .text.boot : { *(.text.boot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  _kernel_end = .;
}
