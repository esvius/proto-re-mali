#!/usr/bin/env python3
"""Generates the D3D9 COM bridge of the native host (src/native/d3d9_gen.inc) from a d3d9.h with
DECLARE_INTERFACE_/STDMETHOD declarations (the MinGW header shipped in DXVK's include/native/directx).

The engine calls D3D9 through vtables in guest memory: every method gets a guest-callable thunk whose handler reads
the stdcall arguments from the guest stack, converts them (interfaces are unwrapped/wrapped, window handles mapped,
pointers to plain structs passed as is since guest memory is flat), calls the host (DXVK) object and returns.
Methods whose arguments need real conversion (structs with pointers, Lock/Unlock shadows) are listed in MANUAL and
implemented by hand in src/native/d3d9.cpp as Manual_<Interface>_<Method>(Ctx*).

Usage: gen_d3d9_bridge.py <d3d9.h> <out.inc>"""
import re
import sys

INTERFACES = ["IDirect3D9", "IDirect3DDevice9", "IDirect3DSwapChain9", "IDirect3DResource9", "IDirect3DSurface9",
              "IDirect3DVolume9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DBaseTexture9",
              "IDirect3DTexture9", "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9",
              "IDirect3DVertexDeclaration9", "IDirect3DVertexShader9", "IDirect3DPixelShader9",
              "IDirect3DStateBlock9", "IDirect3DQuery9"]

MANUAL = {
    "IDirect3D9::CreateDevice", "IDirect3D9::GetAdapterMonitor",
    "IDirect3DDevice9::Reset", "IDirect3DDevice9::Present", "IDirect3DDevice9::TestCooperativeLevel",
    "IDirect3DDevice9::SetSamplerState", "IDirect3DDevice9::GetCreationParameters",
    "IDirect3DDevice9::CreateAdditionalSwapChain", "IDirect3DDevice9::SetCursorProperties",
    "IDirect3DSwapChain9::Present", "IDirect3DSwapChain9::GetPresentParameters",
    "IDirect3DSurface9::LockRect", "IDirect3DSurface9::UnlockRect", "IDirect3DSurface9::GetDC",
    "IDirect3DSurface9::ReleaseDC", "IDirect3DSurface9::GetContainer", "IDirect3DVolume9::GetContainer",
    "IDirect3DTexture9::LockRect", "IDirect3DTexture9::UnlockRect",
    "IDirect3DCubeTexture9::LockRect", "IDirect3DCubeTexture9::UnlockRect",
    "IDirect3DVolumeTexture9::LockBox", "IDirect3DVolumeTexture9::UnlockBox",
    "IDirect3DVolume9::LockBox", "IDirect3DVolume9::UnlockBox",
    "IDirect3DVertexBuffer9::Lock", "IDirect3DVertexBuffer9::Unlock",
    "IDirect3DIndexBuffer9::Lock", "IDirect3DIndexBuffer9::Unlock",
    # texture adaptation for mobile GPUs (mip skipping, DXT decoding)
    "IDirect3D9::CheckDeviceFormat", "IDirect3DDevice9::CreateTexture", "IDirect3DDevice9::CreateCubeTexture",
    "IDirect3DTexture9::GetLevelDesc", "IDirect3DTexture9::GetLevelCount", "IDirect3DTexture9::GetSurfaceLevel",
    "IDirect3DCubeTexture9::GetLevelDesc", "IDirect3DCubeTexture9::GetLevelCount",
    "IDirect3DCubeTexture9::GetCubeMapSurface",
    # display modes follow the render resolution (render_height)
    "IDirect3D9::GetAdapterDisplayMode", "IDirect3D9::GetAdapterModeCount", "IDirect3D9::EnumAdapterModes",
    "IDirect3DDevice9::GetDisplayMode",
}

FLOAT_TYPES = {"float", "FLOAT"}


def parse(header):
    text = open(header).read()
    out = {}
    for m in re.finditer(r"DECLARE_INTERFACE_\((\w+)\s*,\s*\w+\)\s*\{(.*?)\n\};", text, re.S):
        name, body = m.group(1), m.group(2)
        methods = []
        for mm in re.finditer(r"STDMETHOD(?:_\(\s*([\w ]+?)\s*,)?\(?\s*(\w+)\)\((.*?)\)\s*PURE;", body, re.S):
            ret = mm.group(1) or "HRESULT"
            mname = mm.group(2)
            args = mm.group(3).strip()
            params = []
            if args.startswith("THIS_"):
                for p in args[5:].split(","):
                    p = " ".join(p.split())
                    pm = re.match(r"(.*?[\s*])(\w+)$", p)
                    if pm and pm.group(1).strip():
                        params.append((pm.group(1).strip(), pm.group(2)))
                    else:  # unnamed parameter
                        params.append((p, f"p{len(params)}"))
            methods.append((ret.strip(), mname, params))
        out[name] = methods
    return out


