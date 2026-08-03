#ifndef MEMORY_H
#define MEMORY_H

#include <stddef.h>
#include <stdint.h>

#define HEAP_MAGIC_ACTIVE 0x414C4F43 // "ALOC"
#define HEAP_MAGIC_FREE 0x46524545   // "FREE"
#define HEAP_ALIGNMENT 16

typedef struct block_header {
    uint32_t magic; // Магическое число для проверки целостности
    size_t size;    // Полный размер блока (вместе с заголовком)
    uint8_t free;
    struct block_header *next;
    uint32_t canary; // Канарейка в конце заголовка
} block_header_t;

void init_heap(uint64_t start_addr, size_t size);
void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t new_size);
void *kzalloc(size_t size); // Новая функция: выделяет и обнуляет
void kfree(void *ptr);
void kheap_dump(); // Дебаг-функция для проверки кучи

#endif