# Prebuilt APK

Built from `c4c3efa` plus the working tree, on 2026-08-02.

## One APK, both campaigns

There used to be two, because the demo/full choice is compiled into the
engine: `HW_GAME_DEMO` picks the `.big` file the game opens, the music and
speech filenames, and the mission sequence itself, that last one as a
compile-time array. Installing the wrong one failed at startup with
`Unable to open required .big file`.

It still is compiled in — the APK just carries both builds and chooses at
launch. `HomeworldActivity` looks for `Homeworld.big` in the app's data
directory and then, failing that, in Downloads, Documents and the top of
shared storage: present means the full game and `libmain.so`, absent means the
demo and `libmainDemo.so`. The freely redistributable 1.05 demo assets ride
inside the APK and are unpacked on first run, so it plays without anything
being copied to the headset at all.

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
14a99d013dc3cab22448801dc2f8f8fc  homeworld-unbound-vr.apk  99M
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

- **Orders make a sound again.** The VR paths call `clWrap*` directly rather
  than going through the right-click menu callbacks, deliberately - `mrKeyPress`
  runs everything through `kbCheckBindings` and an unbound key vanishes without
  a word - and the feedback the mouse paths pick up on the way was lost with it.
  Moves get the pilots acknowledging, recalling a hotkey group clicks and names
  it, kamikaze clicks. Halt stays silent because the desktop is silent there
  too. `UI_MoveChimes` looked like the move confirmation and is not: it is the
  movement GUI's proximity click while a destination is being placed.
- **The move line starts where the fleet is.** It ran back to wherever the
  ships had been when the order began, which for a fleet already under way is
  a point in empty space. `selCentrePoint` is a global that only moves when
  something recomputes it, and nothing was.
- **The controls card knows about the sensor view.** It still described it as a
  screen to open, which is what every other entry on that card is and exactly
  the wrong idea.

- **The engine cannot run twice in one process, and Android reuses processes.**
  Quit the game and launch it again and the second run started `SDL_main` over
  globals the first had left behind, then died - a doubled path separator in
  `files//SoundFX/UIEvents.lut` being one of the ways. A third launch worked,
  because the crash had cleared the process out. The game now exits its process
  on quit, so every launch is cold, which is the only state the engine starts
  from correctly. This had been broken in every release and was hidden by the
  crash tidying up after itself.
- **A genuinely fresh install hung on its first launch.** Asking for All files
  access from `onCreate` opens a system Settings panel; the panel takes focus,
  SDL will not start the game thread without focus, and focus never returned to
  something that had never drawn a frame. Granting the permission did not
  release it. The request now happens when the player quits, by which point
  there is no running game to take focus from. Every earlier test inherited a
  uid-level grant from a previous install and so never saw the panel at all.

- **Adding your game data after playing the demo no longer needs two launches.**
  The first one appeared to hang in the headset and was in fact dying: the
  demo/full choice is compiled into the engine and fixed once a process has
  loaded one, so a warm restart ran the demo engine against full-game data and
  gave up looking for a file that was not there. It exits and reloads properly
  now instead.

- **The Sensors Manager is a place now, not a screen.** It used to be the map
  drawn flat on the panel at your wrist, which is the one screen in Homeworld
  where that costs something: it was never a screen to begin with, it is a 3D
  scene with its own camera, and it is where the game expects long-range
  movement and hyperspace to be issued. Opening it now pulls the whole
  battlespace into the room around you — the navigation disc under your feet,
  sensor coverage as spheres you can lean into, your fleet as points and class
  glyphs inside them. You can select, give orders, draw flight paths and zoom
  in on what you picked without ever leaving it, and closing it puts you back
  at whatever you were looking at.
- **Mission objectives are on the sensor view.** The pings the game uses to point at things — the drones the first mission asks you to destroy, where a battle has started, what a Proximity Sensor has found — were missing from it.
- **Dragging on the panel never worked.** Press and drag anywhere on the wrist
  panel and the pointer froze where you pressed until you let go. It took out
  band-box selection, the map's own Pan control and the ship view. The cause
  was that the game asks the mouse for movement rather than position once a
  drag starts, and a headset has no mouse to ask.

## Older changes

- **The full campaign no longer needs a computer.** Put your game files in
  **Downloads** — loose or in a folder of their own — grant the app **All files
  access**, and it reads them where they lie. Nothing is copied, so no second
  600 MB appears on the headset, and the files stay somewhere the headset's own
  file browser can reach. Android 11 closed the app's private folder to every
  file manager on the device, which is why this used to need adb and a PC. Any
  capitalisation works: `homeworld.big` and `Homeworld.big` both open, which
  they did not before.
- **The ships are sharp again.** Homeworld's surface textures are tiny — the
  Mothership's are 32x32 and 64x64 — and were drawn to be read at 1999
  resolutions. Smoothing them recovers no detail, because there is none to
  recover, and a headset magnifies them far past anything their author had in
  mind, so the blur was not subtle. Off by default now. Still a setting, under
  Options, Video, Custom Effects, "Surface Filtering", so this changes what a
  fresh install gets and nothing else.
- **Saving works when your saves were copied on by hand.** A `SavedGames`
  folder put on the headset with `adb push` or a file browser belongs to
  whoever copied it, not to the game, and the game could read it but not add to
  it. Old saves listed and loaded perfectly and every new save failed with
  "error writing to file, check disk space", which pointed nowhere near the
  real cause. `install.py` now sets those folders so the game can write to
  them, and when it cannot, it says why in the log instead of failing silently.
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
- **Only the host forwards two ports** on their router: **TCP 10500** and
  **UDP 10600**. Nobody else configures anything — anything a player cannot
  reach directly is now passed on by the host.
- **Both ends have to be on different home network ranges.** If you are both
  on `192.168.1.x`, one of you needs to change your router's range. LAN play
  is unaffected by all of this.
- **More than two players over the internet has not been tested.** Nothing is
  known to stand in the way of it any more, but that is not the same as it
  having worked.
