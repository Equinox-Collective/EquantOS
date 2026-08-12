// block.h - Generic Block Device Abstraction Layer for EquantOS
#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>

typedef int (*block_read_fn)(uint64_t lba, uint32_t count, void *buffer);
typedef int (*block_write_fn)(uint64_t lba, uint32_t count, void *buffer);

typedef struct {
    block_read_fn read;
    block_write_fn write;
    uint32_t sector_size;
} block_device_t;

#endif // BLOCK_H