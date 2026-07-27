# Prebuilt APKs

Built from `bf92ff1` on 2026-07-27.

## What changed since the last build

- **Double-focus no longer lands on top of the ship.** Focusing twice zoomed
  to a framing worked out for a monitor's field of view; through a headset,
  with ships at true physical scale, that put you inside the hull. Now four
  times that distance. Manual zoom is untouched, so you can still push in
  closer deliberately.
- **Managers ride the left wrist.** Construction, Research, Sensors, Launch and
  Trade appear on the wrist panel alongside the main menu rather than taking
  over the view. Sized so the type stays as large as it was.
- **Sensors is on the command wheel at top level.** Flick the wedge to open it;
  dwell on it for the View submenu that was there before.
- **The Mothership stops stealing clicks.** Its collision sphere is far wider
  than the ship, so aiming at anything parked alongside selected the
  Mothership instead. Its hit sphere and selection ring are both half size.
- **Drawn flight paths curve properly.** The spline is centripetal
  Catmull-Rom now, which cannot overshoot or loop the way the old one did on
  an uneven stroke, and hand tremor is filtered before smoothing.
- **The wrist controls card is readable.** Grouped by hand, abbreviations
  spelled out, and it now says what a dim ray means.

Pick ONE - the demo/full choice is compiled in, not detected at runtime.
Installing the wrong one fails at startup with
`Unable to open required .big file`.

| APK | Needs | Opens |
|---|---|---|
| `homeworld-unbound-vr-demo.apk` | nothing - demo assets ship with this repo | `HomeworldDL.big`, `DL_Music.wxd`, `DL_Demo.vce` |
| `homeworld-unbound-vr-full.apk` | your own copy of Homeworld | `Homeworld.big`, `HW_Music.wxd`, `HW_comp.vce` |

`install.py` at the repository root installs either one and copies the
matching game data. Both are debug-signed, arm64-v8a, Quest only.

```
70ceccbf0da4962bace4b522666b74fa  homeworld-unbound-vr-demo.apk  34M
e03d1687261590afe5e1380c0d19d91c  homeworld-unbound-vr-full.apk  34M
```
