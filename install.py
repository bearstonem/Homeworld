#!/usr/bin/env python3
"""
Homeworld: Unbound - interactive installer for Meta Quest.

Walks through connecting a headset, choosing demo or full game, locating your
game data, and pushing everything to the right place. Works on Windows and
Linux.

    python3 install.py          (Linux, macOS)
    py install.py               (Windows)

Installing works on all three. Building from source needs the Android NDK
cross toolchain, which is set up for Linux; elsewhere the script installs a
prebuilt APK instead.

Nothing from any commercial release is bundled here. The demo assets that ship
with this repository are freely redistributable; the full game needs your own
copy.
"""
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent
PKG = "org.gardensofkadesh.homeworld"
DATA = "/sdcard/Android/data/%s" % PKG
DST = "%s/files" % DATA
APK = REPO / "android/project/app/build/outputs/apk/vr/debug/app-vr-debug.apk"
# Prebuilt APKs, one per edition. Which edition an APK is cannot be detected
# from the outside - it is compiled in - so they are kept as separate files
# rather than guessed at.
DIST = REPO / "dist"
PREBUILT = {"d": DIST / "homeworld-unbound-vr-demo.apk",
            "f": DIST / "homeworld-unbound-vr-full.apk"}
DEMO_ASSETS = REPO / "subprojects/demo-assets-1.05/assets"

WINDOWS = platform.system() == "Windows"
MACOS = platform.system() == "Darwin"

# The filenames the binary opens, which are NOT always the case the files ship
# with: the full game's voice file is opened as "HW_comp.vce" while the
# collection ships "HW_Comp.vce", and the demo's is opened as "DL_Demo.vce"
# while the repo ships "DL_demo.vce". Android storage is case-insensitive in
# practice, but pushing under the expected name removes the doubt.
FULL_FILES = [("homeworld.big", "Homeworld.big"),
              ("HW_Music.wxd",  "HW_Music.wxd"),
              ("HW_Comp.vce",   "HW_comp.vce")]
DEMO_FILES = [("HomeworldDL.big", "HomeworldDL.big"),
              ("DL_Music.wxd",    "DL_Music.wxd"),
              ("DL_demo.vce",     "DL_Demo.vce"),
              ("Update.big",      "Update.big")]

# Demo assets that would shadow a full install. Update.big is FIRST in the
# engine's bigFilePrecedence, so leaving the demo copy in place silently
# overrides the full game's data with demo content.
STALE_FOR_FULL = ["HomeworldDL.big", "DL_Music.wxd", "DL_demo.vce",
                  "DL_Demo.vce", "Update.big"]
STALE_FOR_DEMO = ["Homeworld.big", "HW_Music.wxd", "HW_comp.vce", "HW_Comp.vce"]


# ---------------------------------------------------------------- output ----
def _c(code, s):
    if WINDOWS and not os.environ.get("WT_SESSION"):
        return s                                    # old consoles: no colour
    return "\033[%sm%s\033[0m" % (code, s)


def title(s):  print("\n" + _c("1;36", s) + "\n" + "-" * len(s))
def ok(s):     print(_c("32", "  OK  ") + s)
def warn(s):   print(_c("33", "  !   ") + s)
def err(s):    print(_c("31", " FAIL ") + s)
def info(s):   print("      " + s)


def ask(prompt, options=None, default=None):
    """options: list of (key, description)."""
    while True:
        if options:
            for k, d in options:
                mark = " (default)" if k == default else ""
                print("    [%s] %s%s" % (_c("1;37", k), d, mark))
        suffix = " [%s]" % default if default else ""
        try:
            r = input("  %s%s: " % (prompt, suffix)).strip()
        except (EOFError, KeyboardInterrupt):
            print("\nCancelled.")
            sys.exit(1)
        if not r and default:
            return default
        if not options:
            return r
        keys = [k for k, _ in options]
        if r.lower() in keys:
            return r.lower()
        warn("Please choose one of: %s" % ", ".join(keys))


