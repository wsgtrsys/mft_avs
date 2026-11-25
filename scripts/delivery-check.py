#!/usr/bin/python3
# -*- coding: utf-8 -*-
"""
delivery-check.py - is dist/ still the delivery, and is the packed binary still correct?

This is the guard for the deliverable itself.  Four questions, all mechanical:

  1. every file the packaging plan says belongs there is present and byte-identical to
     the working tree (a delivery that drifts from the source is a lie);
  2. nothing extra is in dist/ except generated documents and build products
     (a leftover from an older pack looks like part of the delivery);
  3. the project file in dist/ references only files that are in dist/
     (the usual way a source drop fails to build for somebody else);
  4. the delivered DLL still decodes byte-for-byte like the reference, and survives the
     lifecycle test - asked of the DLL *inside* dist/ with *dist's own* tools, with no
     environment switches, i.e. what a player gets.

    scripts\\delivery-check.py
"""
import hashlib
import importlib.util
import os
import re
import subprocess
import sys

# make-delivery.py owns the packaging plan; import it by path (its name has a hyphen) so
# that the check and the packer can never disagree about what belongs in the package.
# importing a module writes __pycache__ next to it; that directory would then be
# reported as a stale file inside the package we are checking
sys.dont_write_bytecode = True

_HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("make_delivery",
                                              os.path.join(_HERE, "make-delivery.py"))
M = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(M)

ROOT = M.ROOT
DIST = M.DIST

# written by make-delivery.py, or produced by building inside dist/
GENERATED = {"README.md", "THIRD-PARTY.md", "MANIFEST.md", "verify.cmd",
             "scripts/delivery-verify.cmd", "LICENSE", ".gitignore", ".gitattributes"}
BUILD_PRODUCT = re.compile(r"^(bin/.*\.exe|MediaFoundationTransform/.*/bin/.*|"
                           r"ff91-install/(bin|lib|include)/.*|deps/bin/.*)$")

REBUILT = re.compile(r"^(bin/.*|MediaFoundationTransform/.*/bin/.*)$")

# one definition of the byte-level canary: make-delivery.py writes it into
# THIRD-PARTY.md, this script reproduces it.  Two copies would drift.
CANARY = M.CANARY
# the package carries the sample now, so the byte-level check runs on the file that
# ships with it - not on a file only the development workspace happens to have.
FIXTURE = os.path.join(DIST, "samples", "CCTV-4-short.avs")

# the components that are referenced rather than shipped, and the string that must
# appear in the package's own documents for the reference to be actionable
UPSTREAM = {
    "FFmpeg": "https://github.com/FFmpeg/FFmpeg",
    "davs2": "https://github.com/pkuvcl/davs2",
    "uavs3d": "https://github.com/uavs3/uavs3d",
}

problems = 0


def ok(msg):
    print("ok      %s" % msg)


