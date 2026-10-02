#include "aegis.h"

// Calendar time. Local time is UTC plus a fixed offset read from
// /etc/timezone ("<name> <minutes east of UTC>"), or TZ_OFFSET (minutes).

static const char *const day_names[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
                                         "Saturday" };
static const char *const month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                           "August", "September", "October", "November", "December" };

static bool leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

struct tm *gmtime_r(const int64_t *t, struct tm *tm)
{
    int64_t secs = *t, days = secs / 86400, rem = secs % 86400, era, doe, yoe, doy, mp, y;

    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm->tm_hour = rem / 3600;
    tm->tm_min = rem / 60 % 60;
    tm->tm_sec = rem % 60;
    tm->tm_wday = (int)((days % 7 + 11) % 7);     // 1970-01-01 was a Thursday
    // Days to civil date (Howard Hinnant's algorithm).
    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    tm->tm_mday = doy - (153 * mp + 2) / 5 + 1;
    tm->tm_mon = mp < 10 ? mp + 2 : mp - 10;
    y += tm->tm_mon <= 1;
    tm->tm_year = y - 1900;
    {
        static const int cum[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

        tm->tm_yday = cum[tm->tm_mon] + tm->tm_mday - 1 + (tm->tm_mon > 1 && leap(y));
    }
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}

int64_t timegm(const struct tm *tm)
{
    int64_t y = tm->tm_year + 1900, m = tm->tm_mon, era, yoe, doy, doe;

    // Normalize the month first.
    y += m / 12;
    m %= 12;
    if (m < 0) {
        m += 12;
        y--;
    }
    y -= m <= 1;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m > 1 ? m - 2 : m + 10) + 2) / 5 + tm->tm_mday - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

static char zone_name[48] = "UTC";
static int zone_offset;             // seconds east of UTC
static uint64_t zone_checked;

static void load_zone(void)
{
    uint64_t now = uptime_ms();
    const char *env = getenv("TZ_OFFSET");
    char buf[96];
    int fd;
    ssize_t n;

    // Re-read now and then so a changed setting reaches running programs.
    if (zone_checked && now - zone_checked < 10000)
        return;
    zone_checked = now ? now : 1;
    if (env) {
        zone_offset = atoi(env) * 60;
        return;
    }
    if ((fd = open("/etc/timezone", O_RDONLY)) < 0)
        return;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    {
        char *sp = strchr(buf, ' ');

        if (!sp)
            return;
        *sp = 0;
        strlcpy(zone_name, buf, sizeof(zone_name));
        zone_offset = atoi(sp + 1) * 60;
    }
}

int timezone_offset(void)
{
    load_zone();
    return zone_offset;
}

const char *timezone_name(void)
{
    load_zone();
    return zone_name;
}

struct tm *localtime_r(const int64_t *t, struct tm *tm)
{
    int64_t local;

    load_zone();
    local = *t + zone_offset;
    gmtime_r(&local, tm);
    tm->tm_gmtoff = zone_offset;
    tm->tm_zone = zone_name;
    return tm;
}

int64_t mktime(struct tm *tm)
{
    load_zone();
    return timegm(tm) - zone_offset;
}

static void put(char **o, char *end, const char *s)
{
    while (*s && *o < end)
        *(*o)++ = *s++;
}

size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    char *o = buf, *end = buf + (size ? size - 1 : 0), tmp[32];

    if (!size)
        return 0;
    for (; *fmt && o < end; fmt++) {
        if (*fmt != '%') {
            *o++ = *fmt;
            continue;
        }
        switch (*++fmt) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) % 100); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'l': snprintf(tmp, sizeof(tmp), "%d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'p': strlcpy(tmp, tm->tm_hour < 12 ? "AM" : "PM", sizeof(tmp)); break;
        case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
        case 'u': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
        case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
        case 'a': snprintf(tmp, sizeof(tmp), "%.3s", day_names[tm->tm_wday % 7]); break;
        case 'A': strlcpy(tmp, day_names[tm->tm_wday % 7], sizeof(tmp)); break;
        case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", month_names[tm->tm_mon % 12]); break;
        case 'B': strlcpy(tmp, month_names[tm->tm_mon % 12], sizeof(tmp)); break;
        case 'Z': strlcpy(tmp, tm->tm_zone ? tm->tm_zone : "UTC", sizeof(tmp)); break;
        case 'z': {
            long off = tm->tm_gmtoff / 60;

            snprintf(tmp, sizeof(tmp), "%c%02ld%02ld", off < 0 ? '-' : '+', labs(off) / 60, labs(off) % 60);
            break;
        }
        case 'F':
            snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
            break;
        case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 's': snprintf(tmp, sizeof(tmp), "%ld", (long)timegm(tm) - tm->tm_gmtoff); break;
        case 'n': strlcpy(tmp, "\n", sizeof(tmp)); break;
        case '%': strlcpy(tmp, "%", sizeof(tmp)); break;
        case 0: fmt--; tmp[0] = 0; break;
        default: snprintf(tmp, sizeof(tmp), "%%%c", *fmt);
        }
        put(&o, end, tmp);
    }
    *o = 0;
    return o - buf;
}
