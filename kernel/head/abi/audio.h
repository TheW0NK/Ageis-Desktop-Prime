#ifndef AEGIS_ABI_AUDIO_H
#define AEGIS_ABI_AUDIO_H

#include <stdint.h>

// /osystem/devices/audio plays 16-bit little-endian stereo PCM written to it. One
// program (the audio server) opens it for writing at a time.

#define IOCTL_AUDIO_INFO        0x300   // arg: struct aegis_audioinfo *
#define IOCTL_AUDIO_SET_RATE    0x301   // arg: 48000 or 44100
#define IOCTL_AUDIO_VOLUME      0x302   // arg: 0..100, or -1 to only read; returns the volume
#define IOCTL_AUDIO_DRAIN       0x303   // waits until everything written has played

struct aegis_audioinfo {
    uint32_t rate, channels, bits;
    uint32_t buffer_bytes;              // the hardware ring
    uint32_t queued_bytes;              // written but not played yet
    uint32_t volume;
    uint64_t played_bytes;
    char device[48];
};

#endif
