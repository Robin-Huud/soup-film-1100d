#!/usr/bin/env python3
"""Wrap a JPEG in a minimal CR2 container, the way Canon stores the preview.

IFD0 of a CR2 holds the full-size JPEG preview: Compression 6, with
StripOffsets/StripByteCounts pointing at it. No raw data is included, so
this only exercises the preview path.

    make_cr2.py preview.jpg out.CR2
"""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
jpeg = open(src, "rb").read()

# width and height from the JPEG's SOF0
i = 2
while True:
    marker, length = struct.unpack(">HH", jpeg[i:i + 4])
    if marker == 0xFFC0:
        height, width = struct.unpack(">HH", jpeg[i + 5:i + 9])
        break
    i += 2 + length

model = b"Canon EOS 80D\0"
entries = 6
ifd_off = 0x10
ifd_len = 2 + entries * 12 + 4
model_off = ifd_off + ifd_len
data_off = (model_off + len(model) + 511) & ~511

ifd = struct.pack("<H", entries)
ifd += struct.pack("<HHII", 0x0100, 3, 1, width)
ifd += struct.pack("<HHII", 0x0101, 3, 1, height)
ifd += struct.pack("<HHII", 0x0103, 3, 1, 6)
ifd += struct.pack("<HHII", 0x0110, 2, len(model), model_off)
ifd += struct.pack("<HHII", 0x0111, 4, 1, data_off)
ifd += struct.pack("<HHII", 0x0117, 4, 1, len(jpeg))
ifd += struct.pack("<I", 0)

out = b"II*\0" + struct.pack("<I", ifd_off) + b"CR\x02\x00" + b"\0" * 4
out += ifd + model
out += b"\0" * (data_off - len(out)) + jpeg
open(dst, "wb").write(out)
print(f"{dst}: {width}x{height} preview at {data_off}, {len(out)} bytes")
