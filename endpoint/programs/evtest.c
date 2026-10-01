#include "aegis.h"
#include "abi/input.h"

// Prints events from /dev/input. Usage: evtest [COUNT]
// Reads until COUNT events have arrived (default: forever, Ctrl+C to stop).

static const char *type_name(uint16_t type)
{
    switch (type) {
    case EV_SYN: return "SYN";
    case EV_KEY: return "KEY";
    case EV_REL: return "REL";
    case EV_ABS: return "ABS";
    }
    return "?";
}

static const char *axis_name(uint16_t type, uint16_t code)
{
    static const char *const rel[] = { "X", "Y", "WHEEL", "HWHEEL" };
    static const char *const abs[] = { "X", "Y" };

    if (type == EV_REL && code < 4)
        return rel[code];
    if (type == EV_ABS && code < 2)
        return abs[code];
    return "?";
}

int main(int argc, char **argv)
{
    long limit = argc > 1 ? strtol(argv[1], NULL, 10) : -1;
    struct input_event ev[32];
    int fd = open("/dev/input", O_RDONLY);
    long count = 0;
    int n;

    if (fd < 0) {
        perror("evtest: /dev/input");
        return 1;
    }
    printf("evtest: %ld input device(s)\n", ioctl(fd, IOCTL_INPUT_DEVICES, 0));
    while (limit < 0 || count < limit) {
        ssize_t got = read(fd, ev, sizeof(ev));

        if (got <= 0)
            break;
        n = got / sizeof(ev[0]);
        for (int i = 0; i < n && (limit < 0 || count < limit); i++) {
            if (ev[i].type == EV_SYN)
                continue;
            count++;
            if (ev[i].type == EV_KEY)
                printf("dev %u KEY 0x%03x %s mods=0x%03x\n", ev[i].device, ev[i].code,
                       ev[i].value == 0 ? "up" : ev[i].value == 1 ? "down" : "repeat",
                       ev[i].modifiers);
            else
                printf("dev %u %s %s %d\n", ev[i].device, type_name(ev[i].type),
                       axis_name(ev[i].type, ev[i].code), ev[i].value);
        }
    }
    close(fd);
    return 0;
}
