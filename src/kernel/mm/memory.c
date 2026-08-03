#include "memory.h"
#include "stdio.h"
#include "string.h"

static block_header_t *heap_start = NULL;
size_t used_memory = 0;

static size_t align_size(size_t size) {
  return (size + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
}

void init_heap(uint64_t start_addr, size_t size) {
  uint64_t aligned_start = align_size(start_addr);
  size -= (aligned_start - start_addr);

  heap_start = (block_header_t *)aligned_start;
  heap_start->magic = HEAP_MAGIC_FREE;
  heap_start->size = size;
  heap_start->free = 1;
  heap_start->next = NULL;
  heap_start->canary = 0xDEADC0DE;

  used_memory = 0;
}

void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  block_header_t *block =
      (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));
  size_t old_data_size = block->size - sizeof(block_header_t);

  if (new_size <= old_data_size)
    return ptr; // Уже влезает

  // Выделяем новый кусок
  void *new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return NULL;

  // Копируем данные
  memcpy(new_ptr, ptr, old_data_size);

  // Освобождаем старый
  kfree(ptr);

  return new_ptr;
}

void *kmalloc(size_t size) {
  if (size == 0 || !heap_start)
    return NULL;

  // Ensure total requested size + header is strictly aligned to HEAP_ALIGNMENT (16 bytes)
  size_t total_needed = align_size(size) + sizeof(block_header_t);
  total_needed = align_size(total_needed); // Strict block alignment

  block_header_t *current = heap_start;

  while (current) {
    if (current->magic != HEAP_MAGIC_FREE &&
        current->magic != HEAP_MAGIC_ACTIVE) {
      printf("\n!!! KERNEL PANIC: HEAP CORRUPTION DETECTED AT %x !!!\n",
             (uint64_t)current);
      __asm__("cli; hlt");
    }

    if (current->free && current->size >= total_needed) {
      // Split block if remaining space is large enough
      if (current->size >= total_needed + sizeof(block_header_t) + 32) {
        block_header_t *next_node =
            (block_header_t *)((uint8_t *)current + total_needed);
        next_node->magic = HEAP_MAGIC_FREE;
        next_node->size = current->size - total_needed;
        next_node->free = 1;
        next_node->next = current->next;
        next_node->canary = 0xDEADC0DE;

        current->size = total_needed;
        current->next = next_node;
      }

      current->free = 0;
      current->magic = HEAP_MAGIC_ACTIVE;
      used_memory += current->size;

      // Return data payload strictly aligned to 16 bytes
      void *ptr = (void *)((uint8_t *)current + sizeof(block_header_t));
      return ptr;
    }
    current = current->next;
  }
  return NULL;
}

// ВАЖНО: kzalloc обнуляет память. Именно это пофиксит твой GPF!
void* kzalloc(size_t size) {
    void* ptr = kmalloc(size);
    if (ptr != NULL) {
        memset(ptr, 0, size);
    }
    return ptr; // Если NULL, вернем NULL, а не упадем
}
void kfree(void *ptr) {
  if (!ptr)
    return;
  block_header_t *block =
      (block_header_t *)((uint8_t *)ptr - sizeof(block_header_t));

  if (block->magic != HEAP_MAGIC_ACTIVE) {
    printf("KFREE: Double free or invalid magic at %x\n", (uint64_t)ptr);
    return;
  }

  block->free = 1;
  block->magic = HEAP_MAGIC_FREE;
  used_memory -= block->size;

  // Склеивание
  block_header_t *curr = heap_start;
  while (curr && curr->next) {
    if (curr->free && curr->next->free) {
      curr->size += curr->next->size;
      curr->next = curr->next->next;
    } else {
      curr = curr->next;
    }
  }
}

void kheap_dump() {
  block_header_t *current = heap_start;
  printf("--- KERNEL HEAP DUMP ---\n");
  while (current) {
    printf("Block @ %x | Size: %u bytes | Free: %d | Magic: %x\n",
           (uint64_t)current, (unsigned int)current->size, current->free, current->magic);
    current = current->next;
  }
  printf("------------------------\n");
}