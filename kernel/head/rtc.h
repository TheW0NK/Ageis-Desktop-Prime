#ifndef AEGIS_RTC_H
#define AEGIS_RTC_H

#include "kernel.h"

struct rtc_time {
    int year;
    unsigned month, day, hour, minute, second;
};

void rtc_init(void);
int64_t rtc_now(void);
void rtc_civil(int64_t t, struct rtc_time *out);

#endif
