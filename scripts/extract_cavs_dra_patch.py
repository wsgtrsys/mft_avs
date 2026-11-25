#!/usr/bin/env python3
"""
extract_cavs_dra_patch.py -- pull the new files out of ffmpeg-7.1.2_cavs_dra.patch.

The AVS (libcavs) sources are also present in the FFmpeg-AVSPlus tree, but the DRA
audio decoder core (dradec.c / dradec.h) and its FFmpeg wrapper (libdradec.c) only
exist inside the patch file, so they are extracted once into scripts/avs-src/ and
from there ported by port_avs_to_ffmpeg.py.

  python3 scripts/extract_cavs_dra_patch.py                # extract everything
  python3 scripts/extract_cavs_dra_patch.py --list         # show what the patch adds
"""
import argparse
import os
import re
import sys

PATCH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                     "ffmpeg-7.1.2_cavs_dra.patch")
OUT   = os.path.join(os.path.dirname(os.path.abspath(__file__)), "avs-src")

WANT = ["libavcodec/dradec.c", "libavcodec/dradec.h", "libavcodec/libdradec.c",
        "libavcodec/libcavs.c", "libavcodec/libcavs.h", "libavcodec/libcavsdec.c"]


def blocks(text):
    """Yield (target_path, added_lines) for every file the patch touches."""
    cur = None
    added = []
    for line in text.split("\n"):
        if line.startswith("+++ "):
            if cur:
                yield cur, added
            path = line[4:].split("\t")[0].strip()
            path = re.sub(r"^\./?[^/]*/", "", path)      # strip ./ffmpeg-7.1.2_cavs_dra/
            cur, added = path, []
            continue
        if line.startswith("--- ") or line.startswith("diff ") or line.startswith("@@"):
            continue
        if cur is not None and line.startswith("+"):
            added.append(line[1:])
    if cur:
        yield cur, added


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--patch", default=PATCH)
    args = ap.parse_args()

    with open(args.patch, "r", encoding="utf-8", errors="replace", newline="") as f:
        text = f.read().replace("\r\n", "\n")

    got = {}
    for path, added in blocks(text):
        # a file is "new" when its hunk says @@ -0,0 +1,N @@ for every hunk
        got.setdefault(path, []).extend(added)

    if args.list:
        for p, lines in got.items():
            print(f"{p}  {len(lines)} added lines")
        return 0

    os.makedirs(OUT, exist_ok=True)
    for rel in WANT:
        if rel not in got:
            print(f"missing in patch: {rel}")
            continue
        dst = os.path.join(OUT, os.path.basename(rel))
        with open(dst, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(got[rel]) + "\n")
        print(f"wrote {dst}  ({len(got[rel])} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