def yes(prompt, default=True):
    d = "y" if default else "n"
    r = ask(prompt + " (y/n)", None, d).lower()
    return r.startswith("y")


# ------------------------------------------------------------------- adb ----
def find_adb():
    p = shutil.which("adb")
    if p:
        return p
    exe = "adb.exe" if WINDOWS else "adb"
    candidates = []
    if WINDOWS:
        local = os.environ.get("LOCALAPPDATA", "")
        candidates += [Path(local) / "Android/Sdk/platform-tools" / exe,
                       Path("C:/Program Files/SideQuest/resources/build/platform-tools") / exe,
                       Path(local) / "Programs/SideQuest/resources/build/platform-tools" / exe]
    elif MACOS:
        candidates += [Path("/opt/homebrew/bin") / exe,          # Apple Silicon
                       Path("/usr/local/bin") / exe,             # Intel homebrew
                       Path.home() / "Library/Android/sdk/platform-tools" / exe,
                       Path("/Applications/SideQuest.app/Contents/Resources/build/platform-tools") / exe,
                       Path.home() / "Library/Android/platform-tools" / exe]
    else:
        candidates += [Path.home() / "Android/Sdk/platform-tools" / exe,
                       Path("/usr/lib/android-sdk/platform-tools") / exe,
                       Path.home() / ".local/share/SideQuest/platform-tools" / exe,
                       Path("/opt/SideQuest/resources/build/platform-tools") / exe]
    for c in candidates:
        if c.is_file():
            return str(c)
    return None


def adb_help():
    title("adb is not installed")
    info("adb is the tool that talks to your headset. It is a single small")
    info("program from Google's Android platform-tools.")
    print()
    if WINDOWS:
        info("Windows - pick either:")
        info("  * winget install --id Google.PlatformTools")
        info("  * or download 'SDK Platform-Tools for Windows' from")
        info("    https://developer.android.com/tools/releases/platform-tools")
        info("    unzip it, and add that folder to your PATH")
        info("  * or install SideQuest, which bundles adb")
    elif MACOS:
        info("macOS - pick either:")
        info("  * brew install --cask android-platform-tools")
        info("  * or download 'SDK Platform-Tools for Mac' from")
        info("    https://developer.android.com/tools/releases/platform-tools")
        info("    unzip it, and add that folder to your PATH")
        info("  * or install SideQuest, which bundles adb")
        print()
        info("If macOS blocks it as unidentified, allow it once under")
        info("System Settings > Privacy & Security.")
    else:
        info("Linux - pick either:")
        info("  * Debian/Ubuntu:  sudo apt install android-tools-adb")
        info("  * Fedora:         sudo dnf install android-tools")
        info("  * Arch:           sudo pacman -S android-tools")
        info("  * or download platform-tools from")
        info("    https://developer.android.com/tools/releases/platform-tools")
    print()
    info("Then re-run this script.")


class Adb:
    def __init__(self, exe, serial=None):
        self.exe = exe
        self.serial = serial

    def _base(self):
        return [self.exe] + (["-s", self.serial] if self.serial else [])

    def run(self, *args, check=False, quiet=True):
        cp = subprocess.run(self._base() + list(args),
                            capture_output=True, text=True)
        if check and cp.returncode != 0:
            err((cp.stderr or cp.stdout).strip())
        return cp

    def devices(self):
        cp = subprocess.run([self.exe, "devices"], capture_output=True, text=True)
        out = []
        for line in cp.stdout.splitlines()[1:]:
            if "\t" in line:
                serial, state = line.split("\t")[:2]
                out.append((serial.strip(), state.strip()))
        return out

    def alive(self):
        return self.run("shell", "echo", "ok").returncode == 0

    def shell(self, cmd):
        return self.run("shell", cmd)

    def push(self, src, dst):
        return subprocess.run(self._base() + ["push", str(src), dst],
                              capture_output=True, text=True)


