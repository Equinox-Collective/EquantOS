// src/kernel/drivers/audio/ac97.h - Intel AC'97 audio (ICH) with an OSS /dev/dsp
#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stdbool.h>
#include "../../fs/vfs.h"

// OSS ioctls understood by /dev/dsp (values of <sys/soundcard.h>)
#define SNDCTL_DSP_RESET      0x00005000
#define SNDCTL_DSP_SYNC       0x00005001
#define SNDCTL_DSP_SPEED      0xC0045002
#define SNDCTL_DSP_STEREO     0xC0045003
#define SNDCTL_DSP_GETBLKSIZE 0xC0045004
#define SNDCTL_DSP_SETFMT     0xC0045005
#define SNDCTL_DSP_CHANNELS   0xC0045006
#define SNDCTL_DSP_POST       0x00005008
#define SNDCTL_DSP_SETFRAGMENT 0xC004500A
#define SNDCTL_DSP_GETFMTS    0x8004500B
#define SNDCTL_DSP_GETOSPACE  0x8010500C
#define SNDCTL_DSP_GETCAPS    0x8004500F
#define SNDCTL_DSP_SETTRIGGER 0x40045010
#define SNDCTL_DSP_GETTRIGGER 0x80045010
#define SNDCTL_DSP_GETODELAY  0x80045017

#define AFMT_S16_LE           0x00000010
#define PCM_ENABLE_OUTPUT     0x00000002

extern vfs_file_operations_t ac97_dsp_fops;

// write(2) on /dev/dsp: 16-bit little-endian stereo frames at the rate set
// with SNDCTL_DSP_SPEED. Blocks until everything is queued unless nonblock.
int64_t ac97_dsp_write(const void *buf, uint64_t count, bool nonblock);

// Called on every timer interrupt: follows the hardware play position
void ac97_tick(void);

#endif // AC97_H
