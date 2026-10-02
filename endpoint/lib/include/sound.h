#ifndef AEGIS_SOUND_H
#define AEGIS_SOUND_H

#include "aegis.h"

// Decoding sound files: WAV, Ogg Vorbis and MP3. Samples come out as
// interleaved 16-bit PCM at the file's own rate.

struct sound;

struct sound *sound_open(const char *path, char *error, size_t size);
struct sound *sound_open_memory(void *data, size_t len, char *error, size_t size);  // takes data
int sound_rate(const struct sound *s);
int sound_channels(const struct sound *s);
double sound_length(const struct sound *s);         // seconds (0 if unknown)
// Reads up to `frames` frames; returns how many (0 at the end).
size_t sound_read(struct sound *s, int16_t *out, size_t frames);
int sound_seek(struct sound *s, double seconds);
double sound_position(const struct sound *s);
// From tags when the file has them, else "".
const char *sound_title(const struct sound *s);
const char *sound_artist(const struct sound *s);
const char *sound_format(const struct sound *s);     // "WAV", "Ogg Vorbis", "MP3"
void sound_close(struct sound *s);

#endif
