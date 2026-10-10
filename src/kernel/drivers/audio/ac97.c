// src/kernel/drivers/audio/ac97.c - Intel AC'97 audio (ICH) with an OSS /dev/dsp
//
// Playback only. The codec's PCM-out DMA engine walks a ring of 32 buffers
// forever; write(2) copies sound ahead of the hardware position and the timer
// tick follows that position, wiping what has been played so an underrun is
// silence instead of a repeat. No interrupt is used: the position is polled.
#include "ac97.h"
#include "../pci/pci.h"
#include "../serial/serial.h"
#include "../../core/gen/io.h"
#include "../../core/mem/pmm.h"
#include "../../core/mem/vmm.h"
#include "../../core/initcall.h"
#include "../../fs/devfs.h"
#include "../../misc/timer.h"
#include "../../proc/sched.h"
#include "../../proc/syscall.h"
#include "string.h"
#include "stdio.h"

// Mixer (NAM) registers
#define NAM_RESET          0x00
#define NAM_MASTER_VOL     0x02
#define NAM_PCM_VOL        0x18
#define NAM_EXT_AUDIO_ID   0x28
#define NAM_EXT_AUDIO_CTRL 0x2A
#define NAM_PCM_DAC_RATE   0x2C

// Bus master (NABM) registers; the PCM-out box starts at 0x10
#define PO_BDBAR           0x10
#define PO_CIV             0x14
#define PO_LVI             0x15
#define PO_SR              0x16
#define PO_PICB            0x18
#define PO_CR              0x1B
#define NABM_GLOB_CNT      0x2C

#define CR_RUN             0x01
#define CR_RESET           0x02
#define SR_HALTED          0x01

#define BUF_COUNT          32               // the hardware limit
#define BUF_BYTES          4096
#define RING_BYTES         (BUF_COUNT * BUF_BYTES)
#define FRAME_BYTES        4                // 16-bit stereo
// New sound is placed this far ahead of the play position when the queue is empty
#define LEAD_BYTES         BUF_BYTES
// Longest queue: the rest of the ring keeps played and unplayed data apart
#define QUEUE_MAX          (RING_BYTES - 6 * BUF_BYTES)

typedef struct {
    uint32_t addr;
    uint16_t samples;       // 16-bit samples in the buffer (both channels)
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

static bool     ac97_present = false;
static uint16_t nam_base = 0;
static uint16_t nabm_base = 0;
static uint8_t *ring = NULL;                // BUF_COUNT * BUF_BYTES of DMA memory
static int      open_count = 0;
static bool     running = false;            // DMA engine started
static bool     paused = false;             // SNDCTL_DSP_SETTRIGGER without output
static uint32_t rate = 48000;
static uint32_t write_pos = 0;              // ring offset of the next byte to queue
static uint32_t play_pos = 0;               // ring offset the hardware had reached
static int32_t  queued = 0;                 // bytes written and not played yet

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200) __asm__ volatile("sti");
}

// Ring offset the DMA engine is playing from
static uint32_t ac97_hw_pos(void) {
    uint8_t civ, again;
    uint16_t left;
    do {
        civ = inb(nabm_base + PO_CIV);
        left = inw(nabm_base + PO_PICB);        // samples left in buffer civ
        again = inb(nabm_base + PO_CIV);
    } while (civ != again);

    uint32_t left_bytes = (uint32_t)left * 2;
    if (left_bytes > BUF_BYTES) left_bytes = BUF_BYTES;
    return ((uint32_t)(civ % BUF_COUNT) * BUF_BYTES + (BUF_BYTES - left_bytes)) % RING_BYTES;
}

static void ac97_wipe(uint32_t from, uint32_t bytes) {
    while (bytes > 0) {
        uint32_t chunk = RING_BYTES - from;
        if (chunk > bytes) chunk = bytes;
        memset(ring + from, 0, chunk);
        from = (from + chunk) % RING_BYTES;
        bytes -= chunk;
    }
}

