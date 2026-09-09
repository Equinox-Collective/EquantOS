// src/kernel/core/uevent.c - Kernel Uevent Dispatcher
#include "uevent.h"
#include "../drivers/serial/serial.h"
#include "string.h"
#include "stdio.h"

void uevent_broadcast(kobject_action_t action, const char *subsystem, const char *devname, int major, int minor) {
    const char *act_str = (action == KOBJ_ADD) ? "add" : (action == KOBJ_REMOVE ? "remove" : "change");
    char uevent_msg[256];
    snprintf(uevent_msg, sizeof(uevent_msg),
             "[UDEV-HOTPLUG] %s@/devices/virtual/%s/%s ACTION=%s DEVNAME=%s MAJOR=%d MINOR=%d\n",
             act_str, subsystem, devname, act_str, devname, major, minor);
    serial_puts(COM1, uevent_msg);
}

void uevent_init(void) {
    serial_puts(COM1, "[UDEV] Kernel Device Manager and Uevent Subsystem Initialized.\n");
}