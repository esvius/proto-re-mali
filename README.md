# proto-re-mali

**Prototype (2009) on Android phones with weaker Mali GPUs and Vulkan 1.1 drivers.**

This is my fork of [VinTeRuS/proto-re](https://github.com/VinTeRuS/proto-re), the native Android port of
Prototype. The original port needs a Vulkan 1.3 driver. My phone (Realme 8, Mali-G76 MC4) only has Vulkan 1.1,
so the game refused to start. I worked on getting it to run anyway, and this repository holds that work.

## Status

Tested on one device so far:

| | |
|---|---|
| Phone | Realme 8 |
| Chip / GPU | MediaTek Helio G95 / Mali-G76 MC4 |
| Vulkan driver | 1.1.177 (system driver) |
| Result | starts, menus work, **in-game rendering works** |

- Frame rate is limited by the CPU, like in the original port. I have not benchmarked this phone properly yet,
  so I am not giving numbers. Expect it to be playable-ish at best on this class of hardware.
- Other Mali GPUs (G52, G57, G68, ...) are **untested**. They may work, they may not.
- A full playthrough is not tested.

## What I changed

The original port builds DXVK 2.5.3 and calls Vulkan 1.3 functions that a Vulkan 1.1 driver does not have
(`vkCmdBeginRendering`, `vkCmdPipelineBarrier2` and others). On my phone those pointers are null, which means a crash
as soon as the first frame is cleared. What I did:

1. **New DXVK base.** Moved from DXVK 2.5.3 to the DXVK 2.6 based fork by
   [fbraz3](https://github.com/fbraz3/dxvk) and applied the Vulkan 1.1 / Mali-G76 patch series from
   [HousamKak/GeneralsZH-Android](https://github.com/HousamKak/GeneralsZH-Android) on top of it. The port's own
   DXVK changes (SDL2 window layer, Android surface, fixes) were carried over where they did not conflict.
2. **Dynamic rendering emulation**
   ([`39-mali-g76-dynamic-rendering-emulation.patch`](android/patches/dxvk-vk11/39-mali-g76-dynamic-rendering-emulation.patch)).
   When the driver has no `VK_KHR_dynamic_rendering`, every `vkCmdBeginRendering` / `vkCmdEndRendering` is turned
   into a normal render pass and framebuffer. This covers clears, blits, copies, mipmaps, resolves and the
   swapchain blitter in one place, instead of patching every call site.
3. **CI build.** The `android-deps` workflow builds `libdxvk_d3d9.so` for arm64 and publishes the stripped file to
   the `build-output` branch.

### Not in the source yet

To get the first working build I also edited the original release's `libmain.so` by hand, to accept a Vulkan 1.1
driver where it required 1.3. That edit is not in this repository as source code. Moving it into the build is on my list.

## Building

Same as the original port, see the build section of the upstream
[README](https://github.com/VinTeRuS/proto-re#building). The DXVK patch series lives in
`android/patches/dxvk-vk11/` and is applied in file name order by `android/build-deps.sh`.

## What is not included

- No game files, no original DLL, no code generated from it. You need your own copy of the game.
- No APK downloads. The original author does not want builds shared around, so I don't publish any.

## Reporting a problem

Attach the logs, otherwise there is nothing to look at: `proto-native.log`, `proto-native.prev.log` and
`app_process64_d3d9.log` from `Android/data/com.protore.game/files/`. Also write the phone model, chip, Android
version and when it happens.

---

## Credits and legal

**The port itself, the engine recompilation and almost everything in this repository were made by
[VinTeRuS](https://github.com/VinTeRuS). Original project: <https://github.com/VinTeRuS/proto-re>.**
All credit for the Android port goes to them. This fork only adds Vulkan 1.1 / Mali compatibility work on top.

Third-party work used here: [DXVK](https://github.com/doitsujin/dxvk) and the
[fbraz3](https://github.com/fbraz3/dxvk) fork, [GeneralsZH-Android](https://github.com/HousamKak/GeneralsZH-Android)
patches, [SDL2](https://github.com/libsdl-org/SDL), [FAudio](https://github.com/FNA-XNA/FAudio). Each keeps its own
license.

Prototype is a trademark of Activision Publishing, Inc. © 2009 Activision Publishing, Inc. / Radical Entertainment.
This project is not affiliated with or endorsed by Activision, and it does not bypass any DRM.

Original port © VinTeRuS. Rights in the original code stay with its author; nothing here grants a license to it.
Mali compatibility changes and patches in this repository © 2026 esvius. All rights reserved.

**Original port by VinTeRuS · Mali / Vulkan 1.1 compatibility by esvius**