def iface_of(t):
    m = re.match(r"(?:const |CONST )?(?:struct )?(IDirect3D\w*9)\s*(\*+)$", t.replace(" *", "*").strip())
    if not m:
        return None, 0
    return m.group(1), len(m.group(2))


def conv_arg(t, i):
    """Expression converting guest argument slot i (0 = this) to the host parameter of type t."""
    t0 = " ".join(t.split())
    iface, stars = iface_of(t0)
    if iface and stars == 1:
        return f"Unwrap<{iface}>(Arg(c, {i}))"
    if t0 in FLOAT_TYPES:
        return f"ArgF(c, {i})"
    if t0 in ("REFIID", "REFGUID"):
        return f"*Ptr<GUID>(Arg(c, {i}))"
    if t0 == "HWND":
        return f"HostHwnd(Arg(c, {i}))"
    if t0.endswith("HANDLE*") or t0 == "HANDLE *":
        return "nullptr"
    if t0.endswith("*"):
        base = t0[:-1].strip()
        return f"reinterpret_cast<{t0}>(Ptr<void>(Arg(c, {i})))" if base not in ("void", "const void", "CONST void") \
            else f"Ptr<void>(Arg(c, {i}))"
    return f"({t0})Arg(c, {i})"


def gen(ifaces):
    lines = ["// Generated by tools/gen_d3d9_bridge.py; do not edit.", ""]
    for name in INTERFACES:
        methods = ifaces[name]
        for ret, mname, params in methods:
            full = f"{name}::{mname}"
            nargs = len(params) + 1
            fn = f"h_{name}_{mname}"
            if mname in ("QueryInterface", "Release"):  # shared templates in d3d9.cpp
                lines.append(f"static void {fn}(Ctx* c) {{ {mname}Impl<{name}>(c); }}")
                continue
            if mname in MANUAL or full in MANUAL:
                lines.append(f"void Manual_{name}_{mname}(Ctx* c);")
                lines.append(f"static void {fn}(Ctx* c) {{ Manual_{name}_{mname}(c); }}")
                continue
            pre, post, call_args = [], [], []
            for k, (t, pname) in enumerate(params, start=1):
                iface, stars = iface_of(" ".join(t.split()))
                if iface and stars == 2:
                    pre.append(f"{iface}* out{k} = nullptr;")
                    call_args.append(f"Arg(c, {k}) ? &out{k} : nullptr")
                    post.append(f"if (Arg(c, {k})) W32(Arg(c, {k}), Wrap(out{k}));")
                else:
                    call_args.append(conv_arg(t, k))
            call = f"Host<{name}>(Arg(c, 0))->{mname}({', '.join(call_args)})"
            body = " ".join(pre)
            if ret in ("void", "VOID"):
                body += f" {call}; {' '.join(post)} Return(c, 0, {nargs});"
            elif ret in FLOAT_TYPES:
                body += f" float r = {call}; {' '.join(post)} ReturnFStd(c, r, {nargs});"
            elif ret == "HMONITOR":
                body += f" {call}; Return(c, 1, {nargs});"
            else:
                body += f" auto r = {call}; {' '.join(post)} Return(c, (uint32_t)r, {nargs});"
            lines.append(f"static void {fn}(Ctx* c) {{ {body.strip()} }}")
        lines.append(f"static const MethodDef kMethods_{name}[] = {{")
        for _, mname, _ in methods:
            lines.append(f'    {{"{mname}", h_{name}_{mname}}},')
        lines.append("};")
        lines.append("")
    lines.append("static const InterfaceDef kInterfaces[] = {")
    for name in INTERFACES:
        lines.append(f'    {{"{name}", kMethods_{name}, sizeof(kMethods_{name}) / sizeof(MethodDef)}},')
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    ifaces = parse(sys.argv[1])
    missing = [n for n in INTERFACES if n not in ifaces]
    if missing:
        sys.exit(f"interfaces not found: {missing}")
    with open(sys.argv[2], "w") as f:
        f.write(gen(ifaces))
    print(f"{sum(len(ifaces[n]) for n in INTERFACES)} methods in {len(INTERFACES)} interfaces")


if __name__ == "__main__":
    main()
