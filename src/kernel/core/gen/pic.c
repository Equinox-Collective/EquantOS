#include "pic.h"
#include "io.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_ICW4	0x01
#define ICW1_INIT	0x10
#define ICW4_8086	0x01

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

    // Разрешаем: IRQ0 (Таймер), IRQ1 (Клава)
    outb(PIC1_DATA, 0xFC); 
    outb(PIC2_DATA, 0xFF); 
}