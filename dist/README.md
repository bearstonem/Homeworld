# Prebuilt APK

Built from `5a96765` plus the working tree, on 2026-07-27.

## One APK, both campaigns

There used to be two, because the demo/full choice is compiled into the
engine: `HW_GAME_DEMO` picks the `.big` file the game opens, the music and
speech filenames, and the mission sequence itself, that last one as a
compile-time array. Installing the wrong one failed at startup with
`Unable to open required .big file`.

It still is compiled in — the APK just carries both builds and chooses at
launch. `HomeworldActivity` looks for `Homeworld.big` in the app's data
directory: present means the full game and `libmain.so`, absent means the demo
and `libmainDemo.so`. The freely redistributable 1.05 demo assets ride inside
the APK and are unpacked on first run, so it plays without anything being
copied to the headset at all.

Switching either way is a matter of what is in that directory. Drop your own
game data in and the next launch is the full campaign; the demo files that
would otherwise shadow it are cleared automatically, `Update.big` included —
it sits first in the engine's `bigFilePrecedence`, so a demo copy left behind
silently overrides full-game content. A retail `Update.big` of a different
size is left alone.

| | Needs | Opens |
|---|---|---|
| Demo | nothing | `HomeworldDL.big`, `DL_Music.wxd`, `DL_Demo.vce` |
| Full game | your own copy of Homeworld | `Homeworld.big`, `HW_Music.wxd`, `HW_comp.vce` |

`install.py` at the repository root installs it, and copies your game data if
you have it. Debug-signed, arm64-v8a, Quest only.

```
582fb2d27b90067996dc646a6f8fabbe  homeworld-unbound-vr.apk  99M
```

Roughly 65 MB of that is the bundled demo assets and 20 MB the two engine
builds, neither of which is stripped.

## The app has a new package name

`org.gardensofkadesh.homeworld` became `org.homeworldunbound.game`. Android
keys an app's data directory off that name, so to the headset this is a
**different app**: it installs alongside the old one, and your game data and
saves stay behind in the old directory.

`install.py` handles it — it brings everything across on the device, sets the
permissions the new app needs, and offers to remove the old app afterwards.
Installing the APK on its own does not, and leaves you with two entries in
your library under the same name, one of which has all your data.

## What changed since the last build

- **There is a keyboard.** It appears on the panel whenever anything in the
  game is waiting for text — a player name, a game name, chat, a password, the
  name a save is given — and you type on it by pointing and pulling the
  trigger. It sits along the bottom, moves above the field when that is where
  the field is, and stays out of the button column down the right so nothing
  you might want to press disappears behind it.
- **Multiplayer across the internet.** Both connection buttons now open the
  same screens, and the lobby has a **Join by address** field on it: type the
  host's address, press Add Host, and their game appears in the list beside
  any on your own network. The host forwards TCP 10500 and UDP 10600; nobody
  else configures anything.
- **Playing over the internet actually works now**, tested between a headset
  on mobile data and a PC behind a home router. Three separate things were
  broken and none of them showed up on a LAN: peers naming each other by
  addresses only meaningful on their own network, replies going to the wrong
  port through carrier NAT, and the host filing a joining player under an
  address that player had never heard of — which killed the joiner the moment
  the game started.
- **The host forwards two ports** on their router: **TCP 10500** and **UDP
  10600**. Whoever is joining forwards nothing.
- **Two-player only over the internet**, and both ends have to be on different
  home network ranges — if you are both on `192.168.1.x`, one of you needs to
  change your router's range. LAN play is unaffected by all of this.
