#include "string.h"

// Копирует блок памяти из src в dest
void *memcpy(void *dest, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  // Пока n >= 8 и оба указателя выровнены по 8 байт, копируем быстро
  if (((uintptr_t)d % 8 == 0) && ((uintptr_t)s % 8 == 0)) {
    while (n >= 8) {
      *(uint64_t *)d = *(const uint64_t *)s;
      d += 8;
      s += 8;
      n -= 8;
    }
  }

  // Остаток (или если не выровнено) - побайтово
  while (n--) {
    *d++ = *s++;
  }

  return dest;
}
// Заполняет блок памяти одним байтом (например, нулями)
void *memset(void *s, int c, size_t n) {
  uint8_t *p = (uint8_t *)s;
  for (size_t i = 0; i < n; i++) {
    p[i] = (uint8_t)c;
  }
  return s;
}

// Считает длину строки
size_t strlen(const char *s) {
  size_t len = 0;
  while (s[len])
    len++;
  return len;
}

// Сравнивает две строки (возвращает 0, если они равны)
int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  while (n && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  if (n == 0)
    return 0;
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

// Копирует строку
char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return (char *)haystack;
  const char *p1 = haystack;
  while (*p1) {
    const char *p1_begin = p1;
    const char *p2 = needle;
    while (*p1 && *p2 && *p1 == *p2) {
      p1++;
      p2++;
    }
    if (!*p2)
      return (char *)p1_begin;
    p1 = p1_begin + 1;
  }
  return (char *)0; // NULL
}

// --- СИСТЕМНАЯ МАГИЯ: ПРЕВРАЩАЕМ ЧИСЛА В ТЕКСТ ---

// Перевод обычного числа (base=10) в строку
void itoa(int64_t num, int base, char *buffer) {
  int i = 0;
  bool is_negative = false;
  uint64_t value;

  if (num == 0) {
    buffer[i++] = '0';
    buffer[i] = '\0';
    return;
  }

  if (num < 0 && base == 10) {
    is_negative = true;
    // ВАЖНО: для INT64_MIN значение `-num` даёт переполнение в int64_t (UB).
    // Считаем абсолютное значение через unsigned-арифметику, у которой
    // overflow определён.
    value = (uint64_t)(-(num + 1)) + 1ULL;
  } else {
    value = (uint64_t)num;
  }

  // Получаем цифры в обратном порядке
  while (value != 0) {
    uint64_t rem = value % (uint64_t)base;
    buffer[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
    value = value / (uint64_t)base;
  }

  if (is_negative)
    buffer[i++] = '-';
  buffer[i] = '\0';

  // Разворачиваем строку
  int start = 0;
  int end = i - 1;
  while (start < end) {
    char temp = buffer[start];
    buffer[start] = buffer[end];
    buffer[end] = temp;
    start++;
    end--;
  }
}

// Перевод адресов памяти в 16-ричный формат (для дебага и паник-скрина)
void itoa_hex(uint64_t num, char *buffer) {
  int i = 0;
  if (num == 0) {
    buffer[i++] = '0';
    buffer[i] = '\0';
    return;
  }
  while (num != 0) {
    int rem = num % 16;
    buffer[i++] = (rem > 9) ? (rem - 10) + 'A' : rem + '0';
    num = num / 16;
  }
  buffer[i] = '\0';

  // Разворот строки
  int start = 0, end = i - 1;
  while (start < end) {
    char temp = buffer[start];
    buffer[start] = buffer[end];
    buffer[end] = temp;
    start++;
    end--;
  }
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = s1, *p2 = s2;
  while (n--) {
    if (*p1 != *p2) {
      return *p1 - *p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

char *strcat(char *dest, const char *src) {
  char *ptr = dest + strlen(dest);
  while (*src) {
    *ptr++ = *src++;
  }
  *ptr = '\0';
  return dest;
}

static char *strtok_last = NULL;

char *strtok(char *s, const char *delim) {
  if (!s)
    s = strtok_last;
  if (!s)
    return NULL;

  // Пропускаем ведущие разделители
  while (*s && strpbrk(s, delim) == s) {
    s++;
  }

  if (!*s) {
    strtok_last = NULL;
    return NULL;
  }

  char *start = s;
  // Ищем конец токена
  while (*s && strpbrk(s, delim) != s) {
    s++;
  }

  if (*s) {
    *s = '\0';
    strtok_last = s + 1;
  } else {
    strtok_last = NULL;
  }

  return start;
}

// Вспомогательная функция для strtok
char *strpbrk(const char *s1, const char *s2) {
  while (*s1) {
    for (const char *p = s2; *p; p++) {
      if (*s1 == *p)
        return (char *)s1;
    }
    s1++;
  }
  return NULL;
}

char *strrchr(const char *s, int c) {
    char *last = NULL;
    do {
        if (*s == (char)c) {
            last = (char *)s;
        }
    } while (*s++);
    return last;
}

char *strsep(char **stringp, const char *delim) {
    char *start = *stringp;
    char *ptr;
    if (start == NULL)
        return NULL;
    for (ptr = start; *ptr; ptr++) {
        for (const char *d = delim; *d; d++) {
            if (*ptr == *d) {
                *ptr++ = '\0';
                *stringp = ptr;
                return start;
            }
        }
    }
    *stringp = NULL;
    return start;
}