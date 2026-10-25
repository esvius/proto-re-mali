#!/bin/bash
# Builds the Adreno driver stack of the Android port for arm64: Mesa's Turnip Vulkan driver (freedreno, KGSL
# kernel interface) and libadrenotools, which loads it in place of the system driver. DXVK needs Vulkan features
# Qualcomm's own drivers often lack; Turnip has them. Same script locally and on GitHub Actions; nothing here
# depends on the game files.
#
# Usage: android/build-turnip.sh <source dir> <install prefix>
# Needs ANDROID_NDK_HOME (or ANDROID_NDK_LATEST_HOME), cmake, ninja, meson >= 1.4, python3 with mako and pyyaml,
# bison, flex, glslangValidator, git.
# Output: <prefix>/lib/libvulkan_freedreno.so and libvulkan_freedreno_next.so, <prefix>/lib/libadrenotools.a and
# the adrenotools hook libraries
# (<prefix>/lib/lib*_hook.so, lib*_impl.so: shipped in the APK next to libmain.so), headers in <prefix>/include.
set -euo pipefail

SRC=$(realpath -m "${1:?source dir}")
PREFIX=$(realpath -m "${2:?install prefix}")
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_LATEST_HOME:?set ANDROID_NDK_HOME}}
API=29  # adrenotools uses memfd from API 29; older Android is not targeted for Adreno
JOBS=${JOBS:-$(nproc)}

# Two Turnip builds: the 26.2 release (libvulkan_freedreno.so; tested on Adreno 660/735/740) and 26.3, which adds
# Adreno 710/720 (FD710/FD720) but broke startup on an Adreno 740 (libvulkan_freedreno_next.so). The game tries
# them in that order (src/native/vulkan_driver.cpp).
MESA_TAG=mesa-26.2.4
MESA_NEXT_TAG=mesa-26.3.0-rc2
ADRENOTOOLS_REV=8fae8ce

mkdir -p "$SRC" "$PREFIX/lib" "$PREFIX/include"
for tag in $MESA_TAG $MESA_NEXT_TAG; do
    [ -d "$SRC/$tag" ] || git clone -q --depth 1 --branch $tag https://gitlab.freedesktop.org/mesa/mesa.git "$SRC/$tag"
done
if [ ! -d "$SRC/libadrenotools" ]; then
    git clone -q --recurse-submodules https://github.com/bylaws/libadrenotools.git "$SRC/libadrenotools"
    git -C "$SRC/libadrenotools" checkout -q $ADRENOTOOLS_REV
    git -C "$SRC/libadrenotools" submodule update -q --init --recursive
fi

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CROSS=$SRC/build/android-turnip.cross
mkdir -p "$SRC/build"
cat > "$CROSS" <<EOF
[binaries]
c = '$TC/aarch64-linux-android$API-clang'
cpp = '$TC/aarch64-linux-android$API-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
pkg-config = '/bin/false'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8-a'
endian = 'little'
EOF

# Turnip only: no OpenGL, no LLVM, KGSL (the Adreno kernel driver Android ships) and Android's window system.
# android-stub links against stub copies of the platform's private libraries instead of an AOSP tree. libc++ is linked
# statically: the APK ships no libc++_shared.so, and without it the driver fails to load (adrenotools then silently
# hands out the system driver).
build_turnip() {  # <tag> <output name>
    local build="$SRC/build/$1"
    [ -f "$build/build.ninja" ] ||
    meson setup "$build" "$SRC/$1" --cross-file "$CROSS" --buildtype release --prefix "$PREFIX" \
          -Dplatforms=android -Dplatform-sdk-version=$API -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
          -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl \
          -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
          -Dllvm=disabled -Dzstd=disabled -Dexpat=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
          -Dvulkan-layers= -Dtools= -Dbuild-tests=false -Db_ndebug=true \
          -Dc_link_args=-static-libstdc++ -Dcpp_link_args=-static-libstdc++
    ninja -C "$build" -j "$JOBS" src/freedreno/vulkan/libvulkan_freedreno.so
    cp "$build/src/freedreno/vulkan/libvulkan_freedreno.so" "$PREFIX/lib/$2"
}
build_turnip $MESA_TAG libvulkan_freedreno.so
build_turnip $MESA_NEXT_TAG libvulkan_freedreno_next.so

# libadrenotools (static) and its hook libraries.
cmake -S "$SRC/libadrenotools" -B "$SRC/build/adrenotools" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-$API -DCMAKE_BUILD_TYPE=Release
cmake --build "$SRC/build/adrenotools" -j "$JOBS"
find "$SRC/build/adrenotools" -name "libadrenotools.a" -exec cp {} "$PREFIX/lib/" \;
find "$SRC/build/adrenotools" -name "*.a" -path "*linkernsbypass*" -exec cp {} "$PREFIX/lib/" \;
find "$SRC/build/adrenotools" -name "*.so" -exec cp {} "$PREFIX/lib/" \;
cp -r "$SRC/libadrenotools/include/adrenotools" "$PREFIX/include/"

echo "installed to $PREFIX:"
find "$PREFIX/lib" -maxdepth 1 \( -name "*.so" -o -name "*.a" \) | sort
