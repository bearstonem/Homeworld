"""Extract the Homeworld Remastered soundtrack and convert it to the format
this engine's mixer runs at.

The remastered WAVs are 44100 Hz stereo S16; the mixer is FQ_RATE = 22050 Hz
stereo S16 (src/SDL/fqcodec.h). Downsampling offline rather than raising
FQ_RATE keeps every existing sound effect and speech sample - all encoded
against 22050 - untouched.

The track numbers below are HW1's own, from src/Game/SoundMusic.h. The mapping
is not guesswork: HW1 gives several missions the same track number, and the
remastered filenames name exactly those groupings ("mission2and4" is track 0,
which is AMB_Mission2 and AMB_Mission4; "mission6and12" is track 2; and so on).

Nothing from the Remastered Collection is redistributed - this reads the copy
you already own.

Usage:
    python3 tools/rm_music.py <HomeworldRM/Data> <outdir> [--all]

Without --all only the ambient and battle tracks are converted: those are the
in-game music, and the full set is roughly twice the size.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sga_extract import SGA

MIXER_RATE = 22050

# HW1 track number -> remastered file (basename within its archive).
# Ambient/battle live in Music.big; NIS/animatic in MusicHW1Campaign.big.
AMBIENT_BATTLE = {
     0: "a02_mission2and4.wav",      # AMB_Mission2/4, also AMB12_FrontEnd
     1: "a04_mission5.wav",
     2: "a05_mission6and12.wav",
     3: "a06_mission7and8.wav",
     4: "a07_mission9.wav",
     5: "a08_mission10.wav",
     6: "a09_mission11and15.wav",
     7: "a11_mission13.wav",
     8: "a12_mission14.wav",
     9: "tutorial.wav",              # AMB13_Tutorial
    # 10 and 11 sit in HW1's battle range (MUS_FIRST_BATTLE) yet are also
    # AMB_Mission1 and AMB_Mission3. The remaster split them into ambient and
    # battle variants; the battle cuts are used here to match the range.
    10: "a01_mission1and16.wav",
    11: "a03_mission3.wav",
    12: "b01_turanicraiderslong.wav",
    13: "b02_turanicraidersshort.wav",
    14: "b03_swarmers.wav",
    15: "b04_evilempire.wav",
    16: "battle_01.wav",             # NISlet01
}

NIS_ANIMATIC = {
    17: "n01_r1opening.wav",         18: "n01_r2opening.wav",
    19: "n02_p1intro.wav",           20: "n03_tradersintro.wav",
    21: "n04_learnofsacking.wav",    22: "n05_p2intro.wav",
    23: "n06_supernova.wav",         24: "n07_p3vstraders.wav",
    25: "n08_awareness.wav",         26: "n09_headshot.wav",
    27: "n10_miningfacility.wav",
    31: "anim_00_opening.wav",       32: "anim_01_02.wav",
    33: "anim_02_03.wav",            34: "anim_03_04.wav",
    35: "anim_04_05.wav",            36: "anim_05_06.wav",
    37: "anim_06_07.wav",            38: "anim_07_08.wav",
    39: "anim_08_09.wav",            40: "anim_09_10.wav",
    41: "anim_10_11.wav",            42: "anim_11_12.wav",
    43: "anim_12_13.wav",            44: "anim_13_14.wav",
    45: "anim_14_15.wav",            46: "anim_15_16.wav",
    47: "anim_ending.wav",
}


def index(archive):
    """basename -> (dataOffset, compressedLen, uncompressedLen)"""
    out = {}
    for _, nm, doff, clen, ulen in archive.files():
        out[nm.lower()] = (doff, clen, ulen)
    return out


def convert(archive, entry, dst):
    doff, clen, ulen = entry
    raw = archive.read(doff, clen, ulen)
    # ffmpeg reads the source WAV on stdin and writes 22050 stereo S16.
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-i", "pipe:0", "-ar", str(MIXER_RATE), "-ac", "2",
         "-c:a", "pcm_s16le", dst],
        input=raw, check=True)
    return os.path.getsize(dst)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    data, outdir = sys.argv[1], sys.argv[2]
    everything = "--all" in sys.argv
    os.makedirs(outdir, exist_ok=True)

    jobs = [(os.path.join(data, "Music.big"), AMBIENT_BATTLE)]
    if everything:
        jobs.append((os.path.join(data, "MusicHW1Campaign.big"), NIS_ANIMATIC))

    total = 0
    missing = []
    for path, table in jobs:
        arc = SGA(path)
        have = index(arc)
        for track, name in sorted(table.items()):
            e = have.get(name.lower())
            if e is None:
                missing.append(name)
                continue
            dst = os.path.join(outdir, "track%02d.wav" % track)
            size = convert(arc, e, dst)
            total += size
            print("track%02d  %-32s %6.1f MB" % (track, name, size / 1e6))

    print("\n%d tracks, %.0f MB total" % (
        len(AMBIENT_BATTLE) + (len(NIS_ANIMATIC) if everything else 0) - len(missing),
        total / 1e6))
    if missing:
        print("MISSING (not found in archive):", ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
