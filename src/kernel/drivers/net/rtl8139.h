#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stdbool.h>

void rtl8139_send_packet(void *data, uint32_t len);
void rtl8139_poll(void);
bool rtl8139_has_data(void);

#endif // RTL8139_H