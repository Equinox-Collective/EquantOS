// src/kernel/drivers/input/evdev.c - Production Linux Evdev and PS/2 Mousedev Adapter
#include "evdev.h"
#include "../../core/mem/memory.h"
#include "../../proc/task.h"
#include "../../proc/sched.h"
#include "../../proc/syscall.h"
#include "../serial/serial.h"
#include "../../misc/timer.h"
#include "string.h"

static evdev_device_t ev_keyboard;
static evdev_device_t ev_mouse;

// PS/2 Packet Stream for /dev/input/mice and /dev/mouse
static uint8_t mouse_ps2_buf[MOUSE_BUF_SIZE];
static uint32_t mouse_ps2_head = 0;
static uint32_t mouse_ps2_tail = 0;
static uint8_t mouse_buttons_state = 0;
static struct task *mousedev_blocked_reader = NULL;

static void mousedev_enqueue_byte(uint8_t b) {
    uint32_t next = (mouse_ps2_head + 1) % MOUSE_BUF_SIZE;
    if (next != mouse_ps2_tail) {
        mouse_ps2_buf[mouse_ps2_head] = b;
        mouse_ps2_head = next;
    }
}

// Convert input subsystem movement and clicks to 3-byte Standard PS/2 packets
static void mousedev_process_event(uint16_t type, uint16_t code, int32_t value) {
    static int8_t pending_dx = 0;
    static int8_t pending_dy = 0;
    static bool has_movement = false;

    if (type == EV_KEY) {
        if (code == BTN_LEFT) {
            if (value) mouse_buttons_state |= 0x01;
            else       mouse_buttons_state &= ~0x01;
        } else if (code == BTN_RIGHT) {
            if (value) mouse_buttons_state |= 0x02;
            else       mouse_buttons_state &= ~0x02;
        } else if (code == BTN_MIDDLE) {
            if (value) mouse_buttons_state |= 0x04;
            else       mouse_buttons_state &= ~0x04;
        }

        // Generate instant 3-byte PS/2 packet on button press/release
        uint8_t flags = 0x08 | (mouse_buttons_state & 0x07);
        mousedev_enqueue_byte(flags);
        mousedev_enqueue_byte(0);
        mousedev_enqueue_byte(0);

        if (mousedev_blocked_reader) {
            sched_unblock(mousedev_blocked_reader);
            mousedev_blocked_reader = NULL;
        }
    } else if (type == EV_REL) {
        if (code == REL_X) {
            pending_dx += (int8_t)value;
            has_movement = true;
        } else if (code == REL_Y) {
            // PS/2 Y axis goes up, screen coordinates go down: invert delta
            pending_dy -= (int8_t)value;
            has_movement = true;
        }
    } else if (type == EV_SYN && code == SYN_REPORT && has_movement) {
        uint8_t flags = 0x08 | (mouse_buttons_state & 0x07);
        if (pending_dx < 0) flags |= 0x10;
        if (pending_dy < 0) flags |= 0x20;

        mousedev_enqueue_byte(flags);
        mousedev_enqueue_byte((uint8_t)pending_dx);
        mousedev_enqueue_byte((uint8_t)pending_dy);

        pending_dx = 0;
        pending_dy = 0;
        has_movement = false;

        if (mousedev_blocked_reader) {
            sched_unblock(mousedev_blocked_reader);
            mousedev_blocked_reader = NULL;
        }
    }
}

static void evdev_device_push(evdev_device_t *dev, uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (dev->head + 1) % EVDEV_BUFFER_SIZE;
    if (next == dev->tail) return; // Buffer full, discard

    struct linux_input_event *ev = &dev->ring[dev->head];
    uint64_t current_ticks = tick;
    ev->time.tv_sec  = current_ticks / 100;
    ev->time.tv_usec = (current_ticks % 100) * 10000;
    ev->type  = type;
    ev->code  = code;
    ev->value = value;

    dev->head = next;

    if (dev->blocked_reader) {
        sched_unblock(dev->blocked_reader);
        dev->blocked_reader = NULL;
    }
}

