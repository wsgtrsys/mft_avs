# -*- coding: utf-8 -*-
# The delivery README says the DLL needs no extra runtime DLLs.  That is checkable: the
# PE import table lists exactly which modules the loader will ask for, and each of them
# either is in System32 or the claim is false.
import os
import struct
import sys

CANDIDATES = [
    os.path.join("dist", "MediaFoundationTransform", "AVSVideoDecoder", "bin", "x64",
                 "Release", "AVSVideoDecoder.dll"),
    os.path.join("MediaFoundationTransform", "AVSVideoDecoder", "bin", "x64", "Release",
                 "AVSVideoDecoder.dll"),
]


def imports(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[pe:pe + 4] == b"PE\0\0", "not a PE"
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)          # PE32+ vs PE32
    imp_rva, imp_size = struct.unpack_from("<II", d, dd + 8)
    secs = []
    for i in range(nsec):
        s = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0] + i * 40
        vaddr, vsize, raw = struct.unpack_from("<III", d, s + 12)
        rawptr = struct.unpack_from("<I", d, s + 20)[0]
        secs.append((vaddr, vsize, rawptr))

    def r2o(rva):
        for vaddr, vsize, rawptr in secs:
            if vaddr <= rva < vaddr + max(vsize, 0x1000):
                return rawptr + (rva - vaddr)
        raise ValueError("RVA %x not in any section" % rva)

    out, off = [], r2o(imp_rva)
    while True:
        name_rva = struct.unpack_from("<I", d, off + 12)[0]
        if name_rva == 0:
            break
        p = r2o(name_rva)
        end = d.index(b"\0", p)
        out.append(d[p:end].decode("ascii", "replace"))
        off += 20
    return out



def resources(path):
    """Resource types present in the PE, as {id: entries}. RT_VERSION (16) is what answers
    "which build is registered?"; without it the binary can only be identified by hash."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (112 if magic == 0x20B else 96)
    rva, _size = struct.unpack_from("<II", d, dd + 16)     # data directory entry 2 = .rsrc
    if rva == 0:
        return {}
    secs = []
    for i in range(nsec):
        s = pe + 24 + struct.unpack_from("<H", d, pe + 20)[0] + i * 40
        vaddr, vsize, rawptr = struct.unpack_from("<III", d, s + 12)
        secs.append((vaddr, max(vsize, 0x1000), rawptr))

    def r2o(r):
        for v, vs, rp in secs:
            if v <= r < v + vs:
                return rp + (r - v)
        raise ValueError("bad RVA")

    b = r2o(rva)
    named = struct.unpack_from("<H", d, b + 12)[0]
    ids = struct.unpack_from("<H", d, b + 14)[0]
    kinds = {}
    off = b + 16
    for _ in range(named + ids):
        resid, ofs = struct.unpack_from("<II", d, off)
        sub = b + (ofs & 0x7FFFFFFF)
        kinds[resid] = (struct.unpack_from("<H", d, sub + 12)[0] +
                        struct.unpack_from("<H", d, sub + 14)[0])
        off += 8
    return kinds


RES_NAMES = {16: "RT_VERSION", 24: "RT_MANIFEST", 14: "RT_GROUP_ICON", 3: "RT_ICON",
             10: "RT_RCDATA", 2: "RT_BITMAP", 1: "RT_CURSOR"}

def main():
    path = next((p for p in CANDIDATES if os.path.exists(p)), None)
    if not path:
        print("import-table: no DLL found (build the delivery first)")
        return 2
    sysdir = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    names = imports(path)
    print("%s imports %d module(s):" % (path, len(names)))
    missing = []
    for n in sorted(set(names)):
        low = n.lower()
        if low.startswith(("api-ms-win-", "ext-ms-win-", "ms-win-")):
            print("   %-38s API set (resolved by the OS, no file to ship)" % n)
            continue
        here = os.path.exists(os.path.join(sysdir, n))
        print("   %-38s %s" % (n, "a system module" if here else "NOT a system module"))
        if not here:
            missing.append(n)
    kinds = resources(path)
    print("resources present: %s" %
          (", ".join("%s(%d)" % (RES_NAMES.get(k, "id=%d" % k), n)
                     for k, n in sorted(kinds.items())) or "none"))
    if 16 in kinds:
        print("   the DLL carries RT_VERSION: the registered binary can be identified from "
              "its file properties")
    else:
        print("   NO RT_VERSION: after registration the binary can only be identified by its "
              "hash - a delivery gap, not a decode defect")
    print()
    if missing:
        print("import-table: %d module(s) are not system modules: %s" % (len(missing), missing))
        return 1
    print("import-table: every module the loader will ask for is a system module - "
          "the delivered DLL needs no extra runtime files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
