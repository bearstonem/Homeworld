# Building GoK for Android (and Meta Quest)

The Android port reuses the OpenGL ES 1.1 renderer (`-Dgles=true`).
SDL's Java activity (`org.libsdl.app.SDLActivity`) loads the game as
`libmain.so` and calls its `SDL_main` entry point.

## Prerequisites

- Android SDK with platform 34 and build-tools
- Android NDK (tested with r26)
- JDK 17
- The usual GoK build environment for the build machine (meson, flex,
  bison, ...) — `nix develop ./Linux` provides it

## 1. Build SDL2 for Android

Download and unpack an SDL2 release (tested with 2.32.8) to `android/SDL`,
then:

```sh
cd android
cmake -S SDL -B build-sdl-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/sdl-prefix-arm64
cmake --build build-sdl-arm64 -j$(nproc)
cmake --install build-sdl-arm64
```

## 2. Cross-compile the game

Adjust the NDK paths in `android/aarch64-android.meson-cross-build-definition.txt`
to your installation, then from the repository root:

```sh
meson setup --cross-file android/aarch64-android.meson-cross-build-definition.txt \
    --buildtype=release -Db_sanitize=none -Dgles=true -Dmovies=false -Ddemo=true \
    build.android
meson compile -C build.android
```

This produces `build.android/libmain.so`.

> The cross file points pkg-config at `android/sdl-prefix-arm64` so the
> build machine's own SDL2 can not leak into the cross build.

## 3. Package the APK

The gradle project in `android/project` packages prebuilt native libraries
(no NDK build happens inside gradle):

```sh
cp sdl-prefix-arm64/lib/libSDL2.so ../build.android/libmain.so \
    project/app/src/main/jniLibs/arm64-v8a/
cd project
./gradlew assembleDebug
```

The APK lands in `project/app/build/outputs/apk/debug/`.

## 4. Install and provide game data

```sh
adb install app/build/outputs/apk/debug/app-debug.apk
```

The game runs out of its external-storage directory and expects the game
data (.big files etc.) there. For the demo assets:

```sh
adb shell mkdir -p /sdcard/Android/data/org.gardensofkadesh.homeworld/files
adb push subprojects/demo-assets-1.05/assets/. \
    /sdcard/Android/data/org.gardensofkadesh.homeworld/files/
```

(For the full game, build with `-Ddemo=false` and push the original
Homeworld data files instead.)

Settings, saves and screenshots are written to the same directory.

## Meta Quest

The Quest runs standard Android APKs as flat 2D panel apps: enable
developer mode, connect via adb, then install/push as above. Native
VR (OpenXR) support is a separate, future step.
