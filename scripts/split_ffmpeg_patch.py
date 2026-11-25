#!/usr/bin/env python3
"""Split the full ffmpeg-9.1 patch into the hunks that touch upstream files.

The full diff (patches/ffmpeg-9.1-avs-dra.patch) is dominated by the imported
decoder sources (libcavs.c is ~30k lines).  This script writes a second patch
that only contains the changes to files that already exist in FFmpeg, which is
what a reviewer actually wants to read.
"""
import io
import re
import sys

src = sys.argv[1] if len(sys.argv) > 1 else 'patches/ffmpeg-9.1-avs-dra.patch'
dst = sys.argv[2] if len(sys.argv) > 2 else 'patches/ffmpeg-9.1-avs-dra-upstream-hunks.patch'

KEEP = (
    'ffmpeg-9.1/configure',
    'ffmpeg-9.1/libavcodec/Makefile',
    'ffmpeg-9.1/libavcodec/allcodecs.c',
    'ffmpeg-9.1/libavcodec/codec_desc.c',
    'ffmpeg-9.1/libavcodec/codec_id.h',
    'ffmpeg-9.1/libavcodec/defs.h',
    'ffmpeg-9.1/libavcodec/profiles.c',
    'ffmpeg-9.1/libavcodec/profiles.h',
    'ffmpeg-9.1/libavformat/mpegts.c',
)

text = io.open(src, encoding='utf-8', errors='replace').read()
chunks = re.split(r'(?m)^(?=diff --git )', text)
out = []
for c in chunks:
    m = re.match(r'diff --git a/(\S+) b/(\S+)', c)
    if not m:
        continue
    if m.group(2) in KEEP:
        out.append(c)

io.open(dst, 'w', encoding='utf-8', newline='\n').write(''.join(out))
print('wrote %s: %d file(s)' % (dst, len(out)))
