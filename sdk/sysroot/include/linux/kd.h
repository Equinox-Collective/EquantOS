#ifndef _LINUX_KD_H
#define _LINUX_KD_H

#ifndef KIOCSOUND
#define KIOCSOUND   0x4B2F
#endif

#ifndef KDGKBMODE
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45
#define K_RAW       0x00
#define K_XLATE     0x01
#define K_MEDIUMRAW 0x02
#define K_UNICODE   0x03
#define K_OFF       0x04
#endif

#include <bits/kd.h>
#endif
