// src/kernel/equant_version.h - Single source of truth for user-visible OS/kernel identity
#ifndef EQUANT_VERSION_H
#define EQUANT_VERSION_H

// Distribution identity (/etc/os-release)
#define EQUANT_OS_NAME          "EquantOS"
#define EQUANT_OS_ID            "equantos"
#define EQUANT_OS_VERSION       "0.0.1 Alpha"
#define EQUANT_OS_VERSION_ID    "0.0.1"

// Kernel identity (uname(2), /proc/version)
#define EQUANT_KERNEL_RELEASE   "1.0.0-equantos"
#define EQUANT_KERNEL_VERSION   "EquantOS Unix Microkernel x86_64"
#define EQUANT_HOSTNAME         "equant"

#endif // EQUANT_VERSION_H
