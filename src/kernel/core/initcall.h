// src/kernel/core/initcall.h - Kernel Subsystem Initcall Levels
#ifndef INITCALL_H
#define INITCALL_H

#include <stdint.h>

// Compiler annotations for boot-time code/data
#ifndef __init
#define __init
#endif

#ifndef __initdata
#define __initdata
#endif

typedef int (*initcall_t)(void);

#define __define_initcall(fn, id) \
    static initcall_t __initcall_##fn##_id __attribute__((used, section(".initcall" #id ".init"))) = fn

// Standard Linux-style Initcall Levels
#define pure_initcall(fn)       __define_initcall(fn, 1)
#define core_initcall(fn)       __define_initcall(fn, 2)
#define arch_initcall(fn)       __define_initcall(fn, 3)
#define subsys_initcall(fn)     __define_initcall(fn, 4)
#define fs_initcall(fn)         __define_initcall(fn, 5)
#define device_initcall(fn)     __define_initcall(fn, 6)

// Dedicated Initcall for USB Drivers & Stack
#define usb_initcall(fn)        __define_initcall(fn, 6)

#define late_initcall(fn)       __define_initcall(fn, 7)
#define module_initcall(fn)     __define_initcall(fn, 7)

void do_initcalls(void);

#endif // INITCALL_H