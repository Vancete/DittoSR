# Writes src/xess_forward.h: every export of the given libxess.dll files (Intel XeSS, any versions - their union), but
# the ones src/xess_dll.cpp defines, forwarded by name to the game's own XeSS renamed libxess_orig.dll. Run it when a
# newer XeSS adds exports (the loader refuses a game's libxess.dll with exports the proxy lacks: the game wouldn't load).
#   python scripts/xess_exports.py <libxess.dll> [<libxess.dll> ...]
import os
import struct
import sys

OWN = {"xessD3D12CreateContext", "xessD3D12Init", "xessD3D12Execute", "xessDestroyContext", "xessSetVelocityScale",
       "xessSetJitterScale", "xessSetExposureMultiplier", "xessSetMaxResponsiveMaskValue"}


def exports(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    dirs = opt + (112 if struct.unpack_from("<H", d, opt)[0] == 0x20B else 96)
    sec = opt + struct.unpack_from("<H", d, pe + 20)[0]
    secs = [struct.unpack_from("<8sIIII", d, sec + 40 * i) for i in range(nsec)]

    def at(rva):
        for _, vs, va, rs, ro in secs:
            if va <= rva < va + max(vs, rs):
                return rva - va + ro
        raise ValueError("rva %x" % rva)

    e = at(struct.unpack_from("<I", d, dirs)[0])
    count, names = struct.unpack_from("<I", d, e + 24)[0], at(struct.unpack_from("<I", d, e + 32)[0])
    out = []
    for i in range(count):
        p = at(struct.unpack_from("<I", d, names + 4 * i)[0])
        out.append(d[p:d.index(b"\0", p)].decode())
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__ or "usage: xess_exports.py <libxess.dll>...")
    names = sorted(set(n for p in sys.argv[1:] for n in exports(p)))
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "xess_forward.h")
    with open(out, "w", newline="\n") as f:
        f.write("// libxess.dll: every XeSS export xess_dll.cpp doesn't define, forwarded by name to the game's own (libxess_orig.dll).\n")
        f.write("// Written by scripts/xess_exports.py: the %d exports of %d libxess.dll (XeSS 2.0.1 and 2.0.2 the last time). The\n"
                % (len(names), len(sys.argv) - 1))
        f.write("// loader checks a game's libxess.dll has no other before installing.\n#pragma once\n")
        for n in names:
            if n not in OWN:
                f.write('#pragma comment(linker, "/export:%s=libxess_orig.%s")\n' % (n, n))
    print("%d exports, %d forwarded: %s" % (len(names), len(names) - len(OWN & set(names)), out))


if __name__ == "__main__":
    main()
