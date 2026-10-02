#include "sound.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

// Decoders for the Audio Player and play(1). The whole file is read into
// memory; MP3 and WAV are walked frame by frame, Vorbis through stb_vorbis.

enum { FMT_WAV, FMT_OGG, FMT_MP3 };

struct sound {
    int format, rate, channels;
    uint8_t *data;
    size_t len;
    uint64_t frames_total;          // 0 if unknown
    uint64_t frame;                 // next frame to read
    char title[128], artist[128];
    // WAV
    size_t pcm_start, pcm_len;
    int bits, block, wav_float;
    // Vorbis
    stb_vorbis *vorbis;
    // MP3
    mp3dec_t mp3;
    size_t mp3_start, mp3_pos;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int pcm_frames, pcm_used;
};

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

static void set_text(char *out, size_t size, const char *s, size_t len)
{
    size_t n = MIN(len, size - 1);

    memcpy(out, s, n);
    out[n] = 0;
    while (n && (out[n - 1] == 0 || out[n - 1] == ' '))
        out[--n] = 0;
}

// ---- WAV ----

static bool open_wav(struct sound *s, char *error, size_t size)
{
    size_t p = 12;
    int fmt = 0;

    while (p + 8 <= s->len) {
        uint32_t id = le32(s->data + p), clen = le32(s->data + p + 4);
        const uint8_t *c = s->data + p + 8;

        if (p + 8 + clen > s->len)
            clen = s->len - p - 8;
        if (!memcmp(s->data + p, "fmt ", 4) && clen >= 16) {
            fmt = le16(c);
            s->channels = le16(c + 2);
            s->rate = le32(c + 4);
            s->block = le16(c + 12);
            s->bits = le16(c + 14);
            if (fmt == 0xFFFE && clen >= 26)
                fmt = le16(c + 24);
        } else if (!memcmp(s->data + p, "data", 4)) {
            s->pcm_start = p + 8;
            s->pcm_len = clen;
        } else if (!memcmp(s->data + p, "LIST", 4) && clen > 4 && !memcmp(c, "INFO", 4)) {
            for (size_t q = 4; q + 8 <= clen;) {
                uint32_t sl = le32(c + q + 4);

                if (q + 8 + sl > clen)
                    break;
                if (!memcmp(c + q, "INAM", 4))
                    set_text(s->title, sizeof(s->title), (const char *)c + q + 8, sl);
                else if (!memcmp(c + q, "IART", 4))
                    set_text(s->artist, sizeof(s->artist), (const char *)c + q + 8, sl);
                q += 8 + sl + (sl & 1);
            }
        }
        (void)id;
        p += 8 + clen + (clen & 1);
    }
    if (!s->pcm_start || !s->channels || !s->rate || !s->block) {
        snprintf(error, size, "The WAV file has no sound in it.");
        return false;
    }
    s->wav_float = fmt == 3;
    if ((fmt != 1 && fmt != 3) || (s->bits != 8 && s->bits != 16 && s->bits != 24 && s->bits != 32)) {
        snprintf(error, size, "This kind of WAV file (format %d, %d-bit) is not supported.", fmt, s->bits);
        return false;
    }
    s->frames_total = s->pcm_len / s->block;
    return true;
}

static size_t read_wav(struct sound *s, int16_t *out, size_t frames)
{
    size_t n = MIN(frames, s->frames_total - s->frame);
    int bytes = s->bits / 8;

    for (size_t f = 0; f < n; f++) {
        const uint8_t *p = s->data + s->pcm_start + (s->frame + f) * s->block;

        for (int c = 0; c < s->channels; c++, p += bytes) {
            int32_t v;

            switch (s->bits) {
            case 8:
                v = (p[0] - 128) << 8;
                break;
            case 16:
                v = (int16_t)le16(p);
                break;
            case 24:
                v = ((int32_t)(p[0] << 8 | p[1] << 16 | (uint32_t)p[2] << 24)) >> 16;
                break;
            default:
                if (s->wav_float) {
                    union {
                        uint32_t u;
                        float f;
                    } x = { le32(p) };

                    v = (int32_t)(MIN(MAX(x.f, -1.0f), 1.0f) * 32767);
                } else {
                    v = (int32_t)le32(p) >> 16;
                }
            }
            out[f * s->channels + c] = (int16_t)v;
        }
    }
    s->frame += n;
    return n;
}

// ---- MP3 ----

