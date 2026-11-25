#!/usr/bin/env python3
"""patch_davs2_msvc.py - make davs2 compile with MSVC in the 10 bit configuration.

davs2 is written for GCC/Clang and MSVC in the 8 bit configuration happens to
work, because pel_t is uint8_t there.  With HIGH_BIT_DEPTH=1 pel_t is uint16_t
and two MSVC strictness problems appear:

  * source/common/osdep.h: ALIGN_POINTER() casts the aligned address to
    uint8_t* and assigns it back to a pel_t* member -> C2440.
  * source/common/decoder.cc: the public davs2_picture_t carries uint8_t*
    plane pointers, which cannot be assigned from pel_t* without a cast.

The patch is idempotent, so it can run on every build.
"""
import io
import os
import sys

root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', 'deps', 'src', 'davs2-master')
changed = []

# ---------------------------------------------------------------- ALIGN_POINTER
p = os.path.join(root, 'source', 'common', 'osdep.h')
s = io.open(p, encoding='utf-8', errors='replace').read()
old = ('#  define ALIGN_POINTER(p)      (p) = (uint8_t *)((intptr_t)((p) + (CACHE_LINE_SIZE - 1)) & (~(intptr_t)(CACHE_LINE_SIZE - 1)))')
new = '\n'.join([
    '#  define CACHE_LINE_SIZE       32    /* for x86-64 and x86 */',
    '/* The upstream version casts the aligned address to uint8_t*, which MSVC',
    ' * rejects when pel_t is 16 bit (the 10 bit configuration); keep the type of',
    ' * the argument by storing through a void**.  A template is not possible here,',
    " * this header is included inside davs2's extern \"C\" block. */",
    '#ifdef _MSC_VER',
    "#  define ALIGN_POINTER(p) do {                                                             \\",
    "       void *_avs2_aligned = (void *)(((uintptr_t)(void *)(p) +                           \\",
    "                            (uintptr_t)(CACHE_LINE_SIZE - 1)) &                            \\",
    "                            ~(uintptr_t)(CACHE_LINE_SIZE - 1));                            \\",
    "       *(void **)&(p) = _avs2_aligned;                                                     \\",
    '   } while (0)',
    '#else',
    '#  define ALIGN_POINTER(p)      (p) = (uint8_t *)((intptr_t)((p) + (CACHE_LINE_SIZE - 1)) & (~(intptr_t)(CACHE_LINE_SIZE - 1)))',
    '#endif',
])
if '*(void **)&(p) = _avs2_aligned' not in s:
    assert old in s, 'ALIGN_POINTER not found in osdep.h'
    s = s.replace(old, new)
    io.open(p, 'w', encoding='utf-8', newline='\r\n').write(s)
    changed.append('osdep.h: ALIGN_POINTER keeps the pointer type (MSVC)')

# ------------------------------------------------------- picture plane pointers
p = os.path.join(root, 'source', 'common', 'decoder.cc')
s = io.open(p, encoding='utf-8', errors='replace').read()
old = ('        pic->planes[0]  = frame->planes[0];\n'
       '        pic->planes[1]  = frame->planes[1];\n'
       '        pic->planes[2]  = frame->planes[2];')
new = ('        pic->planes[0]  = (uint8_t *)frame->planes[0];\n'
       '        pic->planes[1]  = (uint8_t *)frame->planes[1];\n'
       '        pic->planes[2]  = (uint8_t *)frame->planes[2];')
if old in s:
    s = s.replace(old, new)
    io.open(p, 'w', encoding='utf-8', newline='\r\n').write(s)
    changed.append('decoder.cc: cast pel_t* planes to the public uint8_t*')

# -------------------------------------------------------- SAO asm kernels
# source/common/vec/intrinsic_sao.cc is entirely '#if !HIGH_BIT_DEPTH', but
# davs2_sao_init() still installs the SSE/AVX2 SAO kernels under '#if HAVE_MMX',
# so a 10 bit link fails on davs2_SAO_on_block_*_sse128/_avx2.  Keep the C
# kernels for 10 bit; nothing changes when HIGH_BIT_DEPTH is 0.
p = os.path.join(root, 'source', 'common', 'sao.cc')
s = io.open(p, encoding='utf-8', errors='replace').read()
old = ('    /* init asm function handles */\n#if HAVE_MMX\n')
new = ('    /* init asm function handles */\n'
       '#if HAVE_MMX && !HIGH_BIT_DEPTH   /* the SSE/AVX2 SAO kernels are 8 bit only */\n')
if old in s:
    s = s.replace(old, new)
    io.open(p, 'w', encoding='utf-8', newline='\r\n').write(s)
    changed.append('sao.cc: do not install 8 bit only SAO kernels in 10 bit builds')

for c in changed:
    print('patched ' + c)
if not changed:
    print('already patched')
