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
e5e90705e8cfa24ece79c20d8999418c  homeworld-unbound-vr.apk  118M
```

Roughly 65 MB of that is the bundled demo assets and 20 MB the two engine
builds, neither of which is stripped.

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
- **Playing over the internet actually works now.** Peers were naming each
  other by addresses that only mean something on the other's own network, so a
  connection would come up and then every message on it was quietly dropped.
- **Two-player only over the internet**, and both ends have to be on different
  home network ranges — if you are both on `192.168.1.x`, one of you needs to
  change your router's range. LAN play is unaffected.
