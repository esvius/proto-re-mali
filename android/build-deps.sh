#!/bin/bash
# Builds the third-party native libraries of the Android port for one ABI: SDL2, FAudio and DXVK (d3d9 only, SDL2
# WSI). Same script locally and on GitHub Actions; nothing here depends on the game files.
#
# Usage: android/build-deps.sh <abi: arm64-v8a | x86_64> <source dir> <install prefix>
# Needs ANDROID_NDK_HOME (or ANDROID_NDK_LATEST_HOME), cmake, ninja, meson, glslang, git.
set -euo pipefail

# CI diagnostics: keep a log and, when the script fails, print its interesting lines as GitHub error annotations.
BUILD_LOG=$(mktemp)
exec > >(tee "$BUILD_LOG") 2>&1
on_exit() {
    local rc=$?
    if [ $rc -ne 0 ] && [ -n "${GITHUB_ACTIONS:-}" ]; then
        sleep 1
        enc() { sed ':a;N;$!ba;s/%/%25/g;s/\r/%0D/g;s/\n/%0A/g'; }
        echo "::error title=build-deps errors::$(grep -nEi 'error|failed|fatal|no such|not found' "$BUILD_LOG" | tail -n 15 | cut -c1-300 | enc)"
        echo "::error title=build-deps tail::$(tail -n 25 "$BUILD_LOG" | cut -c1-300 | enc)"
    fi
    exit $rc
}
trap on_exit EXIT

ABI=${1:?abi}
PATCH_DIR=$(realpath "$(dirname "$0")")/patches   # absolute: the patch loops below run git -C <dir>
SRC=$(realpath -m "${2:?source dir}")
PREFIX=$(realpath -m "${3:?install prefix}")
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:?set ANDROID_NDK_HOME}}
API=26
JOBS=${JOBS:-$(nproc)}

SDL2_TAG=release-2.30.9
FAUDIO_TAG=25.10
# DXVK: fbraz3's 2.6-based fork + the Vulkan 1.1 / Mali-G76 patch series (android/patches/dxvk-vk11/), pinned.
DXVK_REPO=https://github.com/fbraz3/dxvk.git
DXVK_COMMIT=46a3bc018bcae408d49d3c500e4e536a11f6789a

case $ABI in
    arm64-v8a) TRIPLE=aarch64-linux-android; CPU_FAMILY=aarch64; CPU=armv8-a ;;
    x86_64) TRIPLE=x86_64-linux-android; CPU_FAMILY=x86_64; CPU=x86_64 ;;
    *) echo "unknown ABI $ABI" >&2; exit 2 ;;
esac

fetch() {  # fetch <dir> <url> <tag>
    [ -d "$SRC/$1" ] || git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "$2" "$SRC/$1"
}
mkdir -p "$SRC" "$PREFIX"
fetch SDL2 https://github.com/libsdl-org/SDL.git $SDL2_TAG
fetch FAudio https://github.com/FNA-XNA/FAudio.git $FAUDIO_TAG
if [ ! -d "$SRC/dxvk/.git" ]; then
    git init -q "$SRC/dxvk"
    git -C "$SRC/dxvk" remote add origin "$DXVK_REPO"
    git -C "$SRC/dxvk" fetch -q --depth 1 origin "$DXVK_COMMIT"
    git -C "$SRC/dxvk" checkout -q FETCH_HEAD
    git -C "$SRC/dxvk" submodule update --init --depth 1 include/vulkan include/spirv include/native/directx subprojects/libdisplay-info
fi

# Android patches (android/patches/<project>-*.patch), applied once.
for proj in SDL2 FAudio; do
    for p in "$PATCH_DIR"/"${proj,,}"-*.patch; do
        [ -e "$p" ] || continue
        git -C "$SRC/$proj" apply --check "$p" 2>/dev/null && git -C "$SRC/$proj" apply "$p" && echo "applied $(basename "$p")"
    done
done

# DXVK patch series, in file-name order. Each patch must apply (exactly, or with one line of context dropped) or the
# build fails: an unpatched DXVK must never ship. Already-applied patches are skipped, so reruns are safe.
for p in "$PATCH_DIR"/dxvk-vk11/*.patch; do
    if git -C "$SRC/dxvk" apply --reverse --check "$p" 2>/dev/null; then echo "already applied $(basename "$p")"
    elif git -C "$SRC/dxvk" apply --check "$p" 2>/dev/null; then git -C "$SRC/dxvk" apply "$p"; echo "applied $(basename "$p")"
    elif git -C "$SRC/dxvk" apply --check -C1 "$p" 2>/dev/null; then git -C "$SRC/dxvk" apply -C1 "$p"; echo "applied (-C1) $(basename "$p")"
    else echo "FAILED to apply $(basename "$p")" >&2; exit 1; fi
done

CMAKE_ANDROID=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI="$ABI"
               -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
               -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX")

# SDL2 (also installs SDL2.pc, which DXVK's meson build looks up).
cmake -S "$SRC/SDL2" -B "$SRC/build/SDL2-$ABI" "${CMAKE_ANDROID[@]}" -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$SRC/build/SDL2-$ABI" -j "$JOBS"
cmake --install "$SRC/build/SDL2-$ABI"

# FAudio on SDL2.
cmake -S "$SRC/FAudio" -B "$SRC/build/FAudio-$ABI" "${CMAKE_ANDROID[@]}" -DBUILD_SDL3=OFF -DBUILD_SHARED_LIBS=ON \
      -DSDL2_DIR="$PREFIX/lib/cmake/SDL2"
cmake --build "$SRC/build/FAudio-$ABI" -j "$JOBS"
cmake --install "$SRC/build/FAudio-$ABI"

# DXVK native, d3d9 only.
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CROSS=$SRC/build/android-$ABI.cross
cat > "$CROSS" <<EOF
[binaries]
c = '$TC/$TRIPLE$API-clang'
cpp = '$TC/$TRIPLE$API-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig'
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = '$CPU_FAMILY'
cpu = '$CPU'
endian = 'little'
EOF
rm -rf "$SRC/build/dxvk-$ABI"
meson setup "$SRC/build/dxvk-$ABI" "$SRC/dxvk" --cross-file "$CROSS" --buildtype release --prefix "$PREFIX" \
      -Denable_d3d9=true -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false \
      -Ddxvk_native_wsi=sdl2
ninja -C "$SRC/build/dxvk-$ABI" -j "$JOBS"
ninja -C "$SRC/build/dxvk-$ABI" install

echo "installed to $PREFIX:"
find "$PREFIX" -name "*.so" | sort
