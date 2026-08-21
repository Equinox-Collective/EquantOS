#ifndef _LINUX_CAPABILITY_H
#define _LINUX_CAPABILITY_H
#include <stdint.h>

typedef struct __user_cap_header_struct {
    uint32_t version;
    int pid;
} *cap_user_header_t;

typedef struct __user_cap_data_struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} *cap_user_data_t;

#define _LINUX_CAPABILITY_VERSION_1 0x19980330
#define _LINUX_CAPABILITY_VERSION_2 0x20071026
#define _LINUX_CAPABILITY_VERSION_3 0x20080522

#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_SYS_ADMIN       21

#endif
