#include "aegis.h"

// Talking to the audio server (/osystem/core/audiod) over "@aegis/audio".
//
// A player connects, sends "play RATE CHANNELS NAME\n" and then 16-bit
// little-endian PCM. A control connection sends "ctl\n" and then commands
// a line at a time: "get" (answers with "volume V mute M", one
// "stream ID VOLUME NAME" line per player and "end"), "volume V", "mute 0|1"
// and "stream ID VOLUME" (each answered "ok" or "error").

#define AUDIO_SOCKET "@aegis/audio"

int audio_open(const char *name, int rate, int channels)
{
    int fd = unix_connect(AUDIO_SOCKET, SOCK_STREAM);
    char clean[64];
    size_t n = 0;

    if (fd < 0)
        return -1;
    for (const char *p = name ? name : "app"; *p && n + 1 < sizeof(clean); p++)
        clean[n++] = *p == '\n' || *p == '\r' ? ' ' : *p;
    clean[n] = 0;
    if (dprintf(fd, "play %d %d %s\n", rate, channels, clean) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

ssize_t audio_write(int fd, const void *pcm, size_t bytes)
{
    const char *p = pcm;
    size_t done = 0;

    while (done < bytes) {
        ssize_t n = write(fd, p + done, bytes - done);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return done ? (ssize_t)done : -1;
        }
        done += n;
    }
    return done;
}

// "volume V mute M" at the start of a reply.
static int sscanf_volume(const char *reply, int *v, int *m)
{
    const char *p = strstr(reply, "volume ");

    if (!p)
        return -1;
    *v = atoi(p + 7);
    p = strstr(p, "mute ");
    *m = p ? atoi(p + 5) : 0;
    return 0;
}

static int control(const char *cmd, char *reply, size_t size)
{
    int fd = unix_connect(AUDIO_SOCKET, SOCK_STREAM);
    size_t got = 0;

    if (fd < 0)
        return -1;
    dprintf(fd, "ctl\n%s\n", cmd);
    reply[0] = 0;
    // Answers end with "ok", "error" or "end".
    while (got + 1 < size) {
        ssize_t n = read(fd, reply + got, size - got - 1);

        if (n <= 0)
            break;
        got += n;
        reply[got] = 0;
        if (strstr(reply, "ok\n") || strstr(reply, "error\n") || strstr(reply, "end\n"))
            break;
    }
    close(fd);
    return strstr(reply, "error\n") ? -1 : 0;
}

int audio_get_volume(bool *muted)
{
    char reply[2048];
    int v, m;

    if (control("get", reply, sizeof(reply)) < 0 || sscanf_volume(reply, &v, &m) < 0)
        return -1;
    if (muted)
        *muted = m;
    return v;
}

int audio_set_volume(int volume)
{
    char cmd[32], reply[64];

    snprintf(cmd, sizeof(cmd), "volume %d", volume);
    return control(cmd, reply, sizeof(reply));
}

int audio_set_mute(bool mute)
{
    char reply[64];

    return control(mute ? "mute 1" : "mute 0", reply, sizeof(reply));
}

int audio_streams(struct audio_stream *out, int max)
{
    char reply[4096], *line, *next;
    int n = 0;

    if (control("get", reply, sizeof(reply)) < 0)
        return -1;
    for (line = reply; *line && n < max; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        else
            next = line + strlen(line);
        if (!strncmp(line, "stream ", 7)) {
            char *p = line + 7, *end;

            out[n].id = strtol(p, &end, 10);
            out[n].volume = strtol(end, &end, 10);
            while (*end == ' ')
                end++;
            strlcpy(out[n].name, end, sizeof(out[n].name));
            n++;
        }
    }
    return n;
}

int audio_set_stream_volume(int id, int volume)
{
    char cmd[48], reply[64];

    snprintf(cmd, sizeof(cmd), "stream %d %d", id, volume);
    return control(cmd, reply, sizeof(reply));
}
