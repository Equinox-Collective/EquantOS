#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

/**
 * @brief Macro to trigger a kernel panic with automatic file and line tracking.
 * @param msg Descriptive error message string.
 */
#define PANIC(msg) kernel_panic(__FILE__, __LINE__, msg)

/**
 * @brief Halt the system safely due to an unrecoverable kernel error.
 * @param file Source file name where panic occurred.
 * @param line Line number in the source file.
 * @param message Detailed error description.
 */
void __attribute__((noreturn)) kernel_panic(const char *file, int line, const char *message);

#endif // PANIC_H