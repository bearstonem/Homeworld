"""Reader for Homeworld 1 .big archives (RBF 1.23), including the variant
packaged with the Remastered Collection.

Filenames are not stored in the TOC - only CRCs of each half plus a length -
but the name does precede each file's data, XOR-obfuscated, so it can be
recovered while reading. The Remastered packaging widens the TOC entry by one
udword; BigFile.c detects that by its 13887-file count and this does the same.

Usage:
    python3 tools/big_extract.py <archive.big> [outdir] [--filter substring]
    (no outdir = list contents)
"""
import os
import struct
import sys
import zlib

HEADER = b"RBF1.23"



# LZSS as used by the .big files - Nelson's variant, ported from
# src/ThirdParty/LZSS. Not zlib: `compressionType` is this. Bits are read
# MSB-first (BitIO.c starts its mask at 0x80).
INDEX_BIT_COUNT = 12
LENGTH_BIT_COUNT = 4
WINDOW_SIZE = 1 << INDEX_BIT_COUNT
BREAK_EVEN = (1 + INDEX_BIT_COUNT + LENGTH_BIT_COUNT) // 9
END_OF_STREAM = 0


class _Bits:
    def __init__(self, data):
        self.d = data
        self.i = 0
        self.rack = 0
        self.mask = 0x80

    def bit(self):
        if self.mask == 0x80:
            self.rack = self.d[self.i] if self.i < len(self.d) else 0
            self.i += 1
        v = self.rack & self.mask
        self.mask >>= 1
        if self.mask == 0:
            self.mask = 0x80
        return 1 if v else 0

    def bits(self, n):
        out = 0
        for _ in range(n):
            out = (out << 1) | self.bit()
        return out


def lzss_expand(data, expected=None):
    b = _Bits(data)
    window = bytearray(WINDOW_SIZE)
    out = bytearray()
    pos = 1
    while True:
        if expected is not None and len(out) >= expected:
            break
        if b.i > len(data) + 4:
            break                                   # ran off the end
        if b.bit():
            c = b.bits(8)
            out.append(c)
            window[pos] = c
            pos = (pos + 1) & (WINDOW_SIZE - 1)
        else:
            mp = b.bits(INDEX_BIT_COUNT)
            if mp == END_OF_STREAM:
                break
            ml = b.bits(LENGTH_BIT_COUNT) + BREAK_EVEN
            for k in range(ml + 1):
                c = window[(mp + k) & (WINDOW_SIZE - 1)]
                out.append(c)
                window[pos] = c
                pos = (pos + 1) & (WINDOW_SIZE - 1)
    return bytes(out)


class Big:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(7)
        if head != HEADER:
            raise ValueError("not an RBF1.23 archive: %r" % head)
        self.numFiles, self.flags = struct.unpack("<II", self.f.read(8))
        # The Remastered build of homeworld.big carries an extra udword per
        # entry. BigFile.c keys off the exact file count; do the same rather
        # than guess from sizes.
        self.remastered = (self.numFiles == 13887)
        # Sizes are the C structs *with padding*, not the sum of their fields:
        # the unsigned short at offset 8 pads out to 12 before the next udword.
        # 36 for the remastered entry (which carries one extra udword), 32 for
        # the original. Confirmed against the archive rather than assumed.
        self.entrySize = 36 if self.remastered else 32
        self.entries = []
        raw = self.f.read(self.entrySize * self.numFiles)
        for i in range(self.numFiles):
            o = i * self.entrySize
            crc1, crc2, nameLen = struct.unpack_from("<IIH", raw, o)
            stored, real, offset, stamp = struct.unpack_from("<IIII", raw, o + 12)
            comp = raw[o + 32] if self.remastered else raw[o + 28]
            self.entries.append(dict(crc1=crc1, crc2=crc2, nameLen=nameLen,
                                     stored=stored, real=real, offset=offset,
                                     comp=comp))

    def name_of(self, e):
        """The filename precedes the data, obfuscated by a CHAINED xor: the
        mask starts at 213 and then becomes each decrypted character in turn
        (bigFilenameDecrypt in BigFile.c). A constant xor decodes nothing."""
        self.f.seek(e["offset"])
        raw = self.f.read(e["nameLen"])
        out = bytearray()
        mask = 213
        for b in raw:
            c = b ^ mask
            out.append(c)
            mask = c
        return out.decode("latin-1", "replace")

    def read(self, e):
        self.f.seek(e["offset"] + e["nameLen"] + 1)
        blob = self.f.read(e["stored"])
        if e["comp"]:
            blob = lzss_expand(blob, e["real"])
        return blob


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    outdir = None
    filt = None
    args = sys.argv[2:]
    if "--filter" in args:
        i = args.index("--filter")
        filt = args[i + 1].lower()
        del args[i:i + 2]
    if args:
        outdir = args[0]

    b = Big(path)
    print("# %s  %d files  (remastered TOC: %s)"
          % (os.path.basename(path), b.numFiles, b.remastered))
    shown = 0
    for e in b.entries:
        try:
            nm = b.name_of(e)
        except Exception:
            continue
        if filt and filt not in nm.lower():
            continue
        shown += 1
        if outdir is None:
            print("%-60s %9d %s" % (nm, e["real"], "zlib" if e["comp"] else "raw"))
            continue
        dst = os.path.join(outdir, nm.replace("\\", "/").lstrip("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(b.read(e))
        print("extracted", dst)
    print("# %d shown" % shown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