// Keeps the engine from ever reaching its "last valid" buffer
static void ac97_extend(void) {
    uint8_t civ = inb(nabm_base + PO_CIV) % BUF_COUNT;
    outb(nabm_base + PO_LVI, (uint8_t)((civ + BUF_COUNT - 1) % BUF_COUNT));
}

// Accounts for what the engine has played since the last call
static void ac97_follow(void) {
    uint32_t pos = ac97_hw_pos();
    uint32_t advanced = (pos + RING_BYTES - play_pos) % RING_BYTES;
    if (advanced == 0) return;
    ac97_wipe(play_pos, advanced);
    play_pos = pos;
    queued -= (int32_t)advanced;
    if (queued < 0) queued = 0;                 // underrun: silence was played
}

// A (re)started engine continues with the buffer after the one it stopped
// in, so up to one buffer (23 ms at 44.1 kHz) of queued sound is skipped.
static void ac97_start(void) {
    if (running || paused) return;
    ac97_extend();
    outw(nabm_base + PO_SR, 0x1C);
    outb(nabm_base + PO_CR, CR_RUN);
    running = true;
    ac97_follow();
}

static void ac97_halt(void) {
    if (!running) return;
    outb(nabm_base + PO_CR, 0);
    running = false;
    ac97_follow();
}

// Forget everything queued and silence the ring; the engine stays where it is
static void ac97_flush(void) {
    memset(ring, 0, RING_BYTES);
    queued = 0;
}

static uint32_t ac97_set_rate(uint32_t hz) {
    if (hz < 8000) hz = 8000;
    if (hz > 48000) hz = 48000;
    outw(nam_base + NAM_PCM_DAC_RATE, (uint16_t)hz);
    uint32_t got = inw(nam_base + NAM_PCM_DAC_RATE);
    rate = got ? got : 48000;
    return rate;
}

void ac97_tick(void) {
    if (!ac97_present || !running) return;

    ac97_follow();

    // If ticks were held up long enough for the engine to reach its last
    // valid buffer it has halted; moving that buffer on restarts it
    if (inw(nabm_base + PO_SR) & SR_HALTED) outw(nabm_base + PO_SR, 0x1C);
    ac97_extend();

    if (queued == 0 && open_count == 0) ac97_halt();
}

int64_t ac97_dsp_write(const void *buf, uint64_t count, bool nonblock) {
    if (!ac97_present) return -ENODEV;
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t done = 0;
    count -= count % FRAME_BYTES;

    while (done < count) {
        uint64_t flags = irq_save();
        if (queued == 0) {
            // Nothing pending: begin just ahead of where the hardware is
            ac97_start();
            if (running) ac97_follow();
            write_pos = (play_pos + LEAD_BYTES) % RING_BYTES;
            write_pos -= write_pos % FRAME_BYTES;
        }
        uint32_t space = QUEUE_MAX - (uint32_t)queued;
        uint64_t n = count - done;
        if (n > space) n = space;
        n -= n % FRAME_BYTES;
        uint64_t left = n;
        while (left > 0) {
            uint32_t chunk = RING_BYTES - write_pos;
            if (chunk > left) chunk = (uint32_t)left;
            memcpy(ring + write_pos, src + done, chunk);
            write_pos = (write_pos + chunk) % RING_BYTES;
            done += chunk;
            left -= chunk;
        }
        queued += (int32_t)n;
        irq_restore(flags);

        if (done >= count) break;
        if (nonblock) return done > 0 ? (int64_t)done : -EAGAIN;
        if (paused && n == 0) return done > 0 ? (int64_t)done : -EAGAIN;

        // Queue full: wait for the hardware to play some of it
        sched_make_sleep(current_task, tick + 2);
        sched_yield();
    }
    return (int64_t)done;
}

static int64_t ac97_fops_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)node; (void)offset;
    return ac97_dsp_write(buffer, size, false);
}

static void ac97_fops_open(vfs_node_t *node) {
    (void)node;
    if (!ac97_present) return;
    uint64_t flags = irq_save();
    if (open_count++ == 0) {
        paused = false;
        ac97_flush();
    }
    irq_restore(flags);
}

