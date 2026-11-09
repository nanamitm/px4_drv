OUTPUT_FORMAT("elf32-fr30")
OUTPUT_ARCH(fr30)
ENTRY(telemetry_dispatch)
SECTIONS {
 . = 0x000e1610;
 .text : { *(.text*) }
 /DISCARD/ : { *(.comment) *(.note*) }
 ASSERT(. <= 0x000e1800, "telemetry exceeds saved/restored bootstrap capacity")
}
