"""Read and write Homeworld 1 .lif textures ("Willy 7").

Layout, confirmed by exact size match on every mothership texture:

    48 bytes    header (ident[8], version, flags, width, height,
                        paletteCRC, imageCRC, and four offsets)
    w*h         one byte per pixel, an index into the palette
    1024        palette, 256 entries of RGBA
    256         teamEffect0 - an index->index remap, NOT a palette
    256         teamEffect1

That teamEffect detail is what makes retexturing safe: team colours are
applied by remapping palette *indices*, so as long as the palette and the two
remap tables are carried through untouched, a replacement image keeps its team
colouring. Only the index grid needs to change.

Which also sets the constraint: a replacement can only use colours already in
the original palette. write_lif() handles that by mapping each pixel to its
nearest palette entry, so an upscaled or repainted image is quantised back
into the palette it must live in.
"""
import struct

HEADER_SIZE = 48
PALETTE_ENTRIES = 256


class Lif:
    def __init__(self, blob):
        if blob[:7] != b"Willy 7":
            raise ValueError("not a Willy 7 .lif")
        self.ident = blob[:8]
        (self.version, self.flags, self.width, self.height,
         self.paletteCRC, self.imageCRC) = struct.unpack_from("<iiiiII", blob, 8)
        (self.o_data, self.o_pal,
         self.o_te0, self.o_te1) = struct.unpack_from("<IIII", blob, 32)

        n = self.width * self.height
        o = HEADER_SIZE
        self.indices = bytearray(blob[o:o + n]);            o += n
        pal = blob[o:o + PALETTE_ENTRIES * 4];              o += PALETTE_ENTRIES * 4
        self.palette = [tuple(pal[i * 4:i * 4 + 4]) for i in range(PALETTE_ENTRIES)]
        self.teamEffect0 = bytearray(blob[o:o + 256]);      o += 256
        self.teamEffect1 = bytearray(blob[o:o + 256])
        self.tail = blob[o + 256:]                          # anything trailing

    def to_rgba(self):
        """Flat RGBA bytes, row 0 first."""
        out = bytearray(self.width * self.height * 4)
        for i, idx in enumerate(self.indices):
            r, g, b, a = self.palette[idx]
            out[i * 4:i * 4 + 4] = bytes((r, g, b, a))
        return bytes(out)

    def pack(self):
        head = bytearray(HEADER_SIZE)
        head[0:8] = self.ident
        struct.pack_into("<iiiiII", head, 8, self.version, self.flags,
                         self.width, self.height, self.paletteCRC, self.imageCRC)
        # Offsets are stored relative to the end of the header, and the engine
        # fixes them up on load; recompute so a resize stays consistent.
        n = self.width * self.height
        struct.pack_into("<IIII", head, 32, 0, n, n + 1024, n + 1024 + 256)
        pal = bytearray()
        for e in self.palette:
            pal += bytes(e)
        return (bytes(head) + bytes(self.indices) + bytes(pal)
                + bytes(self.teamEffect0) + bytes(self.teamEffect1) + self.tail)


def nearest_index(palette, rgb, cache):
    key = rgb
    hit = cache.get(key)
    if hit is not None:
        return hit
    r, g, b = rgb
    best = 0
    bestd = 1 << 30
    for i, (pr, pg, pb, _pa) in enumerate(palette):
        dr = r - pr; dg = g - pg; db = b - pb
        d = dr * dr + dg * dg + db * db
        if d < bestd:
            bestd = d
            best = i
            if d == 0:
                break
    cache[key] = best
    return best


def rebuild(lif, rgba, width, height):
    """Replace a Lif's image with RGBA pixels at a new size, keeping its
    palette and both teamEffect tables byte-identical."""
    cache = {}
    idx = bytearray(width * height)
    for i in range(width * height):
        r, g, b = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]
        idx[i] = nearest_index(lif.palette, (r, g, b), cache)
    lif.width = width
    lif.height = height
    lif.indices = idx
    return lif
