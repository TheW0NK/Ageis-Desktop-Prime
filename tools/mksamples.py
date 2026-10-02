#!/usr/bin/env python3
"""Writes sample pictures (PNG) for a new user's Images folder.

    mksamples.py OUTPUT_DIR
"""
import math
import os
import struct
import sys
import zlib


def png(path, width, height, pixel):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows.extend(pixel(x / width, y / height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(rows), 9)))
        f.write(chunk(b'IEND', b''))


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def mountains(x, y):
    sky = mix((255, 176, 120), (88, 110, 190), y * 1.6)
    sun = math.hypot((x - 0.7) * 1.6, y - 0.35)
    if sun < 0.07:
        sky = (255, 236, 200)
    elif sun < 0.2:
        sky = mix((255, 220, 170), sky, (sun - 0.07) / 0.13)
    ridge1 = 0.55 + 0.08 * math.sin(x * 9) + 0.05 * math.sin(x * 23 + 1)
    ridge2 = 0.68 + 0.05 * math.sin(x * 6 + 2) + 0.03 * math.sin(x * 31)
    if y > ridge2:
        return mix((40, 52, 74), (20, 26, 40), (y - ridge2) * 3)
    if y > ridge1:
        return mix((92, 98, 140), (60, 66, 104), (y - ridge1) * 4)
    return sky


def sea(x, y):
    if y < 0.5:
        return mix((140, 205, 255), (210, 236, 255), y * 2)
    wave = 0.5 + 0.5 * math.sin(x * 60 + y * 90)
    base = mix((24, 120, 170), (8, 50, 90), (y - 0.5) * 2)
    return mix(base, (200, 235, 255), wave * 0.15 * (1 - (y - 0.5) * 2))


def aurora(x, y):
    base = mix((6, 12, 30), (14, 30, 60), y)
    band = math.exp(-((y - 0.35 - 0.1 * math.sin(x * 7)) ** 2) / 0.006)
    band2 = math.exp(-((y - 0.5 - 0.08 * math.sin(x * 5 + 1)) ** 2) / 0.004)
    c = mix(base, (60, 230, 160), band * 0.8)
    c = mix(c, (150, 90, 240), band2 * 0.6)
    if (int(x * 997) * 31 + int(y * 991) * 17) % 997 == 0 and y < 0.6:
        c = (255, 255, 255)
    return c


out = sys.argv[1]
os.makedirs(out, exist_ok=True)
png(os.path.join(out, 'Mountains.png'), 640, 400, mountains)
png(os.path.join(out, 'Sea.png'), 640, 400, sea)
png(os.path.join(out, 'Aurora.png'), 640, 400, aurora)
