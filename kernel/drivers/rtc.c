#include "rtc.h"
#include "apic.h"
#include "cpu.h"

static int64_t boot_epoch;

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static int64_t read_rtc(void)
{
    uint8_t sec, min, hour, day, mon, year, b;

    while (cmos(0x0A) & 0x80)
        ;
    sec = cmos(0x00);
    min = cmos(0x02);
    hour = cmos(0x04);
    day = cmos(0x07);
    mon = cmos(0x08);
    year = cmos(0x09);
    b = cmos(0x0B);

    if (!(b & 0x04)) {
        sec = (sec & 0x0F) + (sec >> 4) * 10;
        min = (min & 0x0F) + (min >> 4) * 10;
        hour = ((hour & 0x0F) + ((hour & 0x70) >> 4) * 10) | (hour & 0x80);
        day = (day & 0x0F) + (day >> 4) * 10;
        mon = (mon & 0x0F) + (mon >> 4) * 10;
        year = (year & 0x0F) + (year >> 4) * 10;
    }
    if (!(b & 0x02) && (hour & 0x80))
        hour = ((hour & 0x7F) + 12) % 24;

    return days_from_civil(2000 + year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

void rtc_init(void)
{
    int64_t a, b;

    do {
        a = read_rtc();
        b = read_rtc();
    } while (a != b);
    boot_epoch = a - (int64_t)(timer_uptime_ms() / 1000);
}

int64_t rtc_now(void)
{
    return boot_epoch + (int64_t)(timer_uptime_ms() / 1000);
}

void rtc_civil(int64_t t, struct rtc_time *out)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t secs = t - days * 86400;
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;

    out->day = doy - (153 * mp + 2) / 5 + 1;
    out->month = mp < 10 ? mp + 3 : mp - 9;
    out->year = (int)(yoe + era * 400) + (out->month <= 2);
    out->hour = secs / 3600;
    out->minute = secs % 3600 / 60;
    out->second = secs % 60;
}
