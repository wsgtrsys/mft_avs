#!/bin/sh
# ---------------------------------------------------------------------------
# pkg-config.sh - a very small pkg-config replacement.
#
# FFmpeg 9.1 detects the external AVS2 / AVS3 decoder libraries through
# pkg-config:
#
#   libdavs2  -> require_pkg_config libdavs2  "davs2 >= 1.6.0"  davs2.h  davs2_decoder_open
#   libuavs3d -> require_pkg_config libuavs3d "uavs3d >= 1.1.41" uavs3d.h uavs3d_decode
#
# and this machine has no pkg-config.  scripts/build-deps.cmd writes a .pc file
# for each library into deps/lib/pkgconfig; this script answers the handful of
# queries FFmpeg asks, and translates the -L/-l flags into the absolute .lib
# paths the MSVC driver accepts.
#
#   scripts/pkg-config.sh --exists --print-errors "davs2 >= 1.6.0"
#   scripts/pkg-config.sh --cflags davs2      -> -I<d>/include
#   scripts/pkg-config.sh --libs   davs2      -> <d>/lib/davs2.lib
#   scripts/pkg-config.sh --variable=includedir davs2
#
# Point FFmpeg at it with:  ./configure --pkg-config=<this file> ...
# ---------------------------------------------------------------------------
set -u

DEPS=$(cd "$(dirname "$0")/../deps" 2>/dev/null && pwd -W)
[ -n "${DEPS:-}" ] || { echo "pkg-config.sh: no deps directory next to me" >&2; exit 1; }
PCDIR="$DEPS/lib/pkgconfig"

mode=""
wantvar=""
mods=""
verreq=""
verneed=""

# configure first probes "pkg-config --version"; without a positive answer it
# disables pkg-config detection entirely.
case " $* " in
    *" --version "*) echo "1.8.1 (scripts/pkg-config.sh shim)"; exit 0 ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --cflags)            mode=cflags ;;
        --libs)              mode=libs ;;
        --exists)            mode=exists ;;
        --print-errors|--print-provides|--silence-errors) ;;
        --variable=*)        mode=variable; wantvar="${1#--variable=}" ;;
        --modversion)        mode=modversion ;;
        --atleast-version|--atleast-pkgconfig-version) mode=version ;;
        --*)                 ;;
        *)
            # "davs2 >= 1.6.0" or a bare module name
            if expr "$1" : '.*[<>=]' >/dev/null 2>&1; then
                verreq=$(echo "$1" | cut -d' ' -f1)
                verneed=$(echo "$1" | sed -e 's/.*[<>=][<=]*[ 	]*//' -e 's/[^0-9.].*//')
                mods="$mods $verreq"
            else
                mods="$mods $1"
            fi
            ;;
    esac
    shift
done

[ -n "$mods" ] || { echo "pkg-config.sh: no module given" >&2; exit 1; }

ver_ge() {  # ver_ge have want
    [ "$1" = "$2" ] && return 0
    [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]
}

pc_get() {  # pc_get file key   (.pc variables use "key=value", metadata uses "Key: value")
    sed -n 's/^'"$2"'[=:][ \t]*//p' "$1" | head -n1
}

pc_expand() {  # pc_expand file value
    f=$1
    pf=$(pc_get "$f" prefix      | tr '\' '/')
    ld=$(pc_get "$f" libdir      | sed "s|\${prefix}|$pf|g" | tr '\' '/')
    inc=$(pc_get "$f" includedir | sed "s|\${prefix}|$pf|g" | tr '\' '/')
    echo "$2" | sed -e "s|\${prefix}|$pf|g" -e "s|\${exec_prefix}|$pf|g" \
                    -e "s|\${libdir}|$ld|g" -e "s|\${includedir}|$inc|g"
}

# MSVC does not know -L/-l: hand the linker the .lib file directly.
pc_libs() {  # pc_libs file
    libs=$(pc_expand "$1" "$(pc_get "$1" Libs)")
    out=""
    dir=""
    for t in $libs; do
        case "$t" in
            -L*) dir=${t#-L} ;;
            -l*) n=${t#-l}; [ -n "$dir" ] && out="$out $dir/$n.lib" || out="$out $n.lib" ;;
            *)   out="$out $t" ;;
        esac
    done
    echo $out
}

rc=0
for m in $mods; do
    pc="$PCDIR/$m.pc"
    if [ ! -f "$pc" ]; then
        echo "$m not found (no $pc)" >&2
        rc=1
        continue
    fi
    ver=$(pc_get "$pc" Version)
    if [ -n "$verneed" ] && ! ver_ge "$ver" "$verneed"; then
        echo "$m version too old: found $ver, need $verneed" >&2
        rc=1
        continue
    fi
    case "$mode" in
        exists)    ;;
        modversion|version) echo "$ver" ;;
        cflags)    echo "$(pc_expand "$pc" "$(pc_get "$pc" Cflags)")" ;;
        libs)      pc_libs "$pc" ;;
        variable)
            case "$wantvar" in
                includedir) pc_expand "$pc" '${includedir}' ;;
                libdir)     pc_expand "$pc" '${libdir}' ;;
                prefix)     pc_get "$pc" prefix ;;
                *) echo "" ;;
            esac ;;
        *) echo "$m" ;;
    esac
done
exit $rc