// ID3v2 at the start: skip it, keeping the title and artist.
static size_t id3(struct sound *s)
{
    size_t size, p;
    int version;

    if (s->len < 10 || memcmp(s->data, "ID3", 3))
        return 0;
    version = s->data[3];
    size = (s->data[6] & 0x7F) << 21 | (s->data[7] & 0x7F) << 14 | (s->data[8] & 0x7F) << 7 | (s->data[9] & 0x7F);
    for (p = 10; p + 10 < 10 + size && p + 10 < s->len;) {
        const uint8_t *f = s->data + p;
        uint32_t fl = version >= 4 ? ((f[4] & 0x7F) << 21 | (f[5] & 0x7F) << 14 | (f[6] & 0x7F) << 7 | (f[7] & 0x7F))
                                   : ((uint32_t)f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
        char *target = !memcmp(f, "TIT2", 4) ? s->title : !memcmp(f, "TPE1", 4) ? s->artist : NULL;

        if (!f[0] || p + 10 + fl > s->len)
            break;
        // Text frames: an encoding byte, then the text (Latin-1 or UTF-8 here).
        if (target && fl > 1 && (f[10] == 0 || f[10] == 3)) {
            if (f[10] == 3) {
                set_text(target, 128, (const char *)f + 11, fl - 1);
            } else {
                char buf[128];
                size_t o = 0;

                for (uint32_t i = 1; i < fl && o + 3 < sizeof(buf); i++) {
                    uint8_t c = f[10 + i];

                    if (c < 0x80) {
                        buf[o++] = c;
                    } else {
                        buf[o++] = 0xC0 | c >> 6;
                        buf[o++] = 0x80 | (c & 0x3F);
                    }
                }
                set_text(target, 128, buf, o);
            }
        }
        p += 10 + fl;
    }
    return 10 + size;
}

static bool decode_mp3_frame(struct sound *s)
{
    mp3dec_frame_info_t info;

    while (s->mp3_pos < s->len) {
        int samples = mp3dec_decode_frame(&s->mp3, s->data + s->mp3_pos, (int)MIN(s->len - s->mp3_pos, 1 << 20),
                                          s->pcm, &info);

        if (!info.frame_bytes)
            return false;
        s->mp3_pos += info.frame_bytes;
        if (samples) {
            if (info.channels != s->channels) {
                // Mixed channel counts: make it the file's.
                if (info.channels == 1 && s->channels == 2)
                    for (int i = samples - 1; i >= 0; i--)
                        s->pcm[i * 2] = s->pcm[i * 2 + 1] = s->pcm[i];
            }
            s->pcm_frames = samples;
            s->pcm_used = 0;
            return true;
        }
    }
    return false;
}

static bool open_mp3(struct sound *s, char *error, size_t size)
{
    mp3dec_frame_info_t info;
    int16_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t pos, frames = 0, bytes = 0;
    int samples = 0;

    s->mp3_start = id3(s);
    mp3dec_init(&s->mp3);
    // Find the first frame and its format.
    for (pos = s->mp3_start; pos < s->len;) {
        samples = mp3dec_decode_frame(&s->mp3, s->data + pos, (int)MIN(s->len - pos, 1 << 20), scratch, &info);
        if (!info.frame_bytes)
            break;
        if (samples) {
            s->rate = info.hz;
            s->channels = info.channels;
            break;
        }
        pos += info.frame_bytes;
    }
    if (!s->rate) {
        snprintf(error, size, "This is not an MP3 file this player can read.");
        return false;
    }
    s->mp3_start = pos;
    // The length, by counting frame headers (fast: no decoding).
    for (size_t p = pos; p + 4 <= s->len && frames < 1000000;) {
        const uint8_t *h = s->data + p;
        int fb = hdr_frame_bytes(h, 0);

        if (!hdr_valid(h) || fb <= 0) {
            p++;
            continue;
        }
        bytes += hdr_frame_samples(h);
        frames++;
        p += fb + hdr_padding(h);
    }
    s->frames_total = bytes;
    mp3dec_init(&s->mp3);
    s->mp3_pos = s->mp3_start;
    return true;
}

static size_t read_mp3(struct sound *s, int16_t *out, size_t frames)
{
    size_t done = 0;

    while (done < frames) {
        size_t n;

        if (s->pcm_used >= s->pcm_frames && !decode_mp3_frame(s))
            break;
        n = MIN(frames - done, (size_t)(s->pcm_frames - s->pcm_used));
        memcpy(out + done * s->channels, s->pcm + s->pcm_used * s->channels, n * s->channels * sizeof(int16_t));
        s->pcm_used += n;
        done += n;
    }
    s->frame += done;
    return done;
}

// ---- Common ----

struct sound *sound_open_memory(void *data, size_t len, char *error, size_t size)
{
    struct sound *s = calloc(1, sizeof(*s));
    bool ok = false;

    if (!s) {
        free(data);
        snprintf(error, size, "Out of memory.");
        return NULL;
    }
    s->data = data;
    s->len = len;
    if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp((uint8_t *)data + 8, "WAVE", 4)) {
        s->format = FMT_WAV;
        ok = open_wav(s, error, size);
    } else if (len >= 4 && !memcmp(data, "OggS", 4)) {
        int err;

        s->format = FMT_OGG;
        if ((s->vorbis = stb_vorbis_open_memory(data, (int)len, &err, NULL))) {
            stb_vorbis_info info = stb_vorbis_get_info(s->vorbis);
            stb_vorbis_comment c = stb_vorbis_get_comment(s->vorbis);

            s->rate = info.sample_rate;
            s->channels = info.channels;
            s->frames_total = stb_vorbis_stream_length_in_samples(s->vorbis);
            for (int i = 0; i < c.comment_list_length; i++) {
                if (!strncasecmp(c.comment_list[i], "TITLE=", 6))
                    strlcpy(s->title, c.comment_list[i] + 6, sizeof(s->title));
                else if (!strncasecmp(c.comment_list[i], "ARTIST=", 7))
                    strlcpy(s->artist, c.comment_list[i] + 7, sizeof(s->artist));
            }
            ok = s->rate > 0;
        } else {
            snprintf(error, size, "The Ogg file could not be read (error %d).", err);
        }
    } else {
        s->format = FMT_MP3;
        ok = open_mp3(s, error, size);
    }
    if (!ok) {
        sound_close(s);
        return NULL;
    }
    return s;
}