void evdev_push_event(uint16_t type, uint16_t code, int32_t value) {
    if (type == EV_KEY && code < BTN_LEFT) {
        evdev_device_push(&ev_keyboard, type, code, value);
        evdev_device_push(&ev_keyboard, EV_SYN, SYN_REPORT, 0);
    } else {
        evdev_device_push(&ev_mouse, type, code, value);
        if (type != EV_SYN) {
            evdev_device_push(&ev_mouse, EV_SYN, SYN_REPORT, 0);
        }
        mousedev_process_event(type, code, value);
        if (type != EV_SYN) {
            mousedev_process_event(EV_SYN, SYN_REPORT, 0);
        }
    }
}

// ----------------------------------------------------------------------------
// /dev/input/event* Operations
// ----------------------------------------------------------------------------
static int64_t evdev_read_common(evdev_device_t *dev, void *buf, size_t count, bool nonblock) {
    if (!dev || !buf || count < sizeof(struct linux_input_event)) return -EINVAL;

    size_t events_to_read = count / sizeof(struct linux_input_event);
    size_t read_bytes = 0;
    struct linux_input_event *out = (struct linux_input_event *)buf;

    while (read_bytes == 0) {
        while (dev->head != dev->tail && (read_bytes / sizeof(struct linux_input_event)) < events_to_read) {
            out[read_bytes / sizeof(struct linux_input_event)] = dev->ring[dev->tail];
            dev->tail = (dev->tail + 1) % EVDEV_BUFFER_SIZE;
            read_bytes += sizeof(struct linux_input_event);
        }

        if (read_bytes > 0) return (int64_t)read_bytes;
        if (nonblock) return -EAGAIN;

        if (current_task) {
            dev->blocked_reader = current_task;
            sched_block(current_task);
            sched_yield();
            dev->blocked_reader = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    return (int64_t)read_bytes;
}

static int64_t evdev_kbd_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset; (void)node;
    bool nonblock = (current_task && current_task->process && 
                    (current_task->process->file_flags[alloc_fd(node, 0)] & O_NONBLOCK));
    return evdev_read_common(&ev_keyboard, buffer, size, nonblock);
}

static int64_t evdev_mouse_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset; (void)node;
    bool nonblock = (current_task && current_task->process && 
                    (current_task->process->file_flags[alloc_fd(node, 0)] & O_NONBLOCK));
    return evdev_read_common(&ev_mouse, buffer, size, nonblock);
}

static int evdev_ioctl_common(evdev_device_t *dev, uint64_t req, void *arg) {
    if (!dev || !arg) return -EFAULT;

    if (req == EVIOCGVERSION) {
        *(int *)arg = 0x010001; // evdev protocol version 1.0.1
        return 0;
    }

    if (req == EVIOCGID) {
        struct input_id *id = (struct input_id *)arg;
        id->bustype = dev->bustype;
        id->vendor  = dev->vendor;
        id->product = dev->product;
        id->version = dev->version;
        return 0;
    }

    // Return device identification name (e.g. EVIOCGNAME)
    if ((req & 0xFFFF) == (EVIOCGNAME(0) & 0xFFFF)) {
        size_t len = (req >> 16) & 0x1FFF;
        strncpy((char *)arg, dev->name, len - 1);
        ((char *)arg)[len - 1] = '\0';
        return (int)strlen((char *)arg);
    }

    // Feature bitmasks for Xorg / Kdrive capabilities
    if ((req & 0xFF) == 0x20) { // EVIOCGBIT(0) - Supported event types
        uint8_t *bits = (uint8_t *)arg;
        memset(bits, 0, 8);
        bits[0] |= (1 << EV_SYN) | (1 << EV_KEY);
        if (dev->is_mouse) {
            bits[0] |= (1 << EV_REL);
        }
        return 0;
    }

    if ((req & 0xFF) == (0x20 + EV_REL) && dev->is_mouse) { // EVIOCGBIT(EV_REL)
        uint8_t *bits = (uint8_t *)arg;
        memset(bits, 0, 8);
        bits[0] |= (1 << REL_X) | (1 << REL_Y) | (1 << REL_WHEEL);
        return 0;
    }

    if ((req & 0xFF) == (0x20 + EV_KEY)) { // EVIOCGBIT(EV_KEY)
        uint8_t *bits = (uint8_t *)arg;
        memset(bits, 0, 64);
        if (dev->is_mouse) {
            bits[BTN_LEFT / 8] |= (1 << (BTN_LEFT % 8));
            bits[BTN_RIGHT / 8] |= (1 << (BTN_RIGHT % 8));
            bits[BTN_MIDDLE / 8] |= (1 << (BTN_MIDDLE % 8));
        } else {
            // Report all regular alphanumeric keys supported
            for (int k = 1; k < 120; k++) {
                bits[k / 8] |= (1 << (k % 8));
            }
        }
        return 0;
    }

    return 0;
}

