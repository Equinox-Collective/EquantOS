#include "idt.h"

idt_gate_t idt[256];
idt_register_t idt_reg;

extern uint64_t isr_stub_table[];
extern void keyboard_handler();
extern void timer_handler();
extern void mouse_handler();
extern void irq0_handler_asm();
extern void syscall_interrupt_asm();
extern void linux_syscall_interrupt_asm();

void set_idt_gate(int n, uint64_t handler, uint16_t sel) {
  idt[n].low_offset = (uint16_t)(handler & 0xFFFF);
  idt[n].sel = sel;
  idt[n].ist = 0;
  idt[n].flags = 0x8E;
  idt[n].mid_offset = (uint16_t)((handler >> 16) & 0xFFFF);
  idt[n].high_offset = (uint32_t)(handler >> 32);
  idt[n].reserved = 0;
}

void init_idt() {
  idt_reg.limit = (uint16_t)(sizeof(idt_gate_t) * 256 - 1);
  idt_reg.base = (uint64_t)&idt;

  uint16_t sel = 0x08; // <--- БЫЛО 0x28. СТАВИМ 0x08 (Kernel Code)
  for (int i = 0; i < 256; i++) { // Лучше занулить все 256
      set_idt_gate(i, isr_stub_table[i < 32 ? i : 0], sel); 
  }

  // Временные заглушки
  set_idt_gate(32, (uint64_t)timer_handler, sel); // Просто счетчик тиков
  set_idt_gate(33, (uint64_t)keyboard_handler, sel);
  set_idt_gate(44, (uint64_t)mouse_handler, sel);
  
  // Системный вызов (с разрешением для Ring 3).
  // На время boot-анимации Nyan Cat ставим ШЛЮЗ-ЛОВУШКУ (trap gate, 0xEF)
  // вместо interrupt gate (0xEE). Разница только в одном: trap gate НЕ гасит
  // флаг IF при входе в сискол, поэтому PIT-таймер продолжает тикать даже во
  // время блокирующих сисколлов (чтения с диска / serial), и анимация не
  // «замерзает». Как только GUI показал первый кадр (syscall 88), мы вернём
  // обычный interrupt gate (см. idt_set_syscall_trap_gate(0)), чтобы не менять
  // модель параллелизма ядра после загрузки.
  set_idt_gate(0x80, (uint64_t)syscall_interrupt_asm, sel);
  idt[0x80].flags = 0xEF;

  /* Этап 6a: Linux-ABI шлюз (int 0x81) для musl. Тот же ring3-доступный
   * trap gate (0xEF), что и 0x80. */
  set_idt_gate(0x81, (uint64_t)linux_syscall_interrupt_asm, sel);
  idt[0x81].flags = 0xEF;

  __asm__ __volatile__("lidt %0" : : "m"(idt_reg));
}

// Переключение шлюза сискола между trap gate (on=1, IF не гасится) и
// interrupt gate (on=0, штатное поведение). Запись в уже загруженную IDT
// действует сразу — CPU читает дескриптор при каждом int 0x80, lidt не нужен.
void idt_set_syscall_trap_gate(int on) {
  idt[0x80].flags = on ? 0xEF : 0xEE;
  /* Этап 8: Linux-шлюз 0x81 ОБЯЗАН переключаться вместе с 0x80. Раньше он
   * навсегда оставался trap gate (IF=1 внутри сисколла), и таймер вытеснял
   * процесс ПОСРЕДИ fork/execve/kmalloc/pmm_alloc. При пайплайне из двух
   * внешних команд (`ls | grep`) два процесса гонялись в аллокаторах ядра,
   * получали одну и ту же физическую страницу и падали в GPF/PF. С interrupt
   * gate сисколл на UP-ядре атомарен; блокирующие пути (пайпы, tty, sleep,
   * waitpid) явно зовут yield()/wq_sleep, а сетевые ожидания делают sti+hlt
   * сами, так что вытеснение там, где оно нужно, сохраняется. */
  idt[0x81].flags = on ? 0xEF : 0xEE;
}