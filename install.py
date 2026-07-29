#!/usr/bin/env python3
"""
Homeworld: Unbound - interactive installer for Meta Quest.

Walks through connecting a headset, installing the app, and - if you own
Homeworld - locating your game data and pushing it across. Works on Windows
and Linux.

There is one APK and it plays either edition. It carries the freely
redistributable 1.05 demo assets and unpacks them on first run, and it holds
both native libraries, choosing between them by whether Homeworld.big is
present. See HomeworldActivity.java.

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
PKG = "org.homeworldunbound.game"
DATA = "/sdcard/Android/data/%s" % PKG
DST = "%s/files" % DATA
# Where 0.1 through 0.6 kept it. The package was renamed after the fork stopped
# being a Gardens of Kadesh build in anything but ancestry, and Android hangs
# the external data directory off the package name - so the game data, which is
# most of a gigabyte and came off a disc, would otherwise have to be copied
# again by hand. It gets moved instead. See migrate_old_data.
OLD_PKG = "org.gardensofkadesh.homeworld"
OLD_DATA = "/sdcard/Android/data/%s" % OLD_PKG
OLD_DST = "%s/files" % OLD_DATA
APK = REPO / "android/project/app/build/outputs/apk/vr/debug/app-vr-debug.apk"
DIST = REPO / "dist"
PREBUILT = DIST / "homeworld-unbound-vr.apk"
DEMO_ASSETS = REPO / "subprojects/demo-assets-1.05/assets"
# One build directory per edition of the engine. Both libraries go into the
# single APK; HomeworldActivity picks between them at launch.
BUILD_FULL = REPO / "build.android-vr"
BUILD_DEMO = REPO / "build.android-vr-demo"
JNILIBS = REPO / "android/project/app/src/vr/jniLibs/arm64-v8a"

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
# The demo files are not pushed from here any more - they ride in the APK and
# HomeworldActivity unpacks them, which is also where their list lives now.

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
    title("Which version do you want to play?")
    info("The app carries the demo and unpacks it itself, so it plays without")
    info("this script copying anything. The full game needs your own copy of")
    info("Homeworld (the Remastered Collection on Steam includes the original,")
    info("which is what this uses).")
    print()
    info("You can change your mind later by re-running this script: the app")
    info("switches on whether your game data is present.")
    print()
    return ask("Play which?",
               [("d", "Demo - nothing to copy, a few missions"),
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
    return shutil.which("ninja") and BUILD_FULL.is_dir() and BUILD_DEMO.is_dir()


def build_apk():
    """Returns a path to an APK, or None."""
    title("Building")

    if PREBUILT.is_file():
        ok("Using the prebuilt APK")
        info(str(PREBUILT))
        if not have_build_tools() or not yes("Build from source instead?", False):
            return PREBUILT

    if not have_build_tools():
        if APK.is_file():
            warn("No build toolchain and no prebuilt APK, but there is one")
            warn("from a previous build:")
            info(str(APK))
            if yes("Use it anyway?", False):
                return APK
        err("Cannot build here.")
        info("Building the Quest APK needs the Android NDK, meson, ninja and")
        info("gradle, plus the cross file in android/. See android/README.md.")
        info("Both build directories are required:")
        info("  %s  (-Ddemo=false)" % BUILD_FULL.name)
        info("  %s  (-Ddemo=true)" % BUILD_DEMO.name)
        if WINDOWS:
            info("On Windows the practical route is to build under WSL, or")
            info("get a prebuilt APK, then re-run this to install it.")
        elif MACOS:
            info("On macOS the cross build is not set up; the practical route")
            info("is to build on Linux, or get a prebuilt APK, then re-run")
            info("this to install it.")
        return None

    if not DEMO_ASSETS.is_dir():
        err("Demo assets missing at %s" % DEMO_ASSETS)
        info("They ride inside the APK now, so the build needs them:")
        info("    meson subprojects download demo-assets")
        return None

    # Both editions, because the APK carries both. HW_GAME_DEMO reaches too
    # far into the engine to be a runtime switch - it picks the .big file, the
    # music and speech filenames, and the mission sequence, that last one as a
    # compile-time array - so there are two libraries and two build trees.
    JNILIBS.mkdir(parents=True, exist_ok=True)
    for builddir, libname, what in ((BUILD_FULL, "libmain.so", "full game"),
                                    (BUILD_DEMO, "libmainDemo.so", "demo")):
        info("Compiling the %s engine (this can take a few minutes)..." % what)
        if subprocess.run(["ninja", "-C", str(builddir)]).returncode != 0:
            err("Build failed in %s." % builddir.name)
            return None
        shutil.copy2(builddir / "libmain.so", JNILIBS / libname)

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


def migrate_old_data(adb):
    """Move game data and saves over from the pre-rename package, once.

    Android keys the external data directory off the package name, so renaming
    the app orphans everything the player put in the old one - the .big file
    off their disc, the soundtrack, the speech pack, and every save. Moving it
    on the device costs seconds; making them find and copy it again costs most
    of a gigabyte over USB and a fair chance they conclude the build is broken.

    A move, not a copy: leaving a second copy behind wastes the space and
    leaves the old app playable and diverging. Only ever runs when the new
    directory has nothing in it, so it cannot overwrite a working install.
    """
    if not dir_exists(adb, OLD_DST):
        return
    listing = (adb.shell("ls %s 2>/dev/null" % OLD_DST).stdout or "").split()
    if not listing:
        return
    if (adb.shell("ls %s 2>/dev/null" % DST).stdout or "").split():
        return                                  # new install already has data

    title("Moving your data over")
    info("This build uses a new package name, and Android keeps game data")
    info("per package. Yours is in the old one:")
    for n in listing:
        info("  %s" % n)
    if not yes("Move it across?", True):
        warn("Left where it is. The new app will start as the demo, and the")
        warn("files are still under %s" % OLD_DST)
        return

    # The app makes and owns the new directory on first run; shell cannot
    # create one it can traverse into. mv into it once it exists, and if it
    # does not yet, say so rather than making an unusable folder.
    if not dir_exists(adb, DST):
        adb.shell("mkdir -p %s" % DST)
        adb.shell("chmod 2775 %s" % DATA)
        adb.shell("chmod 2775 %s" % DST)

    cp = adb.shell("mv %s/* %s/ 2>&1" % (OLD_DST, DST))
    leftover = (adb.shell("ls %s 2>/dev/null" % OLD_DST).stdout or "").split()
    if leftover:
        err("Some of it would not move:")
        for n in leftover:
            info("  %s" % n)
        info((cp.stdout or "").strip()[:200])
        warn("Copy those across by hand, from")
        warn("  %s" % OLD_DST)
        warn("to")
        warn("  %s" % DST)
        return
    adb.shell("rm -rf %s" % OLD_DATA)
    ok("Moved, and the old folder is gone")


def install(adb, apk, edition, assets):
    title("Installing")
    info("Installing the app...")
    cp = adb.run("install", "-r", str(apk))
    if "Success" not in (cp.stdout + cp.stderr):
        err((cp.stderr or cp.stdout).strip())
        return False
    ok("App installed")

    # Before anything looks at the new data directory to decide what is there.
    migrate_old_data(adb)

    # The app makes its own data folder on first run and owns it. Install and
    # copy without ever starting the game and the folder is simply not there
    # yet. One made from here lands mode 2770 owned by shell, which grants the
    # game nothing: it starts, cannot even traverse in to look for the .big
    # files, and quits. So note which levels we had to create and open those
    # up once the copying is done, exactly as the soundtrack folder is handled
    # below. Nothing is chmodded that the app already owns, since shell cannot
    # chmod those anyway.
    fresh = []

    if edition == "f":
        fresh = [d for d in (DATA, DST) if not dir_exists(adb, d)]
        if fresh:
            adb.shell("mkdir -p %s" % DST)

        # Demo files that would shadow the full game. Update.big is FIRST in
        # bigFilePrecedence, so a demo copy left here silently overrides
        # full-game content with demo content.
        adb.shell("cd %s && rm -f %s" % (DST, " ".join(STALE_FOR_FULL)))

        title("Copying game data")
        pairs = [(assets[src], dst) for src, dst in FULL_FILES]
        if not push_tree(adb, pairs, DST):
            return False
        # 2775, not 775: the setgid bit is what makes anything created inside
        # later inherit the ext_data_rw group, and the app makes SavedGames in
        # here itself. Dropping it also breaks `adb push` into those
        # subdirectories afterwards, which fails with "remote fchown failed".
        for d in fresh:
            adb.shell("chmod 2775 %s" % d)
        ok("Game data copied")
    else:
        # Nothing to copy: the APK carries the demo assets and unpacks them on
        # first run, into a directory it creates and owns. Leaving that to the
        # app is better than doing it here - no mkdir, so no chmod, so none of
        # the ownership traps above.
        present = [n for n in STALE_FOR_DEMO
                   if "yes" in (adb.shell("[ -f %s/%s ] && echo yes"
                                          % (DST, n)).stdout or "")]
        if present:
            title("Full game data is already on the headset")
            warn("The app plays the full game whenever Homeworld.big is")
            warn("present, so the demo means deleting what is there:")
            for n in present:
                info("  %s" % n)
            if not yes("Delete it and go back to the demo?", False):
                info("Left alone - the app will keep playing the full game.")
            else:
                adb.shell("cd %s && rm -f %s" % (DST, " ".join(STALE_FOR_DEMO)))
                ok("Full game data removed")
        ok("The app unpacks the demo itself on first launch")

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
        ok("The demo rides inside the app; nothing to copy")

    apk = build_apk()
    if apk is None:
        return 1

    if not install(adb, apk, edition, assets):
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
