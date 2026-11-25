#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""
make-delivery.py - pack the deliverable: MFT source + the built DLL + the FFmpeg 9.1
patch + the build scripts, in one self-consistent tree.

The third-party sources are NOT copied in.  FFmpeg, davs2 and uavs3d are referenced by
upstream URL, and what we actually compiled is pinned by the hash of the snapshot we
built (THIRD-PARTY.md and deps/README.md carry both).  Shipping a vendored copy would
have meant shipping someone else's tree with no way to say which commit it was.

Why a script and not a hand copy: a hand copy cannot be re-checked.  This script is
the only definition of what belongs in dist/, it regenerates the whole tree, and
scripts/delivery-check.py verifies afterwards that the packed source still equals the
working tree and that the packed DLL still decodes byte-for-byte like the reference.

    "C:\\Python310\\python.exe" scripts\\make-delivery.py            pack, copy the existing DLL
    "C:\\Python310\\python.exe" scripts\\make-delivery.py --build   pack, then BUILD the DLL inside dist/ so that
                                       the shipped binary is provably built from the
                                       shipped source (this is what a delivery should say)
"""
import datetime
import hashlib
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")
DATE = datetime.date.today().isoformat()

# ---------------------------------------------------------------- what goes in
SINGLE_FILES = [
    # the patches.  The base they apply to is a reference, not a file in the package:
    # deps/README.md names the FFmpeg repository, the n9.1 tag and the release tarball
    # (with its SHA-256) that the patch was generated against.
    "patches/ffmpeg-9.1-avs-dra.patch",
    "patches/ffmpeg-9.1-avs-dra-upstream-hunks.patch",
]

BUILD_SCRIPTS = [
    # the build chain, in build order, plus everything those scripts read
    "scripts/build-ffmpeg.cmd",
    "scripts/build_ffmpeg.sh",
    "scripts/build-deps.cmd",
    "scripts/build-mft.cmd",
    "scripts/vcenv.sh",
    "scripts/pkg-config.sh",
    "scripts/patch_davs2_msvc.py",
    # patch generation / verification (patch-gate.cmd is the one-command check)
    "scripts/patch-gate.cmd",
    "scripts/make-ffmpeg-patch.py",
    "scripts/normalize_patch_paths.py",
    "scripts/split_ffmpeg_patch.py",
    "scripts/extract_cavs_dra_patch.py",
    # where the third-party sources come from, since they are no longer shipped
    "deps/README.md",
    # the delivery's own checks, so that every claim in README.md can be re-asked with
    # nothing but the package: verify.cmd is the copy of scripts/delivery-verify.cmd
    "scripts/delivery-verify.cmd",
    "scripts/dll-imports.py",
    # and the definition of this package itself.  README.md tells the reader that this
    # tree is generated, not hand-copied; that sentence should come with the file it
    # names.  (Running delivery-check.py needs a development working tree plus a dist/ -
    # inside a clone the check to run is verify.cmd.)
    "scripts/make-delivery.py",
    "scripts/delivery-check.py",
]

TREES = [
    # (path, dirs to skip, file suffixes to skip)
    ("MediaFoundationTransform", {"obj", "x64"}, {".map", ".pdb", ".exp", ".user"}),
    ("tools", set(), {".map", ".inc", ".obj", ".log"}),
    ("scripts/compat", set(), set()),
    # davs2 / uavs3d / nasm and the FFmpeg headers and static libraries are referenced
    # by URL now (deps/README.md, THIRD-PARTY.md): a build in the package means fetching
    # them first.  What is NOT dropped is the harness below, because that is how anyone
    # checks the delivered DLL without registering anything.
    # the console harness: it is how anyone (including verify.cmd)
    # checks the delivered DLL without registering anything
    ("bin", set(), {".map", ".log", ".yuv", ".bmp", ".txt", ".pdb", ".ilk"}),
]

# one sample, so that the byte-level self test in verify.cmd can run out of the box.
# THIRD-PARTY.md states plainly that this file is a broadcast excerpt we do not own.
SAMPLES = ["samples/CCTV-4-short.avs"]

# the generated documentation
README_TEMPLATE = "scripts/delivery-README.md"
VERIFY_TEMPLATE = "scripts/delivery-verify.cmd"
THIRD_PARTY_TEMPLATE = "scripts/delivery-THIRD-PARTY.md"
LICENSE_TEMPLATE = "scripts/delivery-LICENSE"

# the decoded bytes verify.cmd reproduces from that sample (see delivery-check.py, which
# reads the same constant): one definition, so the document and the check cannot drift.
CANARY = "6c52e2bcd47d12ceee40532e1173eb0e"

# git ls-remote refs/tags/n9.1 on 2026-11-25; recorded because the reference is the tag
FFmpeg_TAG_N91_COMMIT = "74e4393595cf2b5857f0da925c4e0dd74ea306e5"

# measured in the development workspace on 2026-11-25.  re-derived from the files when
# they are present; kept here so the reference in THIRD-PARTY.md cannot lose its
# identity on a machine that happens not to have deps/dl.
RECORDED = {
    "ffmpeg-9.1.tar.xz": (12557532, "1a4d394ae63d190a526cd052eb4415c156b5c5e2df37efab41a7de0b8fde2433"),
    "deps/dl/davs2.tar.gz": (534423, "1d688e4832822c779053fa69d2dab2651a3f82461e2b74c67d9bae2ef55da134"),
    "deps/dl/uavs3d.tar.gz": (629324, "f5288ffed9f540a0300fc040b971cb910bf676e2a8441c5aee6495280c0683e8"),
}

# what must never be packed even if a rule would match it
NEVER = {"samples", "video", "out", "tmp", "pristine", "FFmpeg-AVSPlus", "ffmpeg-9.1",
         "dist", "docs", "plan", "deps/dl", "deps/nasm.zip"}

# history and editor state are not part of a delivery; MediaFoundationTransform/ carries
# a vendored .git from the MSFT sample repository, which would otherwise be copied too
HIDDEN = {".git", ".vs", ".idea", "__pycache__"}

# build intermediates.  Skipped while copying (not deleted afterwards) so that the count
# the script prints is the count of what is in dist/, and .gitignore can point at the
# same list.  .lib is in it because the package no longer ships any static library:
# FFmpeg's and davs2's are build inputs that the delivery references instead.
BUILD_JUNK = {".obj", ".pdb", ".ilk", ".exp", ".map", ".iobj", ".ipdb", ".log",
              ".res", ".tlog", ".sbr", ".lib"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")


def relp(base, p):
    """Path of p relative to base, "/" separated, "" for base itself.  Do not build this
    with relpath(p + "/")[:-1]: relpath drops the trailing separator and the [:-1] then
    eats a real character of the last directory name."""
    r = os.path.relpath(p, base).replace("\\", "/")
    return "" if r == "." else r


def in_never(r):
    return any(r == n or r.startswith(n + "/") for n in NEVER)


def plan():
    """The complete list of (path in the working tree, path in dist) this delivery
    contains.  It is one definition on purpose: delivery-check.py asks the same
    question, and two definitions of "what belongs in the package" always drift."""
    items = []
    for r in SINGLE_FILES + BUILD_SCRIPTS + SAMPLES:
        items.append((r, r))
    for path, skip_dirs, skip_suffix in TREES:
        base = os.path.join(ROOT, path.replace("/", os.sep))
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in skip_dirs and d not in HIDDEN]
            rr = relp(ROOT, dirpath)
            if in_never(rr):
                dirnames[:] = []
                continue
            for fn in filenames:
                if os.path.splitext(fn)[1].lower() in skip_suffix:
                    continue
                if os.path.splitext(fn)[1].lower() in BUILD_JUNK:
                    continue
                r = (rr + "/" + fn).replace(os.sep, "/")
                if in_never(r):
                    continue
                items.append((r, r))
    return items


def copy_file(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    # a .cmd with LF instead of CRLF breaks 'goto' and 'set X=' handling in cmd.exe;
    # the delivery must be runnable straight from the archive.
    if dst.lower().endswith(".cmd"):
        with open(dst, "rb") as f:
            data = f.read().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
        with open(dst, "wb") as f:
            f.write(data)


def identity(path):
    """(bytes, sha256) of a file we reference but do not ship.  Measured when the file
    is here, otherwise the value measured on 2026-11-25 - never blank."""
    p = os.path.join(ROOT, path.replace("/", os.sep))
    if os.path.exists(p):
        return format(os.path.getsize(p), ","), sha256_file(p)
    size, h = RECORDED[path]
    return format(size, ","), h


def pack():
    # dist/ may be a git checkout (that is how the delivery gets published); regenerating
    # the tree must not throw away the history, so move it aside and put it back.
    keep_git = None
    gitdir = os.path.join(DIST, ".git")

    def park_git():
        nonlocal keep_git
        if os.path.isdir(gitdir):
            keep_git = os.path.join(ROOT, "tmp", "dist_git_parked")
            if os.path.isdir(keep_git):
                shutil.rmtree(keep_git)
            shutil.move(gitdir, keep_git)

    def restore_git():
        if keep_git:
            shutil.move(keep_git, gitdir)

    park_git()
    if os.path.isdir(DIST):
        # a vendored .git object pack ships read-only files; clear the bit rather than
        # dying here (the tree is generated, deleting it is always allowed).
        def onerror(fn, path, exc):
            os.chmod(path, 0o666)
            fn(path)
        shutil.rmtree(DIST, onerror=onerror)
    os.makedirs(DIST)
    packed = []

    for r in SINGLE_FILES + BUILD_SCRIPTS + SAMPLES:
        src = os.path.join(ROOT, r.replace("/", os.sep))
        if not os.path.exists(src):
            sys.exit("make-delivery: MISSING input %s - the delivery would be incomplete" % r)
        copy_file(src, os.path.join(DIST, r.replace("/", os.sep)))
        packed.append(r)

    for r in SAMPLES:
        src = os.path.join(ROOT, r.replace("/", os.sep))
        if not os.path.exists(src):
            sys.exit("make-delivery: MISSING sample %s - verify.cmd could not self-test" % r)
        copy_file(src, os.path.join(DIST, r.replace("/", os.sep)))
        packed.append(r)

    for path, skip_dirs, skip_suffix in TREES:
        base = os.path.join(ROOT, path.replace("/", os.sep))
        if not os.path.isdir(base):
            sys.exit("make-delivery: MISSING input tree %s" % path)
        n = 0
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in skip_dirs and d not in HIDDEN]
            rr = relp(ROOT, dirpath)
            if in_never(rr):
                dirnames[:] = []
                continue
            for fn in filenames:
                if os.path.splitext(fn)[1].lower() in skip_suffix:
                    continue
                if os.path.splitext(fn)[1].lower() in BUILD_JUNK:
                    continue
                src = os.path.join(dirpath, fn)
                r = (rr + "/" + fn).replace(os.sep, "/")
                if in_never(r):
                    continue
                copy_file(src, os.path.join(DIST, r.replace("/", os.sep)))
                packed.append(r)
                n += 1
        print("  packed %-28s %d file(s)" % (path, n))

    # the intermediate object files of the copied project are not part of a delivery
    for proj in ("AVSVideoDecoder",):
        p = os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder", "obj")
        if os.path.isdir(p):
            shutil.rmtree(p)
    # a build in the development workspace leaves .obj/.pdb/.ilk/.res/.tlog next to the
    # project; those are not part of a delivery in any mode, --build or not.
    junk = BUILD_JUNK
    for dirpath, dirnames, filenames in os.walk(DIST):
        if ".git" in dirnames:
            dirnames.remove(".git")
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in junk:
                os.remove(os.path.join(dirpath, fn))
    restore_git()
    return packed


def build_inside_dist():
    """Build inside dist/ so that the shipped DLL comes from the shipped source."""
    log = os.path.join(ROOT, "tmp", "make_delivery_build.log")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    cmd = [os.path.join(DIST, "scripts", "build-mft.cmd")]
    with open(log, "wb") as f:
        rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=DIST,
                             stdin=subprocess.DEVNULL)
    dll = os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder",
                       "bin", "x64", "Release", "AVSVideoDecoder.dll")
    if rc != 0 or not os.path.exists(dll):
        sys.stderr.write(open(log, "rb").read().decode("utf-8", "replace")[-3000:])
        sys.exit("make-delivery: BUILD INSIDE dist FAILED (exit %s), log %s" % (rc, rel(log)))
    # the build leaves intermediates behind: cl puts a .obj in the current directory when
    # /Fo is not given (the package root), and the linkers write .lib/.exp/.ilk/.map next
    # to every .exe.  None of them belong in a delivery.
    junk = {".obj", ".lib", ".exp", ".ilk", ".map", ".pdb", ".iobj", ".ipdb", ".log"}
    keep_libs = (os.path.join(DIST, "ff91-install"), os.path.join(DIST, "deps"))
    for dirpath, dirnames, filenames in os.walk(DIST):
        if any(dirpath == k or dirpath.startswith(k + os.sep) for k in keep_libs):
            continue
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in junk:
                os.remove(os.path.join(dirpath, fn))
    # strip linkers' map/exp files again: they are intermediates, not deliverables
    for dirpath, _dirnames, filenames in os.walk(os.path.join(DIST, "MediaFoundationTransform")):
        if os.path.basename(dirpath) == "obj":
            shutil.rmtree(dirpath)
            continue
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in (".map", ".exp", ".pdb", ".ilk", ".iobj", ".ipdb"):
                os.remove(os.path.join(dirpath, fn))
    for dirpath, _d, filenames in os.walk(os.path.join(DIST, "bin")):
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in (".map",):
                os.remove(os.path.join(dirpath, fn))
    return dll, log


def license_head(path, lines=4):
    if not os.path.exists(path):
        return "(缺失: %s)" % rel(path)
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            t = line.strip()
            if t:
                out.append(t)
            if len(out) >= lines:
                break
    return "\n".join(out)


def write_third_party():
    """Fill the third-party template.  Every number in it is either re-measured from the
    development workspace or the value measured on 2026-11-25; nothing is hand-typed into
    the document by hand."""
    src = os.path.join(ROOT, THIRD_PARTY_TEMPLATE)
    if not os.path.exists(src):
        sys.exit("make-delivery: missing the THIRD-PARTY template %s" % THIRD_PARTY_TEMPLATE)
    text = open(src, encoding="utf-8").read()

    ff_size, ff_sha = identity("ffmpeg-9.1.tar.xz")
    d_size, d_sha = identity("deps/dl/davs2.tar.gz")
    u_size, u_sha = identity("deps/dl/uavs3d.tar.gz")
    sample = os.path.join(DIST, "samples", "CCTV-4-short.avs")
    ported = os.path.join(ROOT, "ffmpeg-9.1", "libavcodec", "libcavs.c")
    ported_head = ""
    if os.path.exists(ported):
        with open(ported, "r", encoding="utf-8", errors="replace") as f:
            ported_head = "".join(f.readline() for _ in range(5)).strip()

    fields = {
        "DATE": DATE,
        "FF_TAG_COMMIT": FFmpeg_TAG_N91_COMMIT,
        "FF_TARBALL_SIZE": ff_size,
        "FF_TARBALL_SHA": ff_sha,
        "DAVS2_SIZE": d_size,
        "DAVS2_SHA": d_sha,
        "UAVS3D_SIZE": u_size,
        "UAVS3D_SHA": u_sha,
        "FF_LICENSE_HEAD": license_head(os.path.join(ROOT, "FFmpeg-AVSPlus", "LICENSE.md"), 6),
        "PORTED_HEAD": ported_head,
        "SAMPLE_SIZE": format(os.path.getsize(sample), ",") if os.path.exists(sample) else "(未打包)",
        "CANARY": CANARY,
    }
    for k, v in fields.items():
        text = text.replace("{{" + k + "}}", v)
    import re as _re
    missing = sorted(set(_re.findall(r"\{\{[A-Z_]+\}\}", text)))
    if missing:
        sys.exit("make-delivery: THIRD-PARTY template placeholders not filled: %s" % ", ".join(missing))
    out = os.path.join(DIST, "THIRD-PARTY.md")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return out


def write_license():
    src = os.path.join(ROOT, LICENSE_TEMPLATE)
    if not os.path.exists(src):
        sys.exit("make-delivery: missing the licence template %s" % LICENSE_TEMPLATE)
    text = open(src, encoding="utf-8").read().replace("{{YEAR}}", DATE[:4])
    with open(os.path.join(DIST, "LICENSE"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def write_gitignore():
    """A git checkout of this delivery should not collect build intermediates.  The list
    mirrors what pack() deletes, so 'removed from the package' and 'ignored by git' stay
    one fact instead of two."""
    lines = [
        "# build intermediates - the same set make-delivery.py strips from the package",
        "*.obj", "*.pdb", "*.ilk", "*.exp", "*.iobj", "*.ipdb", "*.map", "*.log",
        "*.res", "*.tlog", "*.sbr", "obj/",
        "",
        "# tool and harness scratch",
        "tmp/", "out/", "__pycache__/", ".vs/", ".idea/", "*.user",
        "",
        "# what is deliberately NOT ignored: the source, the scripts, the patches,",
        "# the sample, and the delivered DLL",
        "# (MediaFoundationTransform/AVSVideoDecoder/bin/x64/Release/AVSVideoDecoder.dll),",
        "# whose identity is recorded in MANIFEST.md.  Do not add a bare \"x64/\" here:",
        "# it would hide that DLL, and git cannot re-include a file under an ignored",
        "# directory.",
    ]
    with open(os.path.join(DIST, ".gitignore"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_readme():
    src = os.path.join(ROOT, README_TEMPLATE)
    if not os.path.exists(src):
        sys.exit("make-delivery: missing the README template %s" % README_TEMPLATE)
    text = open(src, encoding="utf-8").read().replace("{{DATE}}", DATE)
    with open(os.path.join(DIST, "README.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    vsrc = os.path.join(ROOT, VERIFY_TEMPLATE)
    copy_file(vsrc, os.path.join(DIST, "verify.cmd"))
    # the harness verify.cmd calls is built by build-mft.cmd; a delivery that shipped
    # no copy of it could not be checked without the development workspace.
    copy_file(vsrc, os.path.join(DIST, "scripts", "delivery-verify.cmd"))


def write_gitattributes():
    """Line endings are not cosmetics in this tree: a .cmd file with LF breaks 'goto' and
    'set X=' in cmd.exe, and the project learned that the hard way.  So the checkout is
    told explicitly which kind of file keeps which ending."""
    lines = [
        "* text=auto",
        "# batch scripts must be CRLF in the working tree, or cmd.exe misparses them",
        "*.cmd  text eol=crlf",
        "*.bat  text eol=crlf",
        "# shell scripts must stay LF, or sh reads '\\r' as part of a command",
        "*.sh   text eol=lf",
        "*.py   text eol=lf",
        "# the delivered binary and the sample: never touch bytes",
        "*.dll  binary",
        "*.exe  binary",
        "*.avs  binary",
        "*.avs2 binary",
        "*.avs3 binary",
        "*.ts   binary",
        "*.patch text",
    ]
    with open(os.path.join(DIST, ".gitattributes"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_manifest(packed, built_dll, build_log):
    rows = []
    total = 0
    tree_hash = hashlib.sha256()
    nfiles = 0
    # .git is not source: counting it would make the "source tree fingerprint" change on
    # every commit, which measures the commit and not the tree.  The walk below is
    # sorted(), which consumes os.walk() up front - so pruning dirnames would do nothing
    # here; skip by path component instead.
    def _hidden(dirpath):
        return any(part in HIDDEN for part in dirpath.replace(os.sep, "/").split("/"))

    for dirpath, dirnames, filenames in sorted(os.walk(DIST)):
        if _hidden(dirpath):
            continue
        r = relp(DIST, dirpath)
        for fn in sorted(filenames):
            if fn == "MANIFEST.md":
                continue
            p = os.path.join(dirpath, fn)
            rr = (r + "/" + fn) if r else fn
            size = os.path.getsize(p)
            h = sha256_file(p)
            total += size
            nfiles += 1
            tree_hash.update(("%s %s\n" % (rr, h)).encode())
            if fn.lower().endswith((".dll", ".exe", ".patch", ".cmd", ".sh", ".py", ".xz")):
                rows.append((rr, size, h))

    dll = built_dll or os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder",
                                    "bin", "x64", "Release", "AVSVideoDecoder.dll")
    dll_rel = rel(dll) if os.path.exists(dll) else "(未找到)"
    lines = []
    lines.append("# MANIFEST（%s）\n" % DATE)
    lines.append("生成命令：`scripts\\make-delivery.py%s`（在开发工作区根目录执行）。\n"
                 % (" --build" if built_dll else ""))
    lines.append("- 文件数：%d，合计 %s 字节" % (nfiles, format(total, ",")))
    lines.append("- 全树内容指纹（逐文件 `路径 + SHA-256` 再哈希）：`%s`" % tree_hash.hexdigest())
    lines.append("- 第三方源码（FFmpeg / davs2 / uavs3d / nasm）**不在本包内**：引用与"
                 "我们实际构建的那一份的哈希见 `THIRD-PARTY.md` 与 `deps\\README.md`。")
    lines.append("- 包内的自检样本：`samples\\CCTV-4-short.avs`（广播片段，版权不属于本项目，"
                 "见 `THIRD-PARTY.md` 第 4 节）；解码 canary `%s`。" % CANARY)
    lines.append("- 交付的二进制：`%s`" % dll_rel)
    if os.path.exists(dll):
        lines.append("  - 大小 %s 字节，SHA-256 `%s`"
                     % (format(os.path.getsize(dll), ","), sha256_file(dll)))
    if built_dll:
        lines.append("  - **由本包内的源码构建**：`scripts\\build-mft.cmd` 在 `dist\\` 内执行，退出码 0；"
                     "构建日志（开发工作区）`%s`" % rel(build_log))
    lines.append("")
    lines.append("## 按目录计数\n")
    lines.append("| 目录 | 文件数 | 字节 |")
    lines.append("|---|---|---|")
    groups = {}
    for dirpath, dirnames, filenames in os.walk(DIST):
        if _hidden(dirpath):
            continue
        r = relp(DIST, dirpath)
        top = r.split("/")[0] if r else ""
        for fn in filenames:
            if fn == "MANIFEST.md":
                continue
            g = top if top else "(包根目录)"
            n, b = groups.get(g, (0, 0))
            p = os.path.join(dirpath, fn)
            groups[g] = (n + 1, b + os.path.getsize(p))
    for g in sorted(groups):
        n, b = groups[g]
        lines.append("| `%s` | %s | %s |" % (g, format(n, ","), format(b, ",")))
    lines.append("")
    lines.append("下表列出所有可执行、可安装、可校验的产物（源码文件按上面的目录计数，"
                 "全树逐文件哈希参与开头那条指纹）。校验某一个文件："
                 "`certutil -hashfile <文件> SHA256`。\n")
    lines.append("| 文件 | 字节 | SHA-256 |")
    lines.append("|---|---|---|")
    for rr, size, h in rows:
        lines.append("| `%s` | %s | `%s` |" % (rr, format(size, ","), h))
    with open(os.path.join(DIST, "MANIFEST.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main(argv):
    do_build = "--build" in argv
    print("packing %s -> %s" % (ROOT, DIST))
    packed = pack()
    print("  packed %-28s %d file(s)" % ("files+scripts", len(SINGLE_FILES + BUILD_SCRIPTS + SAMPLES)))
    built_dll, build_log = (None, None)
    if do_build:
        print("== building inside dist (so the shipped DLL is built from the shipped source)")
        built_dll, build_log = build_inside_dist()
        print("   built: %s" % rel(built_dll))
    else:
        src = os.path.join(ROOT, "MediaFoundationTransform", "AVSVideoDecoder",
                           "bin", "x64", "Release", "AVSVideoDecoder.dll")
        if os.path.exists(src):
            copy_file(src, os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder",
                                        "bin", "x64", "Release", "AVSVideoDecoder.dll"))
            print("   copied the working tree's DLL (use --build to build it inside dist)")
    # verify.cmd writes its scratch under tmp/; a fresh package must not carry it
    scratch = os.path.join(DIST, "tmp")
    if os.path.isdir(scratch):
        shutil.rmtree(scratch)

    write_readme()
    write_license()
    write_gitignore()
    write_gitattributes()
    tp = write_third_party()
    write_manifest(packed, built_dll, build_log)
    print("wrote %s, %s, %s" % (rel(os.path.join(DIST, "README.md")), rel(tp),
                                rel(os.path.join(DIST, "MANIFEST.md"))))
    print("DELIVERY=PACKED  - now run scripts/delivery-check.py"
          + " (with the same python that ran this script)")
    print("               third-party sources are referenced, not copied: "
          + "see THIRD-PARTY.md and deps/README.md")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
