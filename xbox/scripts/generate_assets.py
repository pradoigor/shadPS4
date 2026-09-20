#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic package marks and quiet PCM tone, without external dependencies."""
import math
from pathlib import Path
import struct
import wave
import zlib


def png(width, height):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = bytearray()
    for y in range(height):
        pixels.append(0)
        for x in range(width):
            # Abstract brackets, deliberately distinct from Sony/Microsoft branding.
            nx, ny = x / width, y / height
            left = .24 < nx < .30 and .25 < ny < .75
            right = .70 < nx < .76 and .25 < ny < .75
            bars = (.24 < nx < .40 or .60 < nx < .76) and (.25 < ny < .31 or .69 < ny < .75)
            pixels.extend((101, 228, 195, 255) if left or right or bars else (16, 24, 39, 255))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(pixels, 9)) + chunk(b'IEND', b'')


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    for name, size in {'Logo': (150, 150), 'SmallLogo': (44, 44), 'StoreLogo': (50, 50), 'Splash': (620, 300)}.items():
        (directory / (name + '.png')).write_bytes(png(*size))
    with wave.open(str(directory / 'tone.wav'), 'wb') as output:
        output.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
        samples = bytearray()
        for i in range(96000):
            envelope = min(1, i / 2400, (95999 - i) / 2400)
            samples.extend(struct.pack('<h', round(14000 * envelope * math.sin(2 * math.pi * 440 * i / 48000))))
        output.writeframes(samples)


if __name__ == '__main__':
    generate(Path(__file__).resolve().parents[1] / 'Assets')
