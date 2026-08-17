// src/kernel/core/initcall.h
#ifndef INITCALL_H
#define INITCALL_H

#include <stdint.h>
#include <stdbool.h>

typedef int (*initcall_t)(void);

#define __init __attribute__((section(".init.text")))
#define __initdata __attribute__((section(".init.data")))

#define define_initcall(fn, id) \
    static initcall_t __initcall_##fn##_id __attribute__((used, section(".initcall" #id ".init"))) = fn

// Initcall levels corresponding to kernel boot stages
#define early_initcall(fn)      define_initcall(fn, 1)
#define arch_initcall(fn)       define_initcall(fn, 2)
#define subsys_initcall(fn)     define_initcall(fn, 3)
#define fs_initcall(fn)         define_initcall(fn, 4)
#define device_initcall(fn)     define_initcall(fn, 5)
#define late_initcall(fn)       define_initcall(fn, 6)

void do_initcalls(void);

#endif // INITCALL_H