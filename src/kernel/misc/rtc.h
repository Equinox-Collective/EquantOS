#ifndef RTC_H
#define RTC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} rtc_time_t;

void rtc_init(void);
uint64_t rtc_get_unix_timestamp(void);
void rtc_get_datetime(rtc_time_t *out);

#endif // RTC_H