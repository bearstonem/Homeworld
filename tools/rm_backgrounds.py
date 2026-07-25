"""Extract the Homeworld Remastered background cubemaps for use as skyboxes.

HW1's backgrounds are .btg files - a vertex-coloured mesh plus star points,
not textures - so these cannot replace them by substitution; src/SDL/rmsky.c
renders a textured cube instead. This tool only produces the faces.

The remastered faces are 1024x1024 DXT1 inside DDS. They are converted to PNG
here rather than kept compressed, because Adreno's S3TC support is not
something to rely on and stb_image (already vendored) reads PNG. ~377 KB per
face, so a full set of 16 backgrounds is around 36 MB.

The mission mapping is 1:1 and comes from the remaster's own campaign data:
leveldata/campaign/homeworldclassic/missionNN/missionNN.level references
exactly ezNN, for NN = 01..16. HW1 uses the same names for its .btg files, so
the runtime can key off the background name the level already asks for.

Nothing from the Remastered Collection is redistributed - this reads the copy
you already own.

Usage:
    python3 tools/rm_backgrounds.py <HomeworldRM/Data> <outdir> [ez01 ez02 ...]

With no names given, every ezNN set is converted. Output layout:
    <outdir>/<name>/{posx,negx,posy,negy,posz,negz}.png
"""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sga_extract import SGA

FACES = ("posx", "negx", "posy", "negy", "posz", "negz")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    data, outdir = sys.argv[1], sys.argv[2]
    wanted = set(sys.argv[3:])

    arc = SGA(os.path.join(data, "HWBackgrounds.big"))
    dirs = arc.paths()

    # name -> face -> entry. Prefer the _hq_ variant where both exist.
    sets = {}
    for i, nm, doff, clen, ulen in arc.files():
        m = re.match(r'^(ez\d\d)(_hq)?_(posx|negx|posy|negy|posz|negz)\.dds$',
                     nm, re.I)
        if not m:
            continue
        name, hq, face = m.group(1).lower(), bool(m.group(2)), m.group(3).lower()
        if wanted and name not in wanted:
            continue
        cur = sets.setdefault(name, {})
        # _hq_ wins; otherwise take whatever we find first
        if face not in cur or hq:
            cur[face] = (doff, clen, ulen, hq)

    if not sets:
        print("no ezNN cubemaps found - is this the HomeworldRM/Data directory?")
        return 1

    total = 0
    for name in sorted(sets):
        faces = sets[name]
        missing = [f for f in FACES if f not in faces]
        if missing:
            print("%s: SKIPPED, missing %s" % (name, ",".join(missing)))
            continue
        dstdir = os.path.join(outdir, name)
        os.makedirs(dstdir, exist_ok=True)
        got = 0
        for face in FACES:
            doff, clen, ulen, hq = faces[face]
            dds = arc.read(doff, clen, ulen)
            dst = os.path.join(dstdir, face + ".png")
            # via a temp file: the DDS demuxer needs to seek, so a pipe fails
            tmp = dst + ".dds"
            with open(tmp, "wb") as fh:
                fh.write(dds)
            subprocess.run(
                ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                 "-i", tmp, dst], check=True)
            os.remove(tmp)
            got += os.path.getsize(dst)
        total += got
        print("%s  6 faces  %5.1f MB%s" % (name, got / 1e6, "  (hq)" if hq else ""))

    print("\n%d background(s), %.0f MB" % (len(sets), total / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