def bad(msg):
    global problems
    problems += 1
    print("PROBLEM %s" % msg)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    if not os.path.isdir(DIST):
        print("delivery-check: NOT BUILT - there is no %s" % M.rel(DIST))
        print("              build it with:  scripts\\make-delivery.py --build")
        return 2

    # ---------------------------------------------------------- 1. plan vs. contents
    planned = M.plan()
    missing, differs = 0, 0
    planned_dst = set()
    rebuilt = 0
    for src_rel, dst_rel in planned:
        planned_dst.add(dst_rel)
        if REBUILT.match(dst_rel):
            # the in-package build rewrites these; they are verified by running them in
            # section 5, not by comparing bytes with the development workspace
            rebuilt += 1
            continue
        a = os.path.join(ROOT, src_rel.replace("/", os.sep))
        b = os.path.join(DIST, dst_rel.replace("/", os.sep))
        if not os.path.exists(a):
            bad("the packaging plan points at a file that no longer exists: %s" % src_rel)
            missing += 1
            continue
        if not os.path.exists(b):
            bad("not in the delivery: %s" % dst_rel)
            missing += 1
            continue
        if sha256_file(a) != sha256_file(b):
            bad("the copy in dist/ differs from the working tree: %s" % dst_rel)
            differs += 1
    if not missing and not differs:
        ok("packed source: %d file(s) present and byte-identical to the working tree; "
           "%d file(s) in the build output directories are not byte-compared "
           "because the in-package build writes there - they are checked by running them"
           % (len(planned) - rebuilt, rebuilt))

    # -------------------------------------------------------------- 2. nothing extra
    extra = []
    for dirpath, dirnames, filenames in os.walk(DIST):
        r = M.relp(DIST, dirpath)
        if os.path.basename(dirpath) in ("obj", "tmp", "__pycache__", ".git"):
            dirnames[:] = []   # obj = build intermediate, tmp = verify.cmd's scratch
            continue
        for fn in filenames:
            rr = (r + "/" + fn) if r else fn
            if rr in planned_dst or rr in GENERATED or BUILD_PRODUCT.match(rr):
                continue
            extra.append(rr)
    if extra:
        for e in extra[:10]:
            bad("in dist/ but not in the packaging plan (stale file?): %s" % e)
    else:
        ok("no stale files in dist/ (every file is planned, generated, or a build product)")

    # ------------------------------------------- 3. the project file builds from dist
    proj = os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder",
                        "AVSVideoDecoder.vcxproj")
    if not os.path.exists(proj):
        bad("no project file in the delivery: %s" % M.rel(proj))
    else:
        text = open(proj, encoding="utf-8", errors="replace").read()
        refs = re.findall(r"<(?:ClCompile|ClInclude|ResourceCompile|Midl|None)\s+Include=\"([^\"]+)\"",
                         text)
        unresolved = []
        for r_ in refs:
            if "$(" in r_ or "*" in r_:
                continue
            p = os.path.normpath(os.path.join(os.path.dirname(proj), r_.replace("\\", os.sep)))
            if not os.path.exists(p):
                unresolved.append(r_)
        if unresolved:
            for u in unresolved[:10]:
                bad("the .vcxproj includes %s, which is not in the delivery" % u)
        else:
            ok("the .vcxproj's %d listed file(s) are all present in the delivery" % len(refs))
        # FFmpegDir / DepsDir: the package does not carry those trees any more, it names
        # where to get them.  A path that resolves to nothing is only acceptable when the
        # package says out loud where the input comes from - otherwise a build failure in
        # the package would be a mystery.
        docs = ""
        for name in ("deps/README.md", "THIRD-PARTY.md"):
            p = os.path.join(DIST, name.replace("/", os.sep))
            if os.path.exists(p):
                docs += open(p, encoding="utf-8", errors="replace").read()
        missing_refs = [who for who, url in UPSTREAM.items()
                        if url.replace("https://", "") not in docs.replace("https://", "")]
        if missing_refs:
            bad("the package references no source for %s: neither deps/README.md nor "
                "THIRD-PARTY.md names the upstream repository" % ", ".join(missing_refs))
        else:
            ok("the three referenced components name their upstream repository inside the "
               "package: %s" % ", ".join(sorted(UPSTREAM)))
        # the vendored Microsoft MFT sample ships its own licence file; a package that
        # publishes "our own code is BSD-3" has to point at it, or it overstates what is
        # ours.  Searched in the same two documents as the references above.
        vend = os.path.join(DIST, "MediaFoundationTransform", "LICENSE")
        needle1 = "MediaFoundationTransform" + os.sep + "LICENSE"
        needle2 = "MediaFoundationTransform/LICENSE"
        if os.path.exists(vend):
            if needle1 in docs or needle2 in docs:
                ok("the terms of the vendored MediaFoundationTransform sample are named in "
                   "the package's own documents, so BSD-3 is not claimed for those files")
            else:
                bad("the package ships MediaFoundationTransform" + os.sep + "LICENSE "
                    "(someone else's terms) but neither THIRD-PARTY.md nor deps/README.md "
                    "mentions it")
        for name in ("FFmpegDir", "DepsDir"):
            m = re.search(r"<%s>([^<]*)</%s>" % (name, name), text)
            if not m:
                bad("%s is not defined in the delivered .vcxproj" % name)
                continue
            target = os.path.normpath(os.path.join(
                os.path.dirname(proj), m.group(1).replace("\\", os.sep).replace("$(ProjectDir)", "")))
            # a directory that exists is not enough: what matters is the file the build
            # reads out of it.  dist/deps exists because deps/README.md is in the package;
            # that does not mean davs2.lib is.
            marker = {"FFmpegDir": os.path.join("include", "libavcodec", "avcodec.h"),
                      "DepsDir": os.path.join("lib", "davs2.lib")}.get(name)
            if os.path.isdir(target) and marker and os.path.exists(os.path.join(target, marker)):
                ok("%s resolves inside the delivery and carries %s: %s"
                   % (name, M.rel(os.path.join(target, marker)), M.rel(target)))
            elif os.path.isdir(target):
                print("note    %s points at %s, which is in the package but does not carry "
                      "%s - build it first with scripts\\build-deps.cmd / scripts\\build-ffmpeg.cmd "
                      "(where the sources come from: deps\\README.md)"
                      % (name, M.rel(target), M.rel(os.path.join(target, marker))))
            else:
                print("note    %s resolves outside the package (%s): build it first with "
                      "scripts\\build-ffmpeg.cmd and scripts\\build-deps.cmd; where the "
                      "sources come from is written in deps\\README.md" % (name, M.rel(target)))

    # ---------------------------------------------------------- 4. patch and base
    patch = os.path.join(DIST, "patches", "ffmpeg-9.1-avs-dra.patch")
    tp = os.path.join(DIST, "THIRD-PARTY.md")
    if not os.path.exists(patch):
        bad("the FFmpeg side of the delivery is incomplete: no patch")
    else:
        head = open(patch, encoding="utf-8", errors="replace").readline().strip()
        m = re.match(r"diff --git a/([^/]+)/", head)
        if not m or m.group(1) != "ffmpeg-9.1":
            bad("the patch rewrites %r, which is not the 9.1 base the package names" % head)
        nfiles = open(patch, encoding="utf-8", errors="replace").read().count("diff --git")
        ok("the patch targets the 9.1 source tree and touches %d file(s)" % nfiles)
    # the base is a reference now: the check is that the reference is specific enough to
    # be acted on (repository + tag + the hash of the exact archive we compiled)
    if os.path.exists(tp):
        text = open(tp, encoding="utf-8", errors="replace").read()
        size, sha = M.identity("ffmpeg-9.1.tar.xz")
        for label, needle in (("the FFmpeg repository", UPSTREAM["FFmpeg"]),
                              ("the n9.1 tag's commit", M.FFmpeg_TAG_N91_COMMIT),
                              ("the exact archive's SHA-256", sha)):
            if needle not in text:
                bad("THIRD-PARTY.md does not name %s (%s), so the base of the patch "
                    "cannot be re-fetched identically" % (label, needle))
        if os.path.exists(os.path.join(DIST, "ffmpeg-9.1.tar.xz")):
            bad("the package still carries ffmpeg-9.1.tar.xz - it is a reference now, "
                "not a file to redistribute")
        if UPSTREAM["FFmpeg"] in text and M.FFmpeg_TAG_N91_COMMIT in text and sha in text:
            ok("the FFmpeg base is a checkable reference: FFmpeg/FFmpeg tag n9.1 "
               "(commit %s), and the archive we compiled is pinned by SHA-256 %s"
               % (M.FFmpeg_TAG_N91_COMMIT[:12] + "...", sha[:12] + "..."))
    else:
        bad("no THIRD-PARTY.md in the delivery: the referenced components would be "
            "unnamed")

    # -------------------------------- what the shipped build script can actually rebuild
    bm = os.path.join(DIST, "scripts", "build-mft.cmd")
    tbin = os.path.join(DIST, "bin")
    if os.path.exists(bm):
        # build-mft.cmd names every tool it compiles with a /Fe switch that writes into the
