#ifndef ELF_H
#define ELF_H
#include <stdint.h>

// Заголовок всего ELF-файла
typedef struct {
    uint8_t  e_ident[16];  // Магические символы (0x7F 'E' 'L' 'F')
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;      // Точка входа (куда прыгать)
    uint64_t e_phoff;      // Смещение до Program Headers
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;      // Количество Program Headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

// Заголовок сегмента программы (Program Header)
typedef struct {
    uint32_t p_type;       // 1 = загружаемый сегмент (PT_LOAD)
    uint32_t p_flags;
    uint64_t p_offset;     // Где данные лежат в файле
    uint64_t p_vaddr;      // Куда их надо скопировать в памяти
    uint64_t p_paddr;
    uint64_t p_filesz;     // Размер в файле
    uint64_t p_memsz;      // Размер в памяти (может быть больше для переменных)
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

#endif