struct sound *sound_open(const char *path, char *error, size_t size)
{
    struct aegis_stat st;
    uint8_t *data;
    size_t got = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        snprintf(error, size, "%s cannot be opened: %s", path, strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) < 0 || st.size > (256u << 20) || !(data = malloc(st.size + 1))) {
        close(fd);
        snprintf(error, size, "%s is too large.", path);
        return NULL;
    }
    while (got < st.size) {
        ssize_t n = read(fd, data + got, st.size - got);

        if (n <= 0)
            break;
        got += n;
    }
    close(fd);
    return sound_open_memory(data, got, error, size);
}

int sound_rate(const struct sound *s) { return s->rate; }
int sound_channels(const struct sound *s) { return s->channels; }
const char *sound_title(const struct sound *s) { return s->title; }
const char *sound_artist(const struct sound *s) { return s->artist; }

const char *sound_format(const struct sound *s)
{
    return s->format == FMT_WAV ? "WAV" : s->format == FMT_OGG ? "Ogg Vorbis" : "MP3";
}

double sound_length(const struct sound *s)
{
    return s->rate ? (double)s->frames_total / s->rate : 0;
}

double sound_position(const struct sound *s)
{
    return s->rate ? (double)s->frame / s->rate : 0;
}

size_t sound_read(struct sound *s, int16_t *out, size_t frames)
{
    switch (s->format) {
    case FMT_WAV:
        return read_wav(s, out, frames);
    case FMT_OGG: {
        int n = stb_vorbis_get_samples_short_interleaved(s->vorbis, s->channels, out,
                                                         (int)(frames * s->channels));

        s->frame += n;
        return n;
    }
    default:
        return read_mp3(s, out, frames);
    }
}

int sound_seek(struct sound *s, double seconds)
{
    uint64_t target = (uint64_t)(MAX(seconds, 0) * s->rate);

    if (s->frames_total && target > s->frames_total)
        target = s->frames_total;
    switch (s->format) {
    case FMT_WAV:
        s->frame = target;
        return 0;
    case FMT_OGG:
        if (!stb_vorbis_seek(s->vorbis, (unsigned)target))
            return -1;
        s->frame = target;
        return 0;
    default: {
        // Jump close by size, then decode up to the frame.
        int16_t scratch[MINIMP3_MAX_SAMPLES_PER_FRAME];

        mp3dec_init(&s->mp3);
        s->mp3_pos = s->mp3_start;
        s->frame = 0;
        s->pcm_frames = s->pcm_used = 0;
        if (s->frames_total) {
            double f = (double)target / s->frames_total;
            size_t pos = s->mp3_start + (size_t)(f * (s->len - s->mp3_start));

            // Back a little, so the decoder finds the next frame header.
            if (pos > s->mp3_start + 4096) {
                s->mp3_pos = pos - 4096;
                s->frame = (uint64_t)((double)(s->mp3_pos - s->mp3_start) / (s->len - s->mp3_start) * s->frames_total);
            }
        }
        while (s->frame + MINIMP3_MAX_SAMPLES_PER_FRAME / 2 < target) {
            size_t n = read_mp3(s, scratch, MIN(target - s->frame, (uint64_t)MINIMP3_MAX_SAMPLES_PER_FRAME / 2));

            if (!n)
                break;
        }
        return 0;
    }
    }
}

void sound_close(struct sound *s)
{
    if (!s)
        return;
    if (s->vorbis)
        stb_vorbis_close(s->vorbis);
    free(s->data);
    free(s);
}
