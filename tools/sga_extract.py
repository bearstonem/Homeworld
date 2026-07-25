"""Reader for Relic SGA v2 archives ("_ARCHIVE") as shipped in the Homeworld
Remastered Collection, for pulling remastered backgrounds and audio into this
port. Extracts only - nothing from the Remastered Collection is redistributed
here; you need your own copy.

Usage:  python3 tools/sga_extract.py <archive.big> [outdir]
        (no outdir = list contents)

Minimal reader for Relic SGA v2 archives ("_ARCHIVE"), as shipped in the
Homeworld Remastered Collection. Layout worked out empirically from
HWBackgrounds.big; file records are 17 bytes, not the 20 used by other SGA
versions."""
import struct, zlib, sys, os

class SGA:
    def __init__(self, path):
        self.path = path
        f = self.f = open(path, 'rb')
        head = f.read(180)
        assert head[:8] == b'_ARCHIVE', "not an SGA archive"
        self.major, self.minor = struct.unpack_from('<HH', head, 8)
        self.name = head[28:156].decode('utf-16-le', 'replace').rstrip('\x00')
        self.hdrLen, self.dataOff = struct.unpack_from('<II', head, 172)
        self.HS = 180
        f.seek(0); d = self.d = f.read(self.HS + self.hdrLen)
        o = self.HS; self.sec = {}
        for nm in ("toc", "folder", "file", "string"):
            off, cnt = struct.unpack_from('<IH', d, o); o += 6
            self.sec[nm] = (off, cnt)

    def _str(self, rel):
        so, _ = self.sec["string"]
        base = self.HS + so + rel
        e = self.d.index(b'\0', base)
        return self.d[base:e].decode('utf-8', 'replace')

    def folders(self):
        fo, fc = self.sec["folder"]
        for i in range(fc):
            nameOff, sf, ef, sfi, efi = struct.unpack_from('<IHHHH', self.d, self.HS + fo + i*12)
            yield self._str(nameOff), sfi, efi

    def files(self):
        flo, flc = self.sec["file"]
        for i in range(flc):
            rec = self.HS + flo + i*17
            # 17-byte record: u32 nameOffset, u8 flags, u32 dataOffset,
            # u32 lengthCompressed, u32 lengthUncompressed. The lone byte at
            # +4 is what makes the record an odd 17 wide and is why naive
            # 16- or 20-byte strides mis-parse this archive.
            nameOff = struct.unpack_from("<I", self.d, rec)[0]
            flags   = self.d[rec+4]
            doff, clen, ulen = struct.unpack_from("<III", self.d, rec+5)
            yield i, self._str(nameOff), doff, clen, ulen

    def paths(self):
        """file index -> full path, using folder ranges"""
        out = {}
        for fname, s, e in self.folders():
            for i in range(s, e):
                out[i] = fname
        return out

    def read(self, doff, clen, ulen):
        self.f.seek(self.dataOff + doff)
        blob = self.f.read(clen)
        if clen == ulen:
            return blob
        return zlib.decompress(blob)

if __name__ == '__main__':
    a = SGA(sys.argv[1])
    outdir = sys.argv[2] if len(sys.argv) > 2 else None
    print(f"# {a.name}  v{a.major}.{a.minor}  files={a.sec['file'][1]}")
    dirs = a.paths()
    for i, nm, doff, clen, ulen in a.files():
        rel = dirs.get(i, '').replace('\\', '/')
        if outdir is None:
            print(f"{rel+'/'+nm:60s} {ulen:10d} {'raw' if clen == ulen else 'zlib'}")
            continue
        dst = os.path.join(outdir, rel, nm)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as fh:
            fh.write(a.read(doff, clen, ulen))
        print("extracted", dst)
