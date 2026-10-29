# proto-re-mali

**Prototype (2009) on Android, including phones with Mali GPUs and Vulkan 1.1 / 1.2 drivers.**

*A continuation of [VinTeRuS/proto-re](https://github.com/VinTeRuS/proto-re), which its author archived at v0.2.0. See [Credits](#credits-and-legal).*

The original Android port needs a Vulkan 1.3 driver, so most Mali phones could not start it. This project makes it run
on them, fixes what got in the way (a crash at start, 10 FPS, stutter) and adds a few things I wanted while playing.
The APK contains no game data, you copy the files from your own copy of the game.

## Status

| | |
|---|---|
| Tested on | Realme 8, MediaTek Helio G95, Mali-G76 MC4 |
| Vulkan driver | 1.1.177 (system driver) |
| Result | menus, missions and combat work; about 30 to 40 FPS in gameplay at 540p with half size textures |

- Menus run at well over 100 FPS. Gameplay is CPU bound, like in the original port.
- The first minutes can stutter (DXVK compiles shaders it has not seen yet, and every new area loads textures). It
  settles down after that, and the DXVK cache in the app folder makes it better each run. Do not delete it.
- A full playthrough is not tested. Other Mali GPUs (G52, G57, G68, ...) are untested.
- Videos (Bink) do not play, same as in the original port.

## What this version has

- **Runs on Vulkan 1.1.** DXVK 2.6 with a Vulkan 1.1 patch series, plus my own patches in
  [`android/patches/dxvk-vk11`](android/patches/dxvk-vk11):
  - `39` emulates `vkCmdBeginRendering` with legacy render passes (the crash at start on these drivers)
  - `40` stops DXVK from rebuilding the swap chain every frame on Android (gameplay went from 10 to 14 FPS to 36 to 38)
  - `41` runs the Present waits without holding the device lock, so worker threads are not stalled behind the GPU
- **Power mode.** Tap POWERS: the game's power wheel opens, everything else is hidden, and the 11 wheel entries show as
  buttons on the right, in the same places as in the game's wheel. Tap one and the game selects it.
- **Show FPS** shows FPS, RAM, GPU name, driver and Vulkan version, one under the other.
- Vibration is gone from the settings (it is off), and the settings panel links here.

### Notes

The release APK is the original v0.1.9 APK with three parts replaced: the DXVK library (built by the `android-deps`
workflow), the Java classes (built by the `android-dex` workflow) and the manifest version name. `libmain.so` also
has one small binary edit that makes it accept a Vulkan 1.1 driver; that edit is not in this repository as source yet.

I started from v0.1.9. The changes in v0.2.0 of the original port (extra disguise buttons, the setup file check, a
crash fix for Mali GPUs without null descriptors) are not merged yet.

## Installing

Android 10+, arm64, and your own copy of the PC game (Steam app 10150, build 19788432).

1. Install the APK from [Releases](../../releases) and start it. Without the game files it shows where to put them.
2. Copy `prototypeenginef.dll`, all `.rcf` and `.p3d` files and the `art`, `audio`, `scripts` folders to either
   `/sdcard/Android/data/com.protore.game/files/game/` (copy from a PC over USB on Android 11+) or `/sdcard/Prototype/`
   (any file manager; tap "Allow file access" on the setup screen).
3. Optionally put a `protore.ini` in `/sdcard/Android/data/com.protore.game/files/` (readable, `chmod 666`).
   A good start for a Mali phone:

```ini
render_height = 540
texture_skip_mips = 1
fps_limit = 0
battery_saver = 0
```

| key | |
|---|---|
| `render_height` | render height in pixels, 540 is a good value for Mali-G76 |
| `texture_skip_mips` | 0 original, 1 half, 2 quarter size textures |
| `fps_limit` | 30 / 45 / 60, 0 = off |
| `battery_saver` | 30 fps, no big core pinning |
| `perf_hint` | ADPF on/off (not available on the phone I tested) |
| `vulkan_driver` | `auto` / `system` / `turnip` |
| `audio_driver` | `aaudio` (default) / `openslES` |

## Reporting a problem

Attach the logs, otherwise there is nothing to look at: `proto-native.log`, `proto-native.prev.log` and
`app_process64_d3d9.log` from `Android/data/com.protore.game/files/`. Also write the phone model, the chip, the Android
version, the APK version and when it happens.

## Building

The DXVK library and the Java classes are built by the workflows in `.github/workflows`; they publish to the
`build-output` and `build-output-dex` branches. Building the engine library needs the original DLL and the tools in
`tools/recomp`, see the build notes of the original port.

---

## Credits and legal

**The Android port itself, the static recompilation of the engine and most of the code in this repository were made by
[VinTeRuS](https://github.com/VinTeRuS).** They built it for Vulkan 1.3 only, listed Vulkan 1.1 / 1.2 GPUs as a known
issue in the v0.2.0 release notes and archived the project there. This repository continues from that point.
Original project: <https://github.com/VinTeRuS/proto-re>.

Third-party work: [DXVK](https://github.com/doitsujin/dxvk), the [fbraz3](https://github.com/fbraz3/dxvk) fork,
[GeneralsZH-Android](https://github.com/HousamKak/GeneralsZH-Android) patches, [SDL2](https://github.com/libsdl-org/SDL),
[FAudio](https://github.com/FNA-XNA/FAudio). Each keeps its own license.

Prototype is a trademark of Activision Publishing, Inc. © 2009 Activision Publishing, Inc. / Radical Entertainment.
This project is not affiliated with or endorsed by Activision and does not bypass any DRM.

Original port © VinTeRuS. Rights in the original code stay with its author; nothing here grants a license to it.
Vulkan 1.1 / Mali compatibility changes, the power mode, the overlay and the patches in this repository © 2026 esvius.
All rights reserved.
