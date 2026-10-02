#include "aegis.h"
#include "sound.h"
#include <math.h>

// play FILE...         plays WAV, Ogg Vorbis or MP3 files
// play --tone HZ SECS  plays a sine wave
// play --volume [N]    shows or sets the master volume

static int tone(double hz, double secs)
{
    int fd = audio_open("play", 48000, 2);
    int16_t buf[480 * 2];
    long total = (long)(secs * 48000), done = 0;

    if (fd < 0) {
        perror("play: the audio server");
        return 1;
    }
    while (done < total) {
        int n = (int)MIN(480, total - done);

        for (int i = 0; i < n; i++) {
            int16_t v = (int16_t)(sin(2 * M_PI * hz * (done + i) / 48000) * 12000);

            buf[i * 2] = buf[i * 2 + 1] = v;
        }
        if (audio_write(fd, buf, n * 4) < 0)
            break;
        done += n;
    }
    close(fd);
    printf("play: done\n");
    return 0;
}

static int file(const char *path)
{
    char error[256];
    struct sound *s = sound_open(path, error, sizeof(error));
    int16_t buf[4096 * 8];
    size_t n;
    int fd;

    if (!s) {
        dprintf(STDERR_FILENO, "play: %s\n", error);
        return 1;
    }
    printf("play: %s, %s, %d Hz, %d channel%s, %.1f s%s%s\n", path, sound_format(s), sound_rate(s),
           sound_channels(s), sound_channels(s) == 1 ? "" : "s", sound_length(s), *sound_title(s) ? ": " : "",
           sound_title(s));
    if ((fd = audio_open(path, sound_rate(s), sound_channels(s))) < 0) {
        perror("play: the audio server");
        sound_close(s);
        return 1;
    }
    while ((n = sound_read(s, buf, 4096)) > 0)
        if (audio_write(fd, buf, n * sound_channels(s) * 2) < 0)
            break;
    close(fd);
    sound_close(s);
    printf("play: done\n");
    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;

    if (argc >= 4 && !strcmp(argv[1], "--tone"))
        return tone(strtod(argv[2], NULL), strtod(argv[3], NULL));
    if (argc >= 2 && !strcmp(argv[1], "--volume")) {
        bool muted;
        int v;

        if (argc > 2 && audio_set_volume(atoi(argv[2])) < 0) {
            perror("play: volume");
            return 1;
        }
        if ((v = audio_get_volume(&muted)) < 0) {
            perror("play: volume");
            return 1;
        }
        printf("volume %d%s\n", v, muted ? " (muted)" : "");
        return 0;
    }
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: play FILE... | --tone HZ SECONDS | --volume [0-100]\n");
        return 2;
    }
    for (int i = 1; i < argc; i++)
        ret |= file(argv[i]);
    return ret;
}