# ---------------------------------------------------------- device setup ----
def headset_prep_help(wireless):
    title("Preparing the headset")
    info("1. On your phone, in the Meta Horizon app, open your headset's")
    info("   settings and turn on Developer Mode. (You may need to create a")
    info("   free developer organisation first - Meta's site walks you")
    info("   through it.)")
    info("2. Put the headset on and reboot it once after enabling it.")
    if wireless:
        print()
        info("For wireless, the easiest route is SideQuest on your PC:")
        info("  * Install SideQuest, connect the headset by USB once, accept")
        info("    the 'Allow USB debugging' prompt in the headset")
        info("  * In SideQuest, press the wireless/adb icon - it shows the")
        info("    headset's IP and turns on wireless adb")
        info("  * You can then unplug the cable")
        print()
        info("Or without SideQuest, with the headset plugged in once:")
        info("     adb tcpip 5555")
        info("     adb shell ip route      (to find the headset's IP)")
        print()
        info("Note: the port changes whenever wireless debugging is toggled.")
    else:
        print()
        info("Plug the headset into this computer with a USB cable, then")
        info("look inside the headset - it will ask you to allow USB")
        info("debugging. Tick 'Always allow' and accept.")


def connect_device(adb):
    title("Connecting to your headset")
    mode = ask("How is your headset connected?",
               [("u", "USB cable (simplest)"),
                ("w", "Wireless / network adb"),
                ("h", "I need help setting this up")], "u")
    if mode == "h":
        headset_prep_help(wireless=yes("Do you want wireless (no cable)?", False))
        input("\n  Press Enter once that is done...")
        return connect_device(adb)

    if mode == "w":
        target = ask("Headset address (IP or IP:port, e.g. 192.168.1.92:5555)")
        if ":" not in target:
            target += ":5555"
        info("Connecting to %s ..." % target)
        cp = subprocess.run([adb.exe, "connect", target],
                            capture_output=True, text=True)
        print("      " + cp.stdout.strip())
        if "connected" not in cp.stdout:
            err("Could not connect.")
            info("Common causes: headset asleep, wrong port (it changes each")
            info("time wireless debugging is toggled), or Developer Mode off.")
            if yes("Show the setup guide?", True):
                headset_prep_help(wireless=True)
            return None
        adb.serial = target

    devices = adb.devices()
    usable = [(s, st) for s, st in devices if st == "device"]
    unauth = [(s, st) for s, st in devices if st == "unauthorized"]

    if unauth and not usable:
        warn("Headset found but not authorised.")
        info("Put the headset on - there is an 'Allow USB debugging' prompt")
        info("waiting. Tick 'Always allow' and accept it.")
        input("\n  Press Enter once accepted...")
        usable = [(s, st) for s, st in adb.devices() if st == "device"]

    if not usable:
        err("No headset detected.")
        if yes("Show the setup guide?", True):
            headset_prep_help(wireless=(mode == "w"))
        return None

    if adb.serial is None:
        if len(usable) > 1:
            info("Several devices found:")
            for i, (s, _) in enumerate(usable):
                print("    [%d] %s" % (i, s))
            idx = ask("Which one?", [(str(i), s) for i, (s, _) in enumerate(usable)], "0")
            adb.serial = usable[int(idx)][0]
        else:
            adb.serial = usable[0][0]

    model = adb.shell("getprop ro.product.model").stdout.strip()
    ok("Connected to %s%s" % (adb.serial, (" (%s)" % model) if model else ""))
    return adb


# -------------------------------------------------------------- game data ---
def steam_candidates():
    out = []
    if WINDOWS:
        for drive in "CDEFG":
            out += [Path("%s:/Program Files (x86)/Steam/steamapps/common/Homeworld" % drive),
                    Path("%s:/SteamLibrary/steamapps/common/Homeworld" % drive),
                    Path("%s:/Games/Steam/steamapps/common/Homeworld" % drive)]
    elif MACOS:
        home = Path.home()
        out += [home / "Library/Application Support/Steam/steamapps/common/Homeworld",
                Path("/Applications/Homeworld"),
                home / "Games/Steam/steamapps/common/Homeworld"]
    else:
        home = Path.home()
        out += [home / ".local/share/Steam/steamapps/common/Homeworld",
                home / ".steam/steam/steamapps/common/Homeworld",
                home / "Games/Steam/steamapps/common/Homeworld"]
    return out


