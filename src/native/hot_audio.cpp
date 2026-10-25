// Native replacements of hot engine functions (tools/recomp/recomp.py NATIVE_OVERRIDES): the generated f_<addr>
// forwards to Native_<addr>. Each one follows the original's calling convention and leaves callee-saved registers
// and the x87 stack as the original did.
//
// Audio DSP (the engine's software mixer, the busiest code on phones): written as plain float C - the original
// ran x87 in single-precision mode and SSE, so per-operation float rounding matches; FP contraction is off to keep
// multiply/add pairs unfused.
#include <cmath>
#include <cstring>
#include <initializer_list>

#include "hot_common.h"

#pragma STDC FP_CONTRACT OFF
#pragma clang fp contract(off)

namespace native {

// FUN_105e8860 (cdecl, 4 args): resamples each channel of a voice into a 256-sample block (linear interpolation,
// 20.12 fixed-point phase over a power-of-two ring buffer), optionally meters it, and mixes it into the output
// bins with the voice/bin/output gains.
//   p1: mix context (+0xc output count, +0x10 focused output)
//   p2: voice (+0x42 meter flag, +0x44/+0x488/+0x48c/+0x4a8 gains, +0x48 rate, +0x94 channels, +0x98 ring size,
//       +0xa8 64-bit phase, +0xb0 per-channel gain, +0xf8 bin gains, +0x118 per-channel bin gains,
//       +0x3a0/+0x358 bin/output masks (per channel, +4 each), +0x3e8 output gains, +0x4b0 per-channel meter,
//       +0x4f8 first sample)
//   p3: source channels (stride 24, ring buffer pointer at +4)
//   p4: output bins (pointer per bin, 8 per output)
extern "C" void Native_105e8860(Ctx* c) {
    const uint32_t esp = c->esp;
    const uint32_t p1 = R32(esp + 4), p2 = R32(esp + 8), p3 = R32(esp + 12), p4 = R32(esp + 16);

    const float rate = F(p2 + 0x48) * F(E(0x10e2e9dc));
    const int64_t step = (rate != rate || rate >= 9223372036854775808.0f || rate < -9223372036854775808.0f)
                             ? INT64_MIN
                             : int64_t(rate);  // _ftol2: truncation
    const uint32_t mask = R32(p2 + 0x98) - 1;
    const uint32_t channels = R32(p2 + 0x94);
    const float frac_scale = F(E(0x10e2e864));

    float block[259] = {};  // the original reads (and mixes) up to 3 floats past the 256 it fills
    for (uint32_t ch = 0; ch < channels; ++ch) {
        const uint32_t meter = p2 + 0x4b0 + 4 * ch;
        const float gain = F(meter - 0x400);
        const float* ring = FP(R32(p3 + 4 + 24 * ch));
        int64_t phase = int64_t(R64(p2 + 0xa8));
        for (int i = 0; i < 256; ++i) {
            const float frac = float(uint32_t(phase) & 0xfff) * frac_scale;
            const uint32_t i0 = uint32_t(uint64_t(phase) >> 12) & mask;
            phase += step;
            const float a = (1.0f - frac) * ring[i0];
            const float b = ring[(i0 + 1) & mask] * frac;
            block[i] = a + b;
        }
        if (R8(p2 + 0x42) == 1) {  // FUN_105e85b0: RMS of the block
            float sum = 0.0f;
            for (int i = 0; i < 256; ++i) sum = block[i] * block[i] + sum;  // SSE, strictly in order
            const float scaled = sum * F(E(0x10e313a4));                   // x87, single precision
            WF32(meter, std::sqrt(scaled));
        }
        const uint32_t outputs = R32(p1 + 0xc);
        for (uint32_t o = 0; o < outputs; ++o) {
            const float out_gain = R32(p1 + 0x10) == o ? F(p2 + 0x48c) : F(p2 + 0x488);
            const float og = (out_gain * F(p2 + 0x4a8)) * F(p2 + 0x44);
            const float out_level = F(p2 + 1000 + 4 * o);
            for (uint32_t bin = 0; bin < 8; ++bin) {
                if (!(R32(meter - 0x110) & (1u << bin)) || !(R32(meter - 0x158) & (1u << (o & 31)))) continue;
                const float v = ((((F(p2 + 0x118 + (bin + ch * 8) * 4) * F(p2 + 0xf8 + 0x350 + bin * 4)) * out_level) *
                                  F(p2 + 0xf8 + bin * 4)) * og) * gain;
                if (!(0.0f < v)) continue;
                const uint32_t first = R32(p2 + 0x4f8);
                if (first >= 0x100) continue;
                float* dst = Ptr<float>(R32(p4 + (bin + o * 8) * 4));
                const uint32_t groups = ((0xff - first) >> 2) + 1;
                for (uint32_t k = first; k < first + groups * 4; ++k) dst[k] = block[k] * v + dst[k];
            }
        }
    }
    c->ret_target = Pop32(c);  // cdecl
}

// FUN_105c62e0 (cdecl, 1 arg): biquad IIR filter (direct form I), output accumulated.
//   arg: +0xc coefficients/state (16-byte aligned down: b0 b1 b2 a0 a1 a2, then x1 x2 y1 y2),
//        +0x10 input, +0x14 output (both 4-byte aligned down), +0x18 sample count
extern "C" void Native_105c62e0(Ctx* c) {
    const uint32_t arg = R32(c->esp + 4);
    const uint32_t st = R32(arg + 0xc) & ~0xfu;
    const float* in = FP(R32(arg + 0x10) & ~3u);
    float* out = Ptr<float>(R32(arg + 0x14) & ~3u);
    const int32_t count = int32_t(R32(arg + 0x18));
    const float inv = F(E(0x10d946e0)) / F(st + 0xc);
    const float b0 = F(st) * inv, b1 = F(st + 4) * inv, b2 = F(st + 8) * inv;
    const float a1 = F(st + 0x10) * inv, a2 = F(st + 0x14) * inv;
    float x1 = F(st + 0x18), x2 = F(st + 0x1c), y1 = F(st + 0x20), y2 = F(st + 0x24);
    for (int32_t i = 0; i < count; ++i) {
        const float x = in[i];
        const float y = (((x * b0 + x2 * b2) + x1 * b1) - y1 * a1) - y2 * a2;
        out[i] = out[i] + y;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
    }
    WF32(st + 0x20, y1);
    WF32(st + 0x18, x1);
    WF32(st + 0x1c, x2);
    WF32(st + 0x24, y2);
    c->ret_target = Pop32(c);  // cdecl
}

// FUN_105eb280 (cdecl, 4 args): IMA ADPCM decoding of `blocks` blocks (per channel: index and predictor header,
// 16 bytes = 32 samples) into each channel's float buffer (4-byte aligned down). It also takes scratch memory from
// the engine's allocator FUN_105ba610 (unused by the decoding itself; kept for the allocator's state).
//   p1: stream (+0xc channel destinations, +0x8c data, +0x94 channel count); p2*p3: byte offset of the first
//   block; p3: first block index; p4: block count
extern "C" void Native_105eb280(Ctx* c) {
    const uint32_t esp = c->esp;
    const uint32_t p1 = R32(esp + 4), p2 = R32(esp + 8), p3 = R32(esp + 12), blocks = R32(esp + 16);
    const uint8_t* src = Ptr<const uint8_t>(p2 * p3 + R32(p1 + 0x8c));
    uint32_t channels = R32(p1 + 0x94);
    uint32_t dest[64] = {};
    if (channels != 0) {
        CallGuestCdecl(c, E(0x105ba610), {blocks << 7, 0x10});
        for (uint32_t ch = 0; ch < channels && ch < 64; ++ch) dest[ch] = (R32(p1 + 0xc + 4 * ch) + p3 * 0x80) & ~3u;
    }
    const int32_t* steps = Ptr<const int32_t>(E(0x10e314f8));
    const int32_t* index_delta = Ptr<const int32_t>(E(0x10e314b8));
    const float scale = F(E(0x10e3165c));
    auto decode = [&](int32_t& index, int32_t& pred, uint32_t nibble) {
        const int32_t step = steps[index];
        int32_t diff = ((nibble & 1) ? step >> 2 : 0) + ((nibble & 2) ? step >> 1 : 0) + ((nibble & 4) ? step : 0) +
                       (step >> 3);
        pred += (nibble & 8) ? -diff : diff;
        if (pred > 0x7fff) pred = 0x7fff;
        if (pred < -0x8000) pred = -0x8000;
        int32_t next = index + index_delta[nibble];
        if (next > 0x58) next = 0x58;
        if (next < 0) next = 0;
        index = next;
        return pred;
    };
    for (uint32_t b = 0; b < blocks; ++b) {
        for (uint32_t ch = 0; ch < channels; ++ch) {
            int32_t index = int16_t(src[0] | src[1] << 8);
            int32_t pred = int16_t(src[2] | src[3] << 8);
            src += 4;
            float* out = Ptr<float>(b * 0x80 + dest[ch < 64 ? ch : 63]);
            for (int k = 0; k < 16; ++k) {
                const uint8_t byte = *src++;
                *out++ = float(decode(index, pred, byte & 0xf)) * scale;
                *out++ = float(decode(index, pred, byte >> 4)) * scale;
            }
            channels = R32(p1 + 0x94);
        }
    }
    c->ret_target = Pop32(c);  // cdecl
}

}  // namespace native
