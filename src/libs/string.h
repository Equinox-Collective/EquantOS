// src/libs/string.h - String and Integer Parsing Declarations
#ifndef LIBC_STRING_H
#define LIBC_STRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Memory Functions
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);

// String Functions
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
void *memmove(void *dest, const void *src, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
char *strstr(const char *haystack, const char *needle);
char *strcat(char *dest, const char *src);
char *strtok(char *s, const char *delim);
char *strpbrk(const char *s1, const char *s2);
size_t strspn(const char *s1, const char *s2);
size_t strcspn(const char *s1, const char *s2);
char *strrchr(const char *s, int c);
char *strchr(const char *s, int c);

// Number <-> String Conversion
void itoa(int64_t num, int base, char *buffer);
void itoa_hex(uint64_t num, char *buffer);
int atoi(const char *str);
int64_t atoll(const char *str);
char *strsep(char **stringp, const char *delim);

#endif // LIBC_STRING_H