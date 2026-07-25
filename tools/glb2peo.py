"""Convert a glTF/GLB mesh into Homeworld 1's RMF mesh format (.peo).

Written to test whether a modern model can be dropped in place of a stock
ship. File.c checks disk before the .big, so the output can simply be placed
at the asset's path to override it.

The on-disk layout, all offsets being byte offsets from the start of file:

    68              GeoFileHeader
    112 * nObjects  polygonobject_disk
    per object      vertexentry[16], normalentry[16], polyentry[40]
    32 * nMaterials materialentry_disk
    strings

Two engine constraints drive the shape of the output:

  * Polygon vertex indices are uword, so no object may exceed 65535 vertices.
    A dense model is split across several polygonobjects.
  * The normal list of an object holds its face normals FIRST and then its
    vertex normals, with vertices indexing into the combined list. Confirmed
    against the stock mothership, whose object 0 has 321 face and 93 vertex
    normals and vertex indices running to 413.

Textures are not written. Materials are emitted untextured so the geometry
path can be tested on its own; HW1 textures are 8-bit paletted with team
colour index-remap tables, which is a separate problem.

Usage:
    python3 tools/glb2peo.py <in.glb> <out.peo> [--scale S] [--swap zxy]
"""
import json
import math
import struct
import sys

MAX_VERTS = 65000                   # uword indices, with headroom
HEADER_SIZE = 68
OBJ_SIZE = 112
MAT_SIZE = 32
VERT_SIZE = 16
NORM_SIZE = 16
POLY_SIZE = 40

COMPONENT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
             5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4)}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def load_glb(path):
    d = open(path, "rb").read()
    magic, ver, _ = struct.unpack_from("<III", d, 0)
    if magic != 0x46546C67:
        raise ValueError("not a GLB")
    o = 12
    js = bin_ = None
    while o < len(d):
        clen, ctype = struct.unpack_from("<II", d, o)
        if ctype == 0x4E4F534A:
            js = json.loads(d[o + 8:o + 8 + clen])
        elif ctype == 0x004E4942:
            bin_ = d[o + 8:o + 8 + clen]
        o += 8 + clen
    return js, bin_


