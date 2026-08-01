OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)); /* Execute (1) + Read (4) = 5 */
    rodata  PT_LOAD    FLAGS((1 << 2));             /* Read (4) = 4 */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)); /* Write (2) + Read (4) = 6 */
}

SECTIONS
{
    /* Higher-half kernel base address for x86_64 */
    . = 0xffffffff80000000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    } :text

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    } :rodata

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    } :data

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note.*)
        *(.eh_frame)
    }
}