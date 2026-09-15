// src/kernel/misc/rtc.c - Real-Time Clock (CMOS MC146818) Driver & Unix Epoch Calculator
#include "rtc.h"
#include "../core/gen/io.h"
#include "../core/initcall.h"
#include "../drivers/serial/serial.h"
#include "stdio.h"

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static uint64_t boot_epoch_seconds = 0;

static inline int cmos_read(int reg) {
    outb(CMOS_ADDRESS, (uint8_t)reg);
    return inb(CMOS_DATA);
}

static inline bool rtc_is_updating(void) {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd / 16) * 10) + (bcd & 0x0F);
}

static bool is_leap_year(uint32_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static const int days_per_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static uint64_t datetime_to_unix(const rtc_time_t *t) {
    uint64_t days = 0;

    for (uint32_t y = 1970; y < t->year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }

    for (int m = 1; m < t->month; m++) {
        days += days_per_month[m - 1];
        if (m == 2 && is_leap_year(t->year)) days += 1;
    }

    days += (t->day - 1);

    uint64_t seconds = days * 86400ULL;
    seconds += (uint64_t)t->hour * 3600ULL;
    seconds += (uint64_t)t->minute * 60ULL;
    seconds += (uint64_t)t->second;

    return seconds;
}

void rtc_get_datetime(rtc_time_t *out) {
    while (rtc_is_updating());

    uint8_t second = (uint8_t)cmos_read(0x00);
    uint8_t minute = (uint8_t)cmos_read(0x02);
    uint8_t hour   = (uint8_t)cmos_read(0x04);
    uint8_t day    = (uint8_t)cmos_read(0x07);
    uint8_t month  = (uint8_t)cmos_read(0x08);
    uint8_t year   = (uint8_t)cmos_read(0x09);
    uint8_t status_b = (uint8_t)cmos_read(0x0B);

    // Convert BCD to binary if required
    if (!(status_b & 0x04)) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = ((hour & 0x7F) ? bcd_to_bin(hour & 0x7F) : 0) | (hour & 0x80);
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }

    // Convert 12h to 24h format if needed
    if (!(status_b & 0x02) && (hour & 0x80)) {
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    out->second = second;
    out->minute = minute;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    out->year   = 2000 + year; // Assumes 21st century
}

void rtc_init(void) {
    rtc_time_t t;
    rtc_get_datetime(&t);
    boot_epoch_seconds = datetime_to_unix(&t);

    char buf[128];
    snprintf(buf, sizeof(buf), "[RTC] Hardware Clock: %04u-%02u-%02u %02u:%02u:%02u UTC (Epoch: %llu)\n",
             t.year, t.month, t.day, t.hour, t.minute, t.second, boot_epoch_seconds);
    serial_puts(COM1, buf);
}

uint64_t rtc_get_unix_timestamp(void) {
    extern volatile uint32_t tick;
    return boot_epoch_seconds + (tick / 100);
}

static int __init rtc_initcall(void) {
    rtc_init();
    return 0;
}
core_initcall(rtc_initcall);