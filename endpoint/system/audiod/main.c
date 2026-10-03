#include "aegis.h"
#include "abi/audio.h"

// The audio server. Programs connect to "@aegis/audio" (see lib/audio.c)
// and send PCM; this mixes every stream, resampled to 48 kHz stereo with its
// own volume, into /osystem/devices/audio, and keeps the master volume.

#define RATE        48000
#define CHUNK       480                     // frames mixed at a time (10 ms)
#define TARGET      (RATE * 4 * 60 / 1000)  // bytes kept queued in the device (60 ms)
#define RING        (64 * 1024)
#define MAX_CLIENTS 32

enum { NEW, PLAY, CONTROL };

struct client {
    int fd, kind, id;
    char line[512];
    size_t line_len;
    int rate, channels, volume;
    char name[64];
    uint8_t *ring;
    size_t head, len;
    double pos;                     // fractional frame position past head
    bool eof;
};

static struct client *clients[MAX_CLIENTS];
static int dev = -1, master = 80, next_id = 1;
static bool muted;
static uint64_t last_try;
static bool tried;

static void apply_volume(void)
{
    if (dev >= 0)
        ioctl(dev, IOCTL_AUDIO_VOLUME, muted ? 0 : master);
}

static void open_device(void)
{
    if (dev >= 0 || (tried && uptime_ms() - last_try < 5000))
        return;
    tried = true;
    last_try = uptime_ms();
    if ((dev = open("/osystem/devices/audio", O_WRONLY | O_CLOEXEC)) >= 0) {
        ioctl(dev, IOCTL_AUDIO_SET_RATE, RATE);
        apply_volume();
    }
}

static void drop(int i)
{
    close(clients[i]->fd);
    free(clients[i]->ring);
    free(clients[i]);
    clients[i] = NULL;
}

static void reply(struct client *c, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(c->fd, buf, MIN(n, (int)sizeof(buf) - 1));
}

static void control_line(struct client *c, char *line)
{
    if (!strcmp(line, "get")) {
        reply(c, "volume %d mute %d\n", master, muted);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] && clients[i]->kind == PLAY)
                reply(c, "stream %d %d %s\n", clients[i]->id, clients[i]->volume, clients[i]->name);
        reply(c, "end\n");
    } else if (!strncmp(line, "volume ", 7)) {
        master = MIN(MAX(atoi(line + 7), 0), 100);
        muted = false;
        apply_volume();
        reply(c, "ok\n");
    } else if (!strncmp(line, "mute ", 5)) {
        muted = atoi(line + 5) != 0;
        apply_volume();
        reply(c, "ok\n");
    } else if (!strncmp(line, "stream ", 7)) {
        char *end;
        int id = strtol(line + 7, &end, 10), v = atoi(end);

        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] && clients[i]->id == id) {
                clients[i]->volume = MIN(MAX(v, 0), 100);
                reply(c, "ok\n");
                return;
            }
        reply(c, "error\n");
    } else {
        reply(c, "error\n");
    }
}

static void ring_put(struct client *c, const uint8_t *data, size_t n)
{
    n = MIN(n, RING - c->len);
    for (size_t i = 0; i < n; i++)
        c->ring[(c->head + c->len + i) % RING] = data[i];
    c->len += n;
}

// Reads what a client sent. Returns false when it should go.
static bool client_read(struct client *c)
{
    uint8_t buf[8192];
    size_t want = c->kind == PLAY ? MIN(sizeof(buf), RING - c->len) : sizeof(buf);
    ssize_t n;

    if (!want)
        return true;
    n = read(c->fd, buf, want);
    if (n <= 0) {
        c->eof = true;
        return c->kind == PLAY && c->len > 0;
    }
    if (c->kind == PLAY) {
        ring_put(c, buf, n);
        return true;
    }
    for (ssize_t i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (c->line_len + 1 < sizeof(c->line))
                c->line[c->line_len++] = buf[i];
            continue;
        }
        c->line[c->line_len] = 0;
        c->line_len = 0;
        if (c->kind == NEW) {
            if (!strncmp(c->line, "play ", 5)) {
                char *p = c->line + 5, *end;

                c->rate = strtol(p, &end, 10);
                c->channels = strtol(end, &end, 10);
                while (*end == ' ')
                    end++;
                strlcpy(c->name, *end ? end : "app", sizeof(c->name));
                if (c->rate < 4000 || c->rate > 192000 || c->channels < 1 || c->channels > 8
                    || !(c->ring = malloc(RING)))
                    return false;
                c->kind = PLAY;
                c->volume = 100;
                c->id = next_id++;
                // PCM may follow in the same read.
                ring_put(c, buf + i + 1, n - i - 1);
                return true;
            }
            if (!strcmp(c->line, "ctl")) {
                c->kind = CONTROL;
                continue;
            }
            return false;
        }
        control_line(c, c->line);
    }
    return true;
}

