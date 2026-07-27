# Prebuilt APKs

Built from `8807ae2` on 2026-07-27.

## What changed since the last build

- **Ships are lit from one direction again.** The eye and quad swapchains were
  being created as linear `GL_RGBA8`, which tells the compositor that the pixels
  are raw light and still need a gamma encode on the way to the panel. They had
  already been encoded, so they got encoded twice and every midtone lifted. A
  shadowed hull the shader set to 0.19 arrived at 0.45, deep space came out a
  milky navy instead of black, and most of the contrast between a ship's sunward
  and shadowed sides went with it. The swapchains are sRGB now, with the GLES
  write conversion switched off, which measured the shadow side back down to
  0.174 and roughly doubled hull contrast. These APKs look considerably darker
  than the previous ones. That is how the game is supposed to look. Details in
  `VR_PORT_HANDOFF.md`.

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
e9c36725d18884f2486998e7c975b75a  homeworld-unbound-vr-demo.apk  34M
5c91501c25d66c805317c3b087d7041f  homeworld-unbound-vr-full.apk  34M
```