def find_full_assets(start=None):
    """Look for the three full-game files under a directory."""
    roots = [Path(start)] if start else [p for p in steam_candidates() if p.is_dir()]
    for root in roots:
        if not root.is_dir():
            continue
        for base in [root, root / "Data", root / "Homeworld1Classic/Data"]:
            if base.is_dir() and any((base / n).is_file() for n, _ in FULL_FILES):
                found = {}
                for name, _ in FULL_FILES:
                    for f in base.iterdir():
                        if f.name.lower() == name.lower():
                            found[name] = f
                if len(found) == len(FULL_FILES):
                    return base, found
        # one level of searching, for unusual layouts
        try:
            for sub in root.rglob("homeworld.big"):
                base = sub.parent
                found = {}
                for name, _ in FULL_FILES:
                    for f in base.iterdir():
                        if f.name.lower() == name.lower():
                            found[name] = f
                if len(found) == len(FULL_FILES):
                    return base, found
        except (OSError, PermissionError):
            pass
    return None, None


def choose_edition():
    title("Which version do you want to install?")
    info("The demo is included with this project and needs nothing else.")
    info("The full game needs your own copy of Homeworld (the Remastered")
    info("Collection on Steam includes the original, which is what this uses).")
    print()
    return ask("Install which?",
               [("d", "Demo - works right away, a few missions"),
                ("f", "Full game - all 16 missions, needs your own copy")], "d")


def locate_full_assets():
    title("Finding your Homeworld data")
    info("Looking in the usual Steam locations...")
    base, found = find_full_assets()
    if base:
        ok("Found game data in:")
        info(str(base))
        if yes("Use this?", True):
            return found
    else:
        warn("Could not find it automatically.")
    print()
    info("Point me at the folder containing homeworld.big, HW_Music.wxd and")
    info("HW_Comp.vce. In the Remastered Collection that is:")
    info("  ...\\steamapps\\common\\Homeworld\\Homeworld1Classic\\Data")
    while True:
        p = ask("Path to your Homeworld data folder (blank to cancel)")
        if not p:
            return None
        p = p.strip().strip('"').strip("'")
        base, found = find_full_assets(p)
        if found:
            ok("Found all three files.")
            return found
        err("Could not find the game files there.")
        info("Expected homeworld.big, HW_Music.wxd and HW_Comp.vce.")


# ------------------------------------------------------------------ build ---
def have_build_tools():
    return shutil.which("ninja") and (REPO / "build.android-vr").is_dir()