static void ac97_fops_close(vfs_node_t *node) {
    (void)node;
    if (!ac97_present) return;
    uint64_t flags = irq_save();
    if (open_count > 0 && --open_count == 0) {
        ac97_flush();
        ac97_halt();
        paused = false;
    }
    irq_restore(flags);
}

static int ac97_fops_ioctl(vfs_node_t *node, uint64_t request, void *arg) {
    (void)node;
    if (!ac97_present) return -ENODEV;
    int *value = (int *)arg;

    // musl passes the request as a sign-extended int
    switch ((uint32_t)request) {
    case SNDCTL_DSP_SPEED:
        if (!value) return -EFAULT;
        *value = (int)ac97_set_rate((uint32_t)*value);
        return 0;
    case SNDCTL_DSP_SETFMT:
        if (!value) return -EFAULT;
        *value = AFMT_S16_LE;
        return 0;
    case SNDCTL_DSP_GETFMTS:
        if (!value) return -EFAULT;
        *value = AFMT_S16_LE;
        return 0;
    case SNDCTL_DSP_CHANNELS:
        if (!value) return -EFAULT;
        *value = 2;
        return 0;
    case SNDCTL_DSP_STEREO:
        if (!value) return -EFAULT;
        *value = 1;
        return 0;
    case SNDCTL_DSP_GETBLKSIZE:
        if (!value) return -EFAULT;
        *value = BUF_BYTES;
        return 0;
    case SNDCTL_DSP_SETFRAGMENT:
    case SNDCTL_DSP_POST:
        return 0;
    case SNDCTL_DSP_GETCAPS:
        if (!value) return -EFAULT;
        *value = 0x00001000;                    // DSP_CAP_TRIGGER
        return 0;
    case SNDCTL_DSP_GETOSPACE: {
        struct { int fragments, fragstotal, fragsize, bytes; } *info = arg;
        if (!info) return -EFAULT;
        uint64_t flags = irq_save();
        int space = QUEUE_MAX - queued;
        irq_restore(flags);
        info->fragsize = BUF_BYTES;
        info->fragstotal = QUEUE_MAX / BUF_BYTES;
        info->fragments = space / BUF_BYTES;
        info->bytes = space;
        return 0;
    }
    case SNDCTL_DSP_GETODELAY: {
        if (!value) return -EFAULT;
        uint64_t flags = irq_save();
        *value = queued;
        irq_restore(flags);
        return 0;
    }
    case SNDCTL_DSP_GETTRIGGER:
        if (!value) return -EFAULT;
        *value = paused ? 0 : PCM_ENABLE_OUTPUT;
        return 0;
    case SNDCTL_DSP_SETTRIGGER: {
        if (!value) return -EFAULT;
        uint64_t flags = irq_save();
        if (*value & PCM_ENABLE_OUTPUT) {
            paused = false;
            if (queued > 0) ac97_start();
        } else {
            // The engine stops where it is; what is queued plays after resuming
            paused = true;
            ac97_halt();
        }
        irq_restore(flags);
        return 0;
    }
    case SNDCTL_DSP_RESET: {
        uint64_t flags = irq_save();
        ac97_flush();
        irq_restore(flags);
        return 0;
    }
    case SNDCTL_DSP_SYNC:
        // Let the queue play out
        while (queued > 0 && running && !paused) {
            sched_make_sleep(current_task, tick + 2);
            sched_yield();
        }
        return 0;
    default:
        return -EINVAL;
    }
}

vfs_file_operations_t ac97_dsp_fops = {
    .write = ac97_fops_write,
    .open = ac97_fops_open,
    .close = ac97_fops_close,
    .ioctl = ac97_fops_ioctl,
};

