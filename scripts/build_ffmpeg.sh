#!/bin/bash
# Configure + build a trimmed FFmpeg 9.1 (AVS/AVS+ libcavs decoder) as static
# MSVC libraries, for linking into the AVS MFT decoder and its test tools.
#
# !! This script must be started from scripts/build-ffmpeg.cmd, i.e. from a
#    cmd.exe session where vcvars64.bat has already exported INCLUDE/LIB and the
#    MSYS tools (sh/make/sed/nasm) have been prepended to PATH.  Sourcing
#    vcvars from MSYS bash and then modifying PATH breaks MSYS' environment
#    block, after which cl.exe no longer sees INCLUDE.
#
#   scripts/build-ffmpeg.cmd [--shared] [--full]
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ffmpeg-9.1"
BUILD="$ROOT/ff91-build"
OUT="$ROOT/ff91-install"

MODE=minimal
SHARED=0
CLEAN=0
for a in "$@"; do
    case "$a" in
        --shared) SHARED=1 ;;
        --full)   MODE=full ;;
        --clean)  CLEAN=1 ;;
        *) echo "unknown option $a"; exit 1 ;;
    esac
done

[ -f "$SRC/configure" ] || { echo "missing $SRC - extract ffmpeg-9.1.tar.xz first"; exit 1; }
command -v make >/dev/null || { echo "make not found - run scripts/build-ffmpeg.cmd"; exit 1; }
command -v cl   >/dev/null || { echo "cl.exe not found - run scripts/build-ffmpeg.cmd"; exit 1; }
[ -n "${INCLUDE:-}" ]      || { echo "INCLUDE not set (vcvars not imported)"; exit 1; }

mkdir -p "$BUILD" "$OUT"
cd "$BUILD"

# The Vulkan compute backend of the AVS reconstruction stage needs vulkan.h.  It
# loads the Vulkan loader at run time, so nothing is linked against the SDK and
# the build also works without one.
VK_INC=""
if [ -n "${VULKAN_SDK:-}" ] && [ -d "$VULKAN_SDK/Include" ]; then
    VK_INC=" -I$(cygpath -m "$VULKAN_SDK/Include")"
fi

CONF=( "$SRC/configure"
    --target-os=mingw32
    --toolchain=msvc
    --arch=x86_64
    --prefix="$OUT"
    --disable-doc
    --disable-programs
    --disable-debug
    --disable-avdevice
    --disable-avfilter
    --disable-vulkan
    --disable-swresample
        # libavcodec/avs_vk.c drives Vulkan itself; FFmpeg's own Vulkan hwaccel is
    # not used by the AVS path and would need the SDK at link time
    # libcavs.c includes <pthread.h>; MSVC gets the Win32 shim from compat/avs
    --extra-cflags="-I$(cygpath -w "$SRC/compat/avs")$VK_INC"
)

if [ "$MODE" = minimal ]; then
    CONF+=(
        --disable-everything
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale
        # AVS1-P2 / AVS1-P16 (AVS+) decoder core and its elementary-stream parser
        --enable-decoder=cavs
        --enable-parser=cavsvideo
        --enable-demuxer=cavsvideo
        # DRA audio (AVS audio, GB/T 20090.6) - the wrapper is a component of its
        # own, named after the object file (like the other lib* decoders)
        --enable-decoder=libdra
        # enough demuxing support for the MFT test harness (CCTV-4.ts)
        --enable-demuxer=mpegts
        --enable-demuxer=mpegtsraw
        # the AVS2 / AVS3 samples in video/avs2_avs3_test_video are Matroska
        --enable-demuxer=matroska
        --enable-muxer=rawvideo
        --enable-protocol=file
        # Parsers for the formats AVSMediaSource can also publish (H.264, MPEG-1/2/4,
        # HEVC, AAC, MP2/MP3, AC3).  They are not decoded here - the system MFTs do
        # that - but without them libavformat cannot find the picture size or the audio
        # format from the bit stream, so those streams could not be described at all
        # (a MPEG-TS PMT alone does not carry them).
        --enable-parser=h264
        --enable-parser=hevc
        --enable-parser=mpegvideo
        --enable-parser=mpeg4video
        --enable-parser=aac
        --enable-parser=aac_latm
        --enable-parser=mpegaudio
        --enable-parser=ac3
        # AVS2 / AVS3.  FFmpeg has the bit stream parsers and the wrapper decoders,
        # the actual decoding lives in the external libraries built by
        # scripts/build-deps.cmd (deps/lib/davs2.lib, deps/lib/uavs3d.lib): the
        # wrappers are switched on by --enable-libdavs2 / --enable-libuavs3d.
        --enable-parser=avs2
        --enable-parser=avs3
        # raw AVS2 / AVS3 elementary streams (.avs2 / .avs3): without these the
        # only raw demuxer available is cavsvideo, which labels every AVS start
        # code pattern as CAVS and reports no dimensions.
        --enable-demuxer=avs2
        --enable-demuxer=avs3
    )
