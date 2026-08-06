#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/**
 * @brief Remap 8259 PIC interrupts to avoid CPU exception vector collisions.
 * Master IRQs -> 32-39, Slave IRQs -> 40-47.
 */
void pic_remap(void);

#endif // PIC_H