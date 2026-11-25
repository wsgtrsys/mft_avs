#!/usr/bin/env python3
"""Regenerate patches/ffmpeg-9.1-avs-dra.patch from the working tree.

`git diff --no-index` over the two trees also picks up everything the build
generated (config.h, ffbuild/, *.texi).  The delivered patch has always been
the port itself: the files listed in KEEP below.  New files of the Vulkan
compute backend are part of the port now, so they are listed too - without
them the delivered patch described a decoder that had no GPU path at all.
"""
import io
import os
import subprocess
import sys

# The pristine upstream tree the diff is taken against.  It lives outside tmp/ on purpose:
# it is a gate input, not scratch.  A tmp/ cleanup once deleted tmp/ff91-orig, after which
# the generator wrote an empty diff and exited with "not present in the diff: ..." - and
# nobody noticed, because the check that followed still applied the previously generated
# files.  Extract it with:  mkdir -p pristine && tar -xJf ffmpeg-9.1.tar.xz -C pristine
ORIG = 'pristine/ffmpeg-9.1'
NEW = 'ffmpeg-9.1'
FULL = 'patches/ffmpeg-9.1-avs-dra.patch'
REVIEW = 'patches/ffmpeg-9.1-avs-dra-upstream-hunks.patch'
RAW = 'tmp/ffmpeg-9.1-raw.diff'

KEEP = [
    'compat/avs/pthread.h',
    'configure',
    'libavcodec/Makefile',
    'libavcodec/allcodecs.c',
    'libavcodec/avs_vk.c',
    'libavcodec/avs_vk.h',
    'libavcodec/avs_vk_spv.h',
    'libavcodec/codec_desc.c',
    'libavcodec/codec_id.h',
    'libavcodec/defs.h',
    'libavcodec/dradec.c',
    'libavcodec/dradec.h',
    'libavcodec/libcavs.c',
    'libavcodec/libcavs.h',
    'libavcodec/libcavsdec.c',
    'libavcodec/libdradec.c',
    'libavcodec/profiles.c',
    'libavcodec/profiles.h',
    'libavcodec/vulkan/avs_dsp.comp.glsl',
    'libavformat/mpegts.c',
]

# upstream files, i.e. the ones a reviewer reads first
UPSTREAM = set(KEEP) - {
    'compat/avs/pthread.h',
    'libavcodec/avs_vk.c',
    'libavcodec/avs_vk.h',
    'libavcodec/avs_vk_spv.h',
    'libavcodec/dradec.c',
    'libavcodec/dradec.h',
    'libavcodec/libcavs.c',
    'libavcodec/libcavs.h',
    'libavcodec/libcavsdec.c',
    'libavcodec/libdradec.c',
    'libavcodec/vulkan/avs_dsp.comp.glsl',
}

if not os.path.isdir(ORIG):
    sys.exit('no pristine tree at %s - extract it first:\n'
             '  mkdir -p %s && tar -xJf ffmpeg-9.1.tar.xz -C %s'
             % (ORIG, os.path.dirname(ORIG), os.path.dirname(ORIG)))

if not os.path.isdir(NEW):
    # This is the case of a packaged delivery (dist/): it ships the base tarball and the
    # patch, not the patched working tree, so there is nothing to regenerate from.  Say
    # what to do instead of letting 'git diff --no-index' fail on a missing directory.
    sys.exit('no patched source tree at %s - this generator compares the development tree\n'
             'against pristine, so it only runs in the development workspace.  In a packaged\n'
             'delivery, check the patch itself instead:\n'
             '  tar -xJf ffmpeg-9.1.tar.xz\n'
             '  git apply -p1 --check patches/ffmpeg-9.1-avs-dra.patch' % NEW)

r = subprocess.run(['git', 'diff', '--no-index', ORIG, NEW],
                   stdout=open(RAW, 'wb'), stderr=subprocess.DEVNULL)
if r.returncode not in (0, 1):
    sys.exit('git diff failed: %d' % r.returncode)

text = io.open(RAW, encoding='utf-8', errors='replace').read()

# rewrite only the header lines so the patch applies with -p1 next to a
# directory named ffmpeg-9.1, whatever the temporary tree was called
out = []
for line in text.split('\n'):
    if line.startswith(('diff --git ', '--- ', '+++ ')):
        line = line.replace('a/' + ORIG, 'a/ffmpeg-9.1') \
                   .replace('b/' + NEW, 'b/ffmpeg-9.1') \
                   .replace('a/' + NEW, 'a/ffmpeg-9.1') \
                   .replace('b/' + ORIG, 'b/ffmpeg-9.1')
    out.append(line)
text = '\n'.join(out)

chunks = text.split('diff --git ')
kept, seen = [], set()
for c in chunks:
    if not c.strip():
        continue
    head = c.split('\n', 1)[0]
    if not head.startswith('a/ffmpeg-9.1/'):
        continue
    path = head.split(' b/ffmpeg-9.1/', 1)[1].split('\t')[0]
    if path in KEEP:
        kept.append('diff --git ' + c)
        seen.add(path)

missing = [p for p in KEEP if p not in seen]
if missing:
    sys.exit('not present in the diff: %s' % ', '.join(missing))

io.open(FULL, 'w', encoding='utf-8', newline='\n').write(''.join(kept))
print('wrote %s: %d file(s)' % (FULL, len(kept)))

review = []
for c in kept:
    path = c.split('\n', 1)[0].split(' b/ffmpeg-9.1/', 1)[1].split('\t')[0]
    if path in UPSTREAM:
        review.append(c)
io.open(REVIEW, 'w', encoding='utf-8', newline='\n').write(''.join(review))
print('wrote %s: %d file(s)' % (REVIEW, len(review)))
