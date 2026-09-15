#ifndef RANDOM_H
#define RANDOM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

void random_init(void);
uint32_t random_get_u32(void);
uint64_t random_get_u64(void);
void random_fill(void *buf, size_t len);

#endif // RANDOM_H