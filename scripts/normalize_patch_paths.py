#!/usr/bin/env python3
"""Normalize the diff headers of patches/ffmpeg-9.1-avs-dra.patch.

git diff --no-index uses the real directory names, so the a/ side comes out as
a/tmp/ff91-orig/ffmpeg-9.1/...  Rewrite only the header lines ("diff --git",
"---", "+++") so the patch applies with patch -p1 / git apply -p1 from a
directory that contains ffmpeg-9.1/.
"""
import io
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'patches/ffmpeg-9.1-avs-dra.patch'
s = io.open(path, encoding='utf-8', errors='replace').read()

def fix(line):
    return line.replace('a/tmp/ff91-orig/ffmpeg-9.1', 'a/ffmpeg-9.1')

out = []
for line in s.split('\n'):
    if line.startswith('diff --git ') or line.startswith('--- ') or line.startswith('+++ '):
        line = fix(line)
    out.append(line)

io.open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('normalized ' + path)