fi

if [ "$MODE" = minimal ]; then
    # Round 30: this used to hang off an environment variable that only the person who ran the
    # build had in mind.  Round 28 built with it and used the H.264 control to attribute the
    # missing MESessionEnded to the subtype; round 30 rebuilt without it and the control
    # silently disappeared - the cell delivered 0 pictures again and 11d/11n went red for a
    # reason that looks like a code defect and is only a missing configure flag.  A control a
    # routine rebuild can delete is not a control, so it is now unconditional for the test
    # build.  The shipping path never uses it: the MFT refuses H.264 unless
    # AVS_MFT_ALLOW_H264 is set, and standard formats stay with the system decoders.
    CONF+=(--enable-decoder=h264)
    echo "   minimal build: H.264 decoder included for the attribution control only"
fi

# AVS2 (davs2) and AVS3 (uavs3d) come from external libraries; FFmpeg insists on
# finding them through pkg-config, and this machine has none, so scripts/pkg-config.sh
# answers the two queries configure makes (see deps/lib/pkgconfig/*.pc).
DEPS="$ROOT/deps"
if [ -f "$DEPS/lib/davs2.lib" ] || [ -f "$DEPS/lib/uavs3d.lib" ]; then
    CONF+=(--pkg-config="$ROOT/scripts/pkg-config.sh")
    # FFmpeg's own link tests do not pull in the CRT import library, while
    # davs2's win32thread.obj references the import thunk of _beginthreadex.
    CONF+=(--extra-ldflags="msvcrt.lib")
    # davs2 is GPL, so the whole build becomes GPL (uavs3d itself is BSD-3)
    [ -f "$DEPS/lib/davs2.lib" ] && CONF+=(--enable-gpl --enable-libdavs2 --enable-decoder=libdavs2)
    [ -f "$DEPS/lib/uavs3d.lib" ] && CONF+=(--enable-libuavs3d --enable-decoder=libuavs3d)
    echo "   AVS2/AVS3 back ends: $DEPS/lib"
fi
[ "$SHARED" = 1 ] && CONF+=(--enable-shared --enable-static) || CONF+=(--disable-shared --enable-static)

echo "== configure"

# Turning components on or off is not an ordinary source change: FFmpeg's make rules
# do not make every object depend on config.h, and the *_list.c files that configure
# generates are #included by allcodecs.c / parsers.c / ... .  An incremental rebuild
# therefore keeps objects compiled for the old configuration, which shows up much later
# as unresolved externals in the MFT link (ff_square_tab, ff_ac3_parser).  Drop the
# objects whenever the requested options are not the ones the tree was built with.
STAMP="$BUILD/.configure-options"
# The files the port edits (configure, the Makefile, codec_id.h ...): a change in one of
# them is a change of the whole ABI - codec_id.h for example is inlined into every object
# that uses an enum value - and FFmpeg's dependency tracking will not rebuild all of them.
PORTED_FILES="configure libavcodec/Makefile libavcodec/allcodecs.c libavcodec/codec_id.h libavcodec/codec_desc.c libavcodec/defs.h libavcodec/profiles.c libavcodec/profiles.h libavcodec/libcavs.c libavcodec/libcavs.h libavcodec/libcavsdec.c libavcodec/dradec.c libavcodec/dradec.h libavcodec/libdradec.c libavformat/mpegts.c"
NEWOPTS="$(printf '%s\n' "${CONF[@]}")
$( (cd "$SRC" && md5sum $PORTED_FILES 2>/dev/null) )"
if [ "$CLEAN" = 1 ] || [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$NEWOPTS" ]; then
    echo "   (objects dropped: configure options or ported FFmpeg files changed)"
    find . -name '*.o' -delete
fi
"${CONF[@]}"
printf '%s\n' "$NEWOPTS" > "$STAMP"

# configure regenerates the *_list.c files that are #included by allcodecs.c /
# parsers.c / bitstream.c / protocols.c, but the make rules do not express that
# dependency, so drop the objects that would otherwise stay stale.
rm -f libavcodec/allcodecs.o libavcodec/parsers.o libavcodec/bitstream.o \
      libavformat/protocols.o

echo "== make"
make -j"${NUMBER_OF_PROCESSORS:-8}"
make install

# MSVC's linker (and #pragma comment(lib, "avcodec")) wants avcodec.lib, while
# the FFmpeg/msys toolchain produces libavcodec.a - create MSVC style names.
cd "$OUT/lib"
for a in lib*.a; do
    [ -e "$a" ] || continue
    base="${a#lib}"; base="${base%.a}"
    cp -f "$a" "$base.lib"
    echo "lib    $base.lib"
done
echo
echo "installed in $OUT"