static int evdev_kbd_ioctl(vfs_node_t *node, uint64_t req, void *arg) {
    (void)node;
    return evdev_ioctl_common(&ev_keyboard, req, arg);
}

static int evdev_mouse_ioctl(vfs_node_t *node, uint64_t req, void *arg) {
    (void)node;
    return evdev_ioctl_common(&ev_mouse, req, arg);
}

// ----------------------------------------------------------------------------
// /dev/input/mice and /dev/mouse (Raw 3-byte PS/2 Stream)
// ----------------------------------------------------------------------------
static int64_t mousedev_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    (void)offset; (void)node;
    if (!buffer || size == 0) return 0;

    bool nonblock = false;
    if (current_task && current_task->process) {
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (current_task->process->files[i] == node) {
                if (current_task->process->file_flags[i] & O_NONBLOCK) nonblock = true;
                break;
            }
        }
    }

    while (mouse_ps2_head == mouse_ps2_tail) {
        if (nonblock) return -EAGAIN;
        if (current_task) {
            mousedev_blocked_reader = current_task;
            sched_block(current_task);
            sched_yield();
            mousedev_blocked_reader = NULL;
        } else {
            __asm__ volatile("pause");
        }
    }

    size_t count = 0;
    while (mouse_ps2_head != mouse_ps2_tail && count < size) {
        buffer[count++] = mouse_ps2_buf[mouse_ps2_tail];
        mouse_ps2_tail = (mouse_ps2_tail + 1) % MOUSE_BUF_SIZE;
    }

    return (int64_t)count;
}

bool evdev_mouse_has_data(void) {
    return (mouse_ps2_head != mouse_ps2_tail) || (ev_mouse.head != ev_mouse.tail);
}

bool evdev_mouse_can_read(void) {
    return (mouse_ps2_head != mouse_ps2_tail);
}

vfs_file_operations_t g_evdev_kbd_fops = {
    .read = evdev_kbd_read,
    .write = NULL,
    .ioctl = evdev_kbd_ioctl
};

vfs_file_operations_t g_evdev_mouse_fops = {
    .read = evdev_mouse_read,
    .write = NULL,
    .ioctl = evdev_mouse_ioctl
};

vfs_file_operations_t g_mousedev_fops = {
    .read = mousedev_read,
    .write = NULL,
    .ioctl = NULL
};

void evdev_init(void) {
    memset(&ev_keyboard, 0, sizeof(evdev_device_t));
    strcpy(ev_keyboard.name, "EquantOS Virtual Keyboard");
    ev_keyboard.bustype = 0x03; // USB
    ev_keyboard.vendor  = 0x1234;
    ev_keyboard.product = 0x5678;
    ev_keyboard.version = 0x0100;
    ev_keyboard.is_mouse = false;

    memset(&ev_mouse, 0, sizeof(evdev_device_t));
    strcpy(ev_mouse.name, "EquantOS USB/PS2 Mouse");
    ev_mouse.bustype = 0x03; // USB
    ev_mouse.vendor  = 0x1234;
    ev_mouse.product = 0x9ABC;
    ev_mouse.version = 0x0100;
    ev_mouse.is_mouse = true;

    serial_puts(COM1, "[EVDEV] Linux Event Device & PS/2 Mousedev Subsystem Ready.\n");
}