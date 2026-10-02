#!/usr/bin/env python3
"""Synthesizes short sample tunes as WAV files (for the Music folder).

    mkmusic.py OUTPUT.wav TUNE        TUNE: theme, morning or evening
"""
import math
import struct
import sys

RATE = 44100

TUNES = {
    # (title, tempo in beats per minute, notes as (MIDI note or None, beats))
    'theme': ('Aegis Theme', 112, [(64, 1), (67, 1), (71, 1), (76, 2), (74, 1), (71, 1), (72, 2), (69, 1), (72, 1),
                                   (76, 2), (74, 1), (72, 1), (71, 3), (None, 1), (64, 1), (67, 1), (71, 1), (76, 2),
                                   (79, 1), (78, 1), (76, 4)]),
    'morning': ('Morning', 96, [(60, 1), (64, 1), (67, 1), (72, 2), (71, 1), (67, 1), (69, 2), (65, 1), (69, 1),
                                (72, 2), (71, 1), (69, 1), (67, 4)]),
    'evening': ('Evening', 80, [(57, 2), (60, 1), (64, 1), (62, 2), (59, 2), (60, 1), (57, 1), (55, 2), (57, 4)]),
}


def freq(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def render(tempo, notes):
    beat = 60.0 / tempo
    out = []
    for i, (note, beats) in enumerate(notes):
        n = int(beats * beat * RATE)
        bass = notes[i - i % 4][0] if notes[i - i % 4][0] else 48
        for k in range(n):
            t = k / RATE
            env = min(1.0, t * 40) * math.exp(-t * 2.2)
            v = 0.0
            if note:
                f = freq(note)
                v += env * (0.5 * math.sin(2 * math.pi * f * t) + 0.18 * math.sin(4 * math.pi * f * t)
                            + 0.07 * math.sin(6 * math.pi * f * t))
            fb = freq(bass - 24)
            v += 0.22 * min(1.0, t * 20) * math.sin(2 * math.pi * fb * t)
            out.append(v)
    return out


def write_wav(path, title, samples):
    pcm = bytearray()
    for i, v in enumerate(samples):
        pan = 0.08 * math.sin(i / RATE * 0.7)
        left = int(max(-1.0, min(1.0, v * (0.6 - pan))) * 32767)
        right = int(max(-1.0, min(1.0, v * (0.6 + pan))) * 32767)
        pcm += struct.pack('<hh', left, right)

    def sub(tag, text):
        data = text.encode() + b'\0'
        if len(data) % 2:
            data += b'\0'
        return tag + struct.pack('<I', len(data)) + data

    info = b'INFO' + sub(b'INAM', title) + sub(b'IART', 'Aegis')
    fmt = struct.pack('<HHIIHH', 1, 2, RATE, RATE * 4, 4, 16)
    body = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'LIST' + struct.pack('<I', len(info)) + info
            + b'data' + struct.pack('<I', len(pcm)) + bytes(pcm))
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


title, tempo, notes = TUNES[sys.argv[2]]
write_wav(sys.argv[1], title, render(tempo, notes))
