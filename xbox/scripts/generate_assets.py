#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate committed XS4 package artwork without replacing the brand assets."""
from pathlib import Path
import struct


ASSETS = {
    "Logo.png": (150, 150),
    "SmallLogo.png": (44, 44),
    "StoreLogo.png": (50, 50),
    "Splash.png": (620, 300),
    "XS4-Mark.png": (1254, 1254),
}


def png_size(path):
    with path.open("rb") as image:
        header = image.read(24)
    if len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError(f"Invalid PNG asset: {path}")
    return struct.unpack(">II", header[16:24])


def generate(directory):
    """Check the checked-in images used by the UWP package; never overwrite them."""
    for name, expected_size in ASSETS.items():
        path = directory / name
        if not path.is_file():
            raise FileNotFoundError(f"Required XS4 artwork is missing: {path}")
        actual_size = png_size(path)
        if actual_size != expected_size:
            raise ValueError(f"Unexpected dimensions for {path}: {actual_size}; expected {expected_size}")


if __name__ == "__main__":
    generate(Path(__file__).resolve().parents[1] / "Assets")