# bin directory, so the set of tools the shipped script can rebuild is read out of it.
        marker = "/Fe:" + chr(34) + "%BIN%" + chr(92)
        txt = open(bm, encoding="utf-8", errors="replace").read()
        built = set()
        for piece in txt.split(marker)[1:]:
            name = piece.split(chr(34))[0]
            if name.lower().endswith(".exe"):
                name = name[:-4]
            if name:
                built.add(name)
        shipped = [fn for fn in sorted(os.listdir(tbin))
                   if fn.lower().endswith(".exe")] if os.path.isdir(tbin) else []
        as_is = [s for s in shipped if s not in set(b + ".exe" for b in built)]
        if built and as_is:
            print("note    the shipped build-mft.cmd rebuilds %d of the %d tool(s) in bin/; "
                  "these %d are shipped as-is from the development workspace (their sources "
                  "are in tools/, the shipped script does not compile them): %s"
                  % (len(built), len(shipped), len(as_is), ", ".join(as_is)))
        elif built:
            ok("every tool in bin/ is rebuilt by the build-mft.cmd shipped in the package (%d)"
               % len(built))

    # ------------------------------------------------------- 5. the binary still works
    dll = os.path.join(DIST, "MediaFoundationTransform", "AVSVideoDecoder",
                       "bin", "x64", "Release", "AVSVideoDecoder.dll")
    tools_bin = os.path.join(DIST, "bin")
    if not os.path.exists(dll):
        bad("the delivery has no built DLL: %s" % M.rel(dll))
    elif not os.path.exists(os.path.join(tools_bin, "avsmft-test.exe")):
        bad("the delivery has no console harness (bin/avsmft-test.exe), so the DLL in it "
            "cannot be checked without the development workspace")
    elif not os.path.exists(FIXTURE):
        print("delivery-check: STOPPED - the byte-level check needs %s, which is not "
              "here.  The delivery is not verified; this is not a pass."
              % M.rel(FIXTURE))
        return 2
    else:
        out = os.path.join(ROOT, "tmp", "delivery_check_mft.yuv")
        log = os.path.join(ROOT, "tmp", "delivery_check_decode.log")
        with open(log, "wb") as f:
            rc = subprocess.call([os.path.join(tools_bin, "avsmft-test.exe"), FIXTURE, out,
                                  "-mft", dll, "-fmt", "i420"],
                                 stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        got = hashlib.md5(open(out, "rb").read()).hexdigest() if os.path.exists(out) else "(none)"
        if rc == 0 and got == CANARY:
            ok("the delivered DLL (%s bytes) decodes %s to exactly the reference bytes "
               "(md5 %s)" % (format(os.path.getsize(dll), ","), M.rel(FIXTURE), got))
        else:
            bad("the delivered DLL does not reproduce the reference bytes: exit=%s md5=%s "
                "want=%s (%d byte(s) out), log %s"
                % (rc, got, CANARY, os.path.getsize(out) if os.path.exists(out) else 0,
                   M.rel(log)))
        log2 = os.path.join(ROOT, "tmp", "delivery_check_lifecycle.log")
        lc = os.path.join(tools_bin, "mftlifecycle-test.exe")
        if os.path.exists(lc):
            with open(log2, "wb") as f:
                rc2 = subprocess.call([lc, dll, "-repeat", "10", "-file", FIXTURE],
                                      stdout=f, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL)
            tail = open(log2, encoding="utf-8", errors="replace").read().strip().splitlines()
            if rc2 == 0:
                ok("lifecycle through the delivered DLL: %s" % (tail[-1] if tail else "exit 0"))
            else:
                bad("the delivered DLL failed the lifecycle test (exit %s): %s"
                    % (rc2, tail[-1] if tail else ""))

    # --------------------------------------- 6. "deploy one file" is a checkable claim
    # use the script next to this one: the packaged copy is byte-compared
    # against the working tree in section 1, so it is the same file
    imp = os.path.join(_HERE, "dll-imports.py")
    dll_for_imports = dll
    if os.path.exists(imp) and os.path.exists(dll):
        spec = importlib.util.spec_from_file_location("dll_imports", imp)
        DI = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(DI)
        try:
            mods = DI.imports(dll)
        except Exception as e:                       # noqa: BLE001 - a parser failing is a finding
            mods = None
            bad("could not read the delivered DLL's import table: %s" % e)
        if mods is not None:
            sysdir = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
            real = [m for m in sorted(set(mods))
                    if not m.lower().startswith(("api-ms-win-", "ext-ms-win-", "ms-win-"))]
            outside = [m for m in real if not os.path.exists(os.path.join(sysdir, m))]
            if outside:
                bad("the delivered DLL imports %s, which is not a system module - the "
                    "delivery would need more than one file" % ", ".join(outside))
            else:
                ok("the delivered DLL imports %d module(s), all of them system modules "
                   "(no CRT, no FFmpeg/davs2/uavs3d DLL)" % len(real))
            kinds = DI.resources(dll)
            if 16 not in kinds:
                print("note    the delivered DLL has no RT_VERSION resource: once it is "
                      "registered, nothing in the file says which build it is - only the "
                      "hash in MANIFEST.md does. A delivery gap, not a decode defect.")
    elif not os.path.exists(imp):
        bad("the delivery does not contain scripts/dll-imports.py, so the claim that one "
            "file is enough cannot be checked inside the package")

    # ---------------------------------------------------------- 7. not stale
    # "the binary was built from the source in the package" can only be asked of the
    # sources that link into that binary.  tools/ compiles into separate .exe files:
    # a newer tools/*.cpp does not make the delivered DLL stale, it makes the harness
    # possibly older than its source - a different fact, reported separately.
    if os.path.exists(dll):
        t_dll = os.path.getmtime(dll)
        stale = []
        for dirpath, dirnames, filenames in os.walk(DIST):
            if ".git" in dirnames:
                dirnames.remove(".git")
            r = M.relp(DIST, dirpath)
            if not r.startswith("MediaFoundationTransform"):
                continue
            for fn in filenames:
                if os.path.splitext(fn)[1].lower() not in (".cpp", ".h", ".c", ".vcxproj"):
                    continue
                p = os.path.join(dirpath, fn)
                if os.path.getmtime(p) > t_dll:
                    stale.append(M.rel(p))
        if stale:
            bad("the delivered DLL is older than %d source file(s) that link into it, "
                "e.g. %s - pack it again with scripts\\make-delivery.py --build"
                % (len(stale), stale[0]))
        else:
            ok("the delivered DLL is not older than any source that links into it "
               "(MediaFoundationTransform/, %d source file(s))"
               % sum(1 for dp, dn, fns in os.walk(os.path.join(DIST, "MediaFoundationTransform"))
                     for fn in fns
                     if os.path.splitext(fn)[1].lower() in (".cpp", ".h", ".c", ".vcxproj")))
        exes = [os.path.join(tools_bin, fn) for fn in os.listdir(tools_bin)
                if fn.lower().endswith(".exe")] if os.path.isdir(tools_bin) else []
        if exes:
            t_tool = max(os.path.getmtime(p) for p in exes)
            newer = []
            for dirpath, dirnames, filenames in os.walk(os.path.join(DIST, "tools")):
                for fn in filenames:
                    if os.path.splitext(fn)[1].lower() in (".cpp", ".h"):
                        p = os.path.join(dirpath, fn)
                        if os.path.getmtime(p) > t_tool:
                            newer.append(M.rel(p))
            if newer:
                print("note    %d tool source file(s) in the package are newer than the "
                      "shipped bin/*.exe (%s): the console harness is a separate binary, so "
                      "the delivered DLL is unaffected, but the harness in the package is "
                      "not built from that source - rebuild it with scripts\\build-mft.cmd "
                      "if you need it to be" % (len(newer), ", ".join(newer[:3])))

    print()
    if problems:
        print("DELIVERY=FAILED - %d problem(s)" % problems)
        return 1
    print("DELIVERY=OK - dist/ equals the current source, builds from what it contains, "
          "and the binary in it decodes byte-for-byte like the reference.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
