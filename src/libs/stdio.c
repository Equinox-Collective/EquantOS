#include "stdio.h"
#include "string.h"

// Нам нужно знать про функцию терминала (она в kernel.c)
extern void term_print(const char *str);

int vsprintf(char *buffer, const char *format, va_list args) {
  char *ptr = buffer;
  const char *f = format;
  char temp_buf[64];

  while (*f) {
    if (*f != '%') {
      *ptr++ = *f++;
      continue;
    }

    f++; // Пропускаем '%'

    int width = 0;
    char pad = ' ';
    if (*f == '0') {
      pad = '0';
      f++;
    }
    while (*f >= '0' && *f <= '9') {
      width = width * 10 + (*f - '0');
      f++;
    }

    if (*f == 'u' || *f == 'd' || *f == 'x' || *f == 'p') {
      unsigned long long val;
      if (*f == 'p') {
          val = va_arg(args, unsigned long long);
          itoa_hex(val, temp_buf);
      } else if (*f == 'x') {
          val = va_arg(args, unsigned int);
          itoa_hex(val, temp_buf);
      } else {
          val = va_arg(args, unsigned int);
          itoa(val, 10, temp_buf);
      }

      int len = strlen(temp_buf);
      // Добавляем набивку (padding), если число короче нужной ширины
      while (len < width) {
        *ptr++ = pad;
        len++;
      }

      char *t = temp_buf;
      while (*t)
        *ptr++ = *t++;
    } else if (*f == 's') {
      char *s = va_arg(args, char *);
      if (!s)
        s = "(null)";
      while (*s)
        *ptr++ = *s++;
    } else if (*f == 'c') {
      *ptr++ = (char)va_arg(args, int);
    } else if (*f == '%') {
      *ptr++ = '%';
    }

    if (*f)
      f++; // Пропускаем символ формата (u, d, x и т.д.), если мы еще не в конце
           // строки
  }

  *ptr = '\0';
  return (int)(ptr - buffer);
}
int sprintf(char *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int len = vsprintf(buffer, format, args);
    va_end(args);
    return len;
}

// ГЛАВНЫЙ БОСС
void printf(const char *format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsprintf(buffer, format, args);
    va_end(args);

    // Вместо term_print(buffer) мы могли бы делать:
    // vfs_node_t* tty = vfs_find("tty0");
    // vfs_write(tty, 0, strlen(buffer), (uint8_t*)buffer);

    // Но пока для простоты оставим мост, но назовем его правильно:
    term_print(buffer);
}