def accessor(g, bin_, idx):
    a = g["accessors"][idx]
    bv = g["bufferViews"][a["bufferView"]]
    fmt, size = COMPONENT[a["componentType"]]
    n = NCOMP[a["type"]]
    base = bv.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = bv.get("byteStride") or (size * n)
    out = []
    for i in range(a["count"]):
        out.append(struct.unpack_from("<" + fmt * n, bin_, base + i * stride))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    src, dst = sys.argv[1], sys.argv[2]
    scale = 1.0
    swap = "xyz"
    if "--scale" in sys.argv:
        scale = float(sys.argv[sys.argv.index("--scale") + 1])
    if "--swap" in sys.argv:
        swap = sys.argv[sys.argv.index("--swap") + 1]

    g, bin_ = load_glb(src)
    prim = g["meshes"][0]["primitives"][0]
    pos = accessor(g, bin_, prim["attributes"]["POSITION"])
    nrm = (accessor(g, bin_, prim["attributes"]["NORMAL"])
           if "NORMAL" in prim["attributes"] else None)
    uv = (accessor(g, bin_, prim["attributes"]["TEXCOORD_0"])
          if "TEXCOORD_0" in prim["attributes"] else None)
    idx = [i[0] for i in accessor(g, bin_, prim["indices"])]
    tris = [tuple(idx[i:i + 3]) for i in range(0, len(idx) - 2, 3)]
    print("source: %d verts, %d tris" % (len(pos), len(tris)))

    ax = {"x": 0, "y": 1, "z": 2}
    order = [ax[c] for c in swap]

    def xf(v):
        return (v[order[0]] * scale, v[order[1]] * scale, v[order[2]] * scale)

    # split into objects that respect the uword index limit
    objects = []
    cur_map = {}
    cur_tris = []
    for t in tris:
        if len(cur_map) + 3 > MAX_VERTS:
            objects.append((cur_map, cur_tris))
            cur_map, cur_tris = {}, []
        local = []
        for v in t:
            if v not in cur_map:
                cur_map[v] = len(cur_map)
            local.append(cur_map[v])
        cur_tris.append((tuple(local), t))
    if cur_tris:
        objects.append((cur_map, cur_tris))
    print("split into %d polygonobject(s)" % len(objects))

    # ---- build each object's blocks
    blocks = []
    for vmap, otris in objects:
        inv = [0] * len(vmap)
        for src_i, loc in vmap.items():
            inv[loc] = src_i
        face_normals = []
        for (loc, orig) in otris:
            a, b, c = (xf(pos[orig[0]]), xf(pos[orig[1]]), xf(pos[orig[2]]))
            ux, uy, uz = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
            vx, vy, vz = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
            nx, ny, nz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
            L = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            face_normals.append((nx / L, ny / L, nz / L))
        vert_normals = []
        for src_i in inv:
            vert_normals.append(xf(nrm[src_i]) if nrm else (0.0, 0.0, 1.0))
        # normalise the transformed vertex normals (scale is uniform, so this
        # only guards against a zero-length input)
        vn = []
        for (x, y, z) in vert_normals:
            L = math.sqrt(x * x + y * y + z * z) or 1.0
            vn.append((x / L, y / L, z / L))
        blocks.append(dict(inv=inv, tris=otris, fn=face_normals, vn=vn))

    # ---- lay the file out
    nObj = len(blocks)
    off = HEADER_SIZE + OBJ_SIZE * nObj
    for bl in blocks:
        bl["oVert"] = off;  off += VERT_SIZE * len(bl["inv"])
        bl["oNorm"] = off;  off += NORM_SIZE * (len(bl["fn"]) + len(bl["vn"]))
        bl["oPoly"] = off;  off += POLY_SIZE * len(bl["tris"])
    oMaterial = off; off += MAT_SIZE
    oStrings = off

    names = b"glb\0material\0"
    total = oStrings + len(names)

    out = bytearray(total)
    struct.pack_into("<8s", out, 0, b"RMF99ba")
    struct.pack_into("<IIIIIIIII", out, 8,
                     0x402,        # version
                     0,            # pName
                     total,        # __obsolete (was fileSize)
                     0xFFFFFFFF,   # localSize: 'no paco fixup info'
                     1,            # nPublicMaterials
                     0,            # nLocalMaterials
                     oMaterial, oMaterial,
                     nObj)

    for i, bl in enumerate(blocks):
        o = HEADER_SIZE + i * OBJ_SIZE
        struct.pack_into("<IBBHiiiiIIIIII", out, o,
                         oStrings,                 # pName -> "glb"
                         0, i, 0,
                         len(bl["inv"]), len(bl["fn"]), len(bl["vn"]),
                         len(bl["tris"]),
                         bl["oVert"], bl["oNorm"], bl["oPoly"],
                         0, 0, 0)
        # identity localMatrix
        for r in range(4):
            for c in range(4):
                struct.pack_into("<f", out, o + 48 + (r * 4 + c) * 4,
                                 1.0 if r == c else 0.0)

    for bl in blocks:
        nfn = len(bl["fn"])
        for k, src_i in enumerate(bl["inv"]):
            x, y, z = xf(pos[src_i])
            struct.pack_into("<fffi", out, bl["oVert"] + k * VERT_SIZE,
                             x, y, z, nfn + k)
        for k, (x, y, z) in enumerate(bl["fn"] + bl["vn"]):
            struct.pack_into("<fff", out, bl["oNorm"] + k * NORM_SIZE, x, y, z)
        for k, (loc, orig) in enumerate(bl["tris"]):
            o = bl["oPoly"] + k * POLY_SIZE
            struct.pack_into("<iHHHH", out, o, k, loc[0], loc[1], loc[2], 0)
            for j in range(3):
                u, v = (uv[orig[j]] if uv else (0.0, 0.0))
                struct.pack_into("<ff", out, o + 12 + j * 8, u, v)
            struct.pack_into("<H", out, o + 36, 0)

    struct.pack_into("<IIIIfIHBBI", out, oMaterial,
                     oStrings + 4,        # pName -> "material"
                     0xFFFFFFFF,          # ambient
                     0xFFFFFFFF,          # diffuse
                     0xFFFFFFFF,          # specular
                     1.0,                 # kAlpha
                     0,                   # texture: none
                     0, 0, 0, 0)
    out[oStrings:oStrings + len(names)] = names

    open(dst, "wb").write(bytes(out))
    print("wrote %s (%d bytes, %d objects, %d tris)"
          % (dst, total, nObj, sum(len(b["tris"]) for b in blocks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
