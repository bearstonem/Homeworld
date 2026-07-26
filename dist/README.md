# Prebuilt APKs

Built from `da24cf1` on 2026-07-26.

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
70654905e8817e6a41d458a84133e7a2  homeworld-unbound-vr-demo.apk  34M
22d0297f3a5613f332806ec947521acd  homeworld-unbound-vr-full.apk  34M
```
