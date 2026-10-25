#!/usr/bin/env python3
"""Generate hook registry entries from the engine's prologues.

For each "name rva symbol" line on stdin, decodes the first instructions at the rva (llvm-objdump), takes whole
instructions until at least 5 bytes are covered and prints a registry initializer. A CALL/JMP rel32 inside the
stolen bytes is recorded as rel32_at (re-based by DetourAttach); any other relative branch makes the function
unhookable and is reported on stderr instead.

Usage: tools/hookgen.py < list.txt   (lines: <name> <rva hex> <C++ replacement expression>)
"""
import os
import subprocess
import sys

DLL = os.environ.get("PROTORE_ENGINE",
                     os.path.expanduser("~/Downloads/Prototype (2009)/Prototype/prototypeenginef.dll"))
BASE = 0x10000000


def instructions(rva, length=24):
    start = BASE + rva
    out = subprocess.run(["llvm-objdump", "-d", f"--start-address={start}", f"--stop-address={start + length}", DLL],
                         capture_output=True, text=True, check=True).stdout
    result = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or ":" not in parts[0]:
            continue
        head = parts[0].strip()
        try:
            int(head.split(":")[0], 16)
        except ValueError:
            continue
        raw = head.split(":", 1)[1].split()
        mnemonic = parts[1].strip().split()[0] if len(parts) > 1 and parts[1].strip() else ""
        result.append((bytes(int(b, 16) for b in raw), mnemonic))
    return result


def function_bounds():
    """rva -> size of every non-thunk function, and the sorted list of function starts (rva)."""
    csv = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "re", "functions.csv")
    sizes = {}
    with open(csv) as f:
        next(f)
        for row in f:
            parts = row.split(",")
            if parts[2] == "false":
                sizes[int(parts[0], 16) - BASE] = int(parts[1])
    return sizes, sorted(sizes)


SIZES, STARTS = function_bounds()


def next_function_start(rva):
    import bisect
    i = bisect.bisect_right(STARTS, rva)
    return STARTS[i] if i < len(STARTS) else None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, rva_text, expr = line.split(None, 2)
        rva = int(rva_text, 16)
        size = SIZES.get(rva)
        stolen = b""
        rel32_at = -1
        problem = None
        if size is None:
            problem = "not a function start in functions.csv"
        for raw, mnemonic in ([] if problem else instructions(rva)):
            if len(stolen) >= 5:
                break
            if len(stolen) >= size and any(b != 0xCC for b in raw):
                problem = "function shorter than 5 bytes and not followed by int3 padding"
                break
            if raw[0] in (0xE8, 0xE9) and len(raw) == 5:
                rel32_at = len(stolen)
            elif raw[0] in range(0x70, 0x80) or raw[0] == 0xEB or (raw[0] == 0x0F and 0x80 <= raw[1] <= 0x8F) or \
                    raw[0] in (0xE0, 0xE1, 0xE2, 0xE3):
                problem = f"relative branch '{mnemonic}' in the first {len(stolen) + len(raw)} bytes"
                break
            stolen += raw
        following = next_function_start(rva)
        if not problem and following is not None and rva + len(stolen) > following:
            problem = f"patch would reach the next function at {following:#010x}"
        if problem or len(stolen) < 5:
            print(f"// {name} (rva {rva:#010x}): not hookable: {problem or 'function shorter than 5 bytes'}",
                  file=sys.stderr)
            continue
        expected = ", ".join(f"0x{b:02x}" for b in stolen)
        tail = f", {rel32_at}" if rel32_at >= 0 else ""
        print(f'    {{"{name}", 0x{rva:08x}, reinterpret_cast<void*>({expr}),\n'
              f'     {{{expected}}}, {len(stolen)}, {len(stolen)}, true, {{}}{tail}}},')


if __name__ == "__main__":
    main()
