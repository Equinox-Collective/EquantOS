// src/system/pic.c - УБЕДИСЬ, ЧТО ЭТОТ КОД АКТУАЛЕН!
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4	0x01
#define ICW1_SINGLE	0x02
#define ICW1_INTERVAL4	0x04
#define ICW1_LEVEL	0x08
#define ICW1_INIT	0x10
 
#define ICW4_8086	0x01
#define ICW4_AUTO	0x02
#define ICW4_BUF_SLAVE	0x08
#define ICW4_BUF_MASTER	0x0C
#define ICW4_SFNM	0x10

void pic_remap() {
    // ICW1 - Начало инициализации
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); 
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    // ICW2 - Переназначение векторов
    outb(PIC1_DATA, 0x20); // Master IRQs 0-7 -> IDT 32-39
    outb(PIC2_DATA, 0x28); // Slave IRQs 8-15 -> IDT 40-47

    // ICW3 - Каскадирование
    outb(PIC1_DATA, 0x04); // Master: Slave на IRQ2
    outb(PIC2_DATA, 0x02); // Slave: Его ID 2

    // ICW4 - Режим работы
    outb(PIC1_DATA, ICW4_8086); 
    outb(PIC2_DATA, ICW4_8086); 

    // ЖЕСТКАЯ МАСКА (0 - разрешено, 1 - запрещено)
    // Разрешаем: IRQ0 (Таймер), IRQ1 (Клава), IRQ2 (Каскад)
    // В двоичном: 1111 1000 = 0xF8
    outb(PIC1_DATA, 0xF8); 

    // Разрешаем: IRQ12 (Мышь)
    // В двоичном: 1110 1111 = 0xEF
    outb(PIC2_DATA, 0xEF); 
}