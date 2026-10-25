#!/bin/bash
# Builds and runs the native-replacement difftest (hot_audio_test.cpp) on the host. Small: one generated file.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
DLL=${PROTORE_ENGINE:-$HOME/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll}
OUT=$ROOT/build/hot_test
mkdir -p "$OUT"
printf '105e8860\n105e85b0\n105c62e0\n105eb280\n1071b610\n' > "$OUT/functions.txt"
python3 -I "$ROOT/tools/recomp/recomp.py" --functions "$OUT/functions.txt" --out "$OUT/ref.c" --base 0x58000000 \
    --no-native >/dev/null
# Engine data pages read by the tested functions (constants and tables), as {DLL address, 4096 bytes} records.
python3 -I - "$DLL" "$OUT/pages.bin" <<'EOF'
import struct, sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
with open(sys.argv[2], "wb") as f:
    for page in (0x10d94000, 0x10e2e000, 0x10e31000, 0x1127f000, 0x11137000, 0x10da2000, 0x10e5b000):
        f.write(struct.pack("<I", page) + pe.get_data(page - 0x10000000, 0x1000).ljust(0x1000, b"\0"))
EOF
FLAGS="-O1 -DRECOMP_FLAT_MEMORY -fsigned-char -fno-strict-aliasing -fwrapv -I$ROOT/src/recomp -I$ROOT/src/native"
gcc -std=c11 $FLAGS -w -include math.h -c "$OUT/ref.c" -o "$OUT/ref.o"
g++ -std=c++17 $FLAGS -c "$ROOT/src/native/hot_audio.cpp" -o "$OUT/hot_audio.o"
g++ -std=c++17 $FLAGS -c "$ROOT/src/native/hot_skin.cpp" -o "$OUT/hot_skin.o"
g++ -std=c++17 $FLAGS "$ROOT/src/native/tests/hot_audio_test.cpp" "$OUT/ref.o" "$OUT/hot_audio.o" "$OUT/hot_skin.o" -o "$OUT/hot_audio_test"
"$OUT/hot_audio_test" "$OUT/pages.bin"