static int ac97_probe(pci_device_t *dev) {
    if (ac97_present) return 0;

    uint32_t bar0 = pci_read_dword(dev->bus, dev->slot, dev->func, 0x10);
    uint32_t bar1 = pci_read_dword(dev->bus, dev->slot, dev->func, 0x14);
    if (!(bar0 & 1) || !(bar1 & 1)) {
        serial_puts(COM1, "[AC97] Mixer/bus-master BARs are not I/O ports\n");
        return -1;
    }
    nam_base = (uint16_t)(bar0 & ~0x3u);
    nabm_base = (uint16_t)(bar1 & ~0x3u);

    // I/O decoding and bus mastering
    uint32_t command = pci_read_dword(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_word(dev->bus, dev->slot, dev->func, 0x04, (uint16_t)((command & 0xFFFF) | 0x05));

    // The ring and its descriptor list must sit below 4 GB
    void *ring_phys = pmm_alloc_continuous(BUF_COUNT);
    void *bdl_phys = pmm_alloc();
    if (!ring_phys || !bdl_phys ||
        (uint64_t)ring_phys + RING_BYTES > 0x100000000ULL || (uint64_t)bdl_phys >= 0x100000000ULL) {
        serial_puts(COM1, "[AC97] No DMA memory below 4 GB\n");
        return -1;
    }
    ring = (uint8_t *)VIRT(ring_phys);
    memset(ring, 0, RING_BYTES);
    ac97_bd_t *bdl = (ac97_bd_t *)VIRT(bdl_phys);
    for (int i = 0; i < BUF_COUNT; i++) {
        bdl[i].addr = (uint32_t)((uint64_t)ring_phys + (uint64_t)i * BUF_BYTES);
        bdl[i].samples = BUF_BYTES / 2;
        bdl[i].flags = 0;
    }

    // Leave cold reset with interrupts off, then reset the codec
    outl(nabm_base + NABM_GLOB_CNT, 0x00000002);
    outw(nam_base + NAM_RESET, 0xFFFF);
    outw(nam_base + NAM_MASTER_VOL, 0x0000);    // 0 dB, unmuted
    outw(nam_base + NAM_PCM_VOL, 0x0808);       // 0 dB, unmuted

    // Variable sample rate, when the codec has it (otherwise 48 kHz only)
    if (inw(nam_base + NAM_EXT_AUDIO_ID) & 0x0001) {
        outw(nam_base + NAM_EXT_AUDIO_CTRL, inw(nam_base + NAM_EXT_AUDIO_CTRL) | 0x0001);
    }
    ac97_set_rate(44100);

    // PCM-out engine: reset, then point it at the descriptor list
    outb(nabm_base + PO_CR, CR_RESET);
    for (int i = 0; i < 1000 && (inb(nabm_base + PO_CR) & CR_RESET); i++) {
        __asm__ volatile("pause");
    }
    outl(nabm_base + PO_BDBAR, (uint32_t)(uint64_t)bdl_phys);
    outb(nabm_base + PO_LVI, BUF_COUNT - 1);

    ac97_present = true;
    devfs_register_device("dsp", &ac97_dsp_fops, NULL, 0666);

    char msg[96];
    snprintf(msg, sizeof(msg), "[AC97] Audio ready: /dev/dsp, 16-bit stereo, %u Hz\n", rate);
    serial_puts(COM1, msg);
    return 0;
}

static pci_device_id_t ac97_pci_ids[] = {
    { 0x8086, 0x2415, 0, 0 },   // Intel 82801AA (QEMU -device AC97)
    { 0x8086, 0x2425, 0, 0 },   // Intel 82801AB
    { 0x8086, 0x2445, 0, 0 },   // Intel 82801BA
    { 0x8086, 0x24C5, 0, 0 },   // Intel 82801DB
    { 0x8086, 0x24D5, 0, 0 },   // Intel 82801EB
    { 0, 0, 0, 0 }
};

static pci_driver_t ac97_driver = {
    .name = "ac97",
    .id_table = ac97_pci_ids,
    .probe = ac97_probe,
    .remove = NULL,
    .next = NULL
};

static int __init ac97_initcall(void) {
    pci_register_driver(&ac97_driver);
    return 0;
}
device_initcall(ac97_initcall);