def build_apk(edition):
    """Returns a path to an APK, or None."""
    title("Building")
    want_demo = (edition == "d")
    builddir = REPO / "build.android-vr"

    prebuilt = PREBUILT[edition]
    if prebuilt.is_file():
        ok("Using the prebuilt %s APK" % ("demo" if want_demo else "full game"))
        info(str(prebuilt))
        if not have_build_tools() or not yes("Build from source instead?", False):
            return prebuilt

    if not have_build_tools():
        if APK.is_file():
            warn("No build toolchain and no prebuilt APK for this edition,")
            warn("but there is an APK from a previous build:")
            info(str(APK))
            info("Its edition cannot be detected from the outside. If the")
            info("game fails to start, it is the wrong one - rebuild, or get")
            info("the matching APK from dist/.")
            if yes("Use it anyway?", False):
                return APK
        err("Cannot build here.")
        info("Building the Quest APK needs the Android NDK, meson, ninja and")
        info("gradle, plus the cross file in android/. See android/README.md.")
        if WINDOWS:
            info("On Windows the practical route is to build under WSL, or")
            info("get a prebuilt APK, then re-run this to install it.")
        elif MACOS:
            info("On macOS the cross build is not set up; the practical route")
            info("is to build on Linux, or get a prebuilt APK, then re-run")
            info("this to install it.")
        return None

    # Match the meson 'demo' option to what was chosen; a mismatch produces a
    # binary that opens the wrong filenames and dies at startup with
    # "Unable to open required .big file".
    import json
    current = None
    intro = builddir / "meson-info/intro-buildoptions.json"
    if intro.is_file():
        for o in json.loads(intro.read_text()):
            if o["name"] == "demo":
                current = bool(o["value"])
    if current is not None and current != want_demo:
        info("Reconfiguring build for %s..." % ("demo" if want_demo else "full game"))
        meson = shutil.which("meson")
        if not meson:
            err("meson not found; cannot switch edition.")
            return None
        subprocess.run([meson, "configure", str(builddir),
                        "-Ddemo=%s" % ("true" if want_demo else "false")],
                       check=False)

    info("Compiling (this can take a few minutes)...")
    if subprocess.run(["ninja", "-C", str(builddir)]).returncode != 0:
        err("Build failed.")
        return None
    lib = builddir / "libmain.so"
    dest = REPO / "android/project/app/src/vr/jniLibs/arm64-v8a/libmain.so"
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(lib, dest)

    info("Packaging APK...")
    gradlew = REPO / "android/project" / ("gradlew.bat" if WINDOWS else "gradlew")
    cp = subprocess.run([str(gradlew), "assembleVrDebug"],
                        cwd=str(REPO / "android/project"))
    if cp.returncode != 0:
        err("Gradle failed.")
        return None
    ok("Built %s" % APK.name)
    return APK


# ---------------------------------------------------------------- install ---
def dir_exists(adb, path):
    return "yes" in (adb.shell("[ -d %s ] && echo yes" % path).stdout or "")


def push_tree(adb, pairs, dest_dir):
    """pairs: list of (local Path, remote filename)."""
    for src, name in pairs:
        size = src.stat().st_size / 1e6
        info("  %-18s %6.1f MB" % (name, size))
        cp = adb.push(src, "%s/%s" % (dest_dir, name))
        if cp.returncode != 0:
            err((cp.stderr or cp.stdout).strip())
            return False
    return True


def install(adb, apk, edition, assets, music_dir):
    title("Installing")
    info("Installing the app...")
    cp = adb.run("install", "-r", str(apk))
    if "Success" not in (cp.stdout + cp.stderr):
        err((cp.stderr or cp.stdout).strip())
        return False
    ok("App installed")

    # The app makes its own data folder on first run and owns it. Install and
    # copy without ever starting the game and the folder is simply not there
    # yet. One made from here lands mode 2770 owned by shell, which grants the
    # game nothing: it starts, cannot even traverse in to look for the .big
    # files, and quits. So note which levels we had to create and open those
    # up once the copying is done, exactly as the soundtrack folder is handled
    # below. Nothing is chmodded that the app already owns, since shell cannot
    # chmod those anyway.
    fresh = [d for d in (DATA, DST) if not dir_exists(adb, d)]
    if fresh:
        adb.shell("mkdir -p %s" % DST)

    # Clear whichever edition's files would shadow this one.
    stale = STALE_FOR_FULL if edition == "f" else STALE_FOR_DEMO
    adb.shell("cd %s && rm -f %s" % (DST, " ".join(stale)))

    title("Copying game data")
    if edition == "f":
        pairs = [(assets[src], dst) for src, dst in FULL_FILES]
    else:
        pairs = [(DEMO_ASSETS / src, dst) for src, dst in DEMO_FILES]
    if not push_tree(adb, pairs, DST):
        return False
    # 2775, not 775: the setgid bit is what makes anything created inside
    # later inherit the ext_data_rw group, and the app makes SavedGames in
    # here itself. Dropping it also breaks `adb push` into those subdirectories
    # afterwards, which fails with "remote fchown failed".
    for d in fresh:
        adb.shell("chmod 2775 %s" % d)
    ok("Game data copied")

    if music_dir:
        title("Copying the remastered soundtrack")
        tracks = sorted(music_dir.glob("track*.wav"))
        info("%d tracks, %.0f MB - this takes a moment"
             % (len(tracks), sum(t.stat().st_size for t in tracks) / 1e6))
        adb.shell("mkdir -p %s/music" % DST)
        # Every push is checked. These can fail as a group while looking fine
        # (a destination directory that lost its ext_data_rw group refuses the
        # ownership change adb does as it copies, so all of them come back
        # "remote fchown failed"), and the game says nothing about it either:
        # rmMusicHasTrack gates each override, so a missing set just plays the
        # original .wxd score. Reporting success here would bury both.
        failed = []
        for t in tracks:
            cp = adb.push(t, "%s/music/%s" % (DST, t.name))
            if cp.returncode != 0:
                failed.append((cp.stderr or cp.stdout).strip())
        # chmod AFTER pushing: adb sets ownership as it copies, and doing this
        # first makes the push fail with "remote fchown failed". A directory
        # made by adb is mode 2770 owned by shell, which grants the game
        # nothing - it cannot even traverse in, and simply plays the original
        # music with no error.
        adb.shell("chmod -R 775 %s/music" % DST)
        if failed:
            err("%d of %d tracks did not copy: %s"
                % (len(failed), len(tracks), failed[0]))
            warn("The game will run, playing its original music instead.")
        else:
            ok("Soundtrack copied")

    # If the game was ever started before its data landed, that process is
    # still around having already given up looking for it. Launching from the
    # library resumes that one and it dies immediately, which reads as "the
    # install did not work". The next start has to be a fresh process.
    adb.shell("am force-stop %s" % PKG)

    return True


