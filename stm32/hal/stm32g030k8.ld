/* STM32G030K8: 64 KB flash (the last two 2 KB pages hold the saved parameters and the plug
 * record), 8 KB RAM. */
ENTRY(Reset_Handler)

MEMORY {
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 60K
  SAVED (r)  : ORIGIN = 0x0800F000, LENGTH = 4K
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 8K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS {
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) . = ALIGN(4); } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  /* At least 1 KB left for the stack. */
  ASSERT(_ebss + 1024 <= _estack, "RAM: no room left for the stack")
}
