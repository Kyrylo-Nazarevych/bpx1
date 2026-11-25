/* BPX-1 bench runtime: memory map for Spike. One 16 MiB RAM region at 0x80000000 (inside Spike's
   default memory): code first, then constants, data, the HTIF words, .bss; the stack grows down
   from the end of the region. Only .text holds instructions; rvtrace dumps it as prog.text. */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 16M }
SECTIONS
{
  .text : { KEEP(*(.text.init)) *(.text .text.*) . = ALIGN(4); } > RAM
  .rodata : ALIGN(16) { *(.rodata .rodata.*) *(.srodata .srodata.*) } > RAM
  .data : ALIGN(16) {
    *(.data .data.*)
    __global_pointer$ = . + 0x800;
    *(.sdata .sdata.*)
  } > RAM
  .tohost : ALIGN(64) { KEEP(*(.tohost)) } > RAM
  .bss (NOLOAD) : ALIGN(16) {
    __bss_start = .;
    *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  } > RAM
  __stack_top = ORIGIN(RAM) + LENGTH(RAM);
  /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