static int16_t sample(struct client *c, size_t frame, int ch)
{
    size_t at = (c->head + frame * c->channels * 2 + (size_t)MIN(ch, c->channels - 1) * 2) % RING;

    return (int16_t)(c->ring[at] | c->ring[(at + 1) % RING] << 8);
}

// Mixes CHUNK frames; returns whether any stream had sound.
static bool mix(int16_t *out)
{
    int32_t acc[CHUNK * 2] = { 0 };
    bool any = false;

    for (int k = 0; k < MAX_CLIENTS; k++) {
        struct client *c = clients[k];
        size_t frames, used;
        double step;

        if (!c || c->kind != PLAY || !c->len)
            continue;
        frames = c->len / (c->channels * 2);
        step = (double)c->rate / RATE;
        any = true;
        for (int i = 0; i < CHUNK; i++) {
            size_t idx = (size_t)c->pos;
            double frac = c->pos - idx;

            if (idx + 1 >= frames)
                break;
            for (int ch = 0; ch < 2; ch++) {
                int s0 = sample(c, idx, ch), s1 = sample(c, idx + 1, ch);

                acc[i * 2 + ch] += (int32_t)((s0 + (s1 - s0) * frac) * c->volume / 100);
            }
            c->pos += step;
        }
        used = MIN((size_t)c->pos, frames);
        c->pos -= used;
        c->head = (c->head + used * c->channels * 2) % RING;
        c->len -= used * c->channels * 2;
    }
    for (int i = 0; i < CHUNK * 2; i++)
        out[i] = (int16_t)MIN(MAX(acc[i], -32768), 32767);
    return any;
}

int main(void)
{
    int lfd = unix_listen("@aegis/audio", SOCK_STREAM);
    uint64_t clock_next = uptime_ms();

    if (lfd < 0) {
        syslog("audiod", "cannot listen: %s", strerror(errno));
        return 1;
    }
    open_device();
    syslog("audiod", dev >= 0 ? "playing to /osystem/devices/audio" : "no sound device; streams are discarded");
    for (;;) {
        struct pollfd fds[MAX_CLIENTS + 1];
        int map[MAX_CLIENTS + 1], n = 0;
        bool playing = false;

        fds[n].fd = lfd;
        fds[n].events = POLLIN;
        map[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = clients[i];

            if (!c)
                continue;
            if (c->kind == PLAY && c->len)
                playing = true;
            if (c->eof || (c->kind == PLAY && RING - c->len < 4096))
                continue;
            fds[n].fd = c->fd;
            fds[n].events = POLLIN;
            map[n++] = i;
        }
        poll(fds, n, playing ? 5 : 500);
        if (fds[0].revents & POLLIN) {
            int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);

            if (fd >= 0) {
                int slot = -1;

                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (!clients[i]) {
                        slot = i;
                        break;
                    }
                if (slot < 0 || !(clients[slot] = calloc(1, sizeof(struct client))))
                    close(fd);
                else
                    clients[slot]->fd = fd;
            }
        }
        for (int i = 1; i < n; i++)
            if (fds[i].revents && clients[map[i]] && !client_read(clients[map[i]]))
                drop(map[i]);
        // Finished players go once everything they sent has played.
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (clients[i] && clients[i]->eof && (clients[i]->kind != PLAY
                                                  || clients[i]->len < (size_t)clients[i]->channels * 4))
                drop(i);
        open_device();
        // Keep the device fed.
        for (;;) {
            int16_t chunk[CHUNK * 2];

            if (dev >= 0) {
                struct aegis_audioinfo info;

                if (ioctl(dev, IOCTL_AUDIO_INFO, (unsigned long)&info) < 0 || info.queued_bytes >= TARGET)
                    break;
            } else {
                // No device: consume at the real rate anyway.
                if (uptime_ms() < clock_next)
                    break;
                clock_next = MAX(clock_next + 10, uptime_ms() - 50);
            }
            if (!mix(chunk))
                break;
            if (dev >= 0 && write(dev, chunk, sizeof(chunk)) < 0) {
                close(dev);
                dev = -1;
            }
        }
    }
}