def main():
    print(_c("1;36", r"""
   _   _                              _    _
  | | | |___ _ __  _____ __ _____ _ _| |__| |
  | |_| / _ \ '  \/ -_) V  V / _ \ '_| / _` |
  |_| |_\___/_|_|_\___|\_/\_/\___/_| |_\__,_|   UNBOUND
                                    Meta Quest VR installer
"""))
    info("This will build (if it can), install, and copy game data to your")
    info("headset. Nothing is launched automatically - you start it from the")
    info("headset when it is done.")

    title("Checking tools")
    adb_exe = find_adb()
    if not adb_exe:
        err("adb not found")
        adb_help()
        return 1
    ok("adb: %s" % adb_exe)

    adb = connect_device(Adb(adb_exe))
    if adb is None:
        return 1

    edition = choose_edition()
    assets = None
    if edition == "f":
        assets = locate_full_assets()
        if not assets:
            warn("No full game data - falling back to the demo.")
            edition = "d"
    if edition == "d":
        missing = [n for n, _ in DEMO_FILES if not (DEMO_ASSETS / n).is_file()]
        if missing:
            err("Demo assets missing from the repository: %s" % ", ".join(missing))
            return 1
        ok("Using the demo assets included with this project")

    music_dir = None
    for cand in [REPO / "music", Path.home() / "hw-music"]:
        if cand.is_dir() and list(cand.glob("track*.wav")):
            music_dir = cand
            break
    if music_dir:
        title("Remastered soundtrack")
        info("Found converted tracks in %s" % music_dir)
        if not yes("Install the remastered soundtrack too?", True):
            music_dir = None
    elif edition == "f":
        title("Remastered soundtrack (optional)")
        info("If you own the Remastered Collection you can also use its")
        info("re-recorded soundtrack. Convert it once with:")
        info("    python3 tools/rm_music.py <HomeworldRM/Data> ./music --all")
        info("then re-run this script. Without it the original music plays.")

    apk = build_apk(edition)
    if apk is None:
        return 1

    if not install(adb, apk, edition, assets, music_dir):
        err("Installation failed.")
        return 1

    title("Done")
    ok("Homeworld: Unbound is installed on your headset.")
    print()
    info("Put the headset on and launch it from your app library.")
    info("It is under 'Unknown Sources' as \"Homeworld: Unbound\".")
    print()
    info("It is deliberately not launched for you: starting it over adb")
    info("sometimes leaves the headset stuck in Meta's loading environment.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nCancelled.")
        sys.exit(1)
