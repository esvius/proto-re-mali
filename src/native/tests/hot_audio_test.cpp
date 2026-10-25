// Difftest for native replacements (src/native/hot_*.cpp): the recompiled original and the native version run on
// the same random inputs in guest (low 4 GiB) memory and must produce the same outputs.
//
// Build/run: src/native/tests/run_hot_test.sh (generates the reference with recomp.py --no-native).
#include <sys/mman.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "../guest.h"

extern "C" {
volatile uint32_t RecompRestrictLo = 0, RecompRestrictHi = 0xffffffffu, RecompTestDetours = 0;
uint8_t* RecompMemoryBase = nullptr;
void f_585e8860(Ctx* c);  // recompiled references (ref.c)
void f_585c62e0(Ctx* c);
void f_585eb280(Ctx* c);
void f_5871b610(Ctx* c);
void Native_1071b610(Ctx* c);
void Native_105e8860(Ctx* c);
void Native_105c62e0(Ctx* c);
void Native_105eb280(Ctx* c);
// Stand-in for the engine allocator FUN_105ba610 (cdecl): a fixed scratch block.
static uint32_t g_alloc_calls = 0;
static void FakeAlloc(Ctx* c) {
    ++g_alloc_calls;
    c->eax = 0x2f000000u;
    c->ret_target = Pop32(c);
}
void RecompDispatch(Ctx* c, uint32_t target) {
    if (RecompFn fn = RecompLookup(target)) {
        fn(c);
        return;
    }
    fprintf(stderr, "unexpected dispatch to %08x\n", target);
    exit(2);
}
// Fake render settings object for FUN_1071b610: vf[0x60](flags) returns the settings, vf[0x1c]() the skip flag.
static uint32_t g_skip = 0;
static void FakeGetSettings(Ctx* c) {
    c->eax = 0x2e000200u;
    c->ret_target = Pop32(c);
    c->esp += 4;
}
static void FakeSkipFlag(Ctx* c) {
    c->eax = g_skip;
    c->ret_target = Pop32(c);
}
RecompFn RecompLookup(uint32_t va) {
    if (va == 0x585ba610u) return FakeAlloc;
    if (va == 0x2e100060u) return FakeGetSettings;
    if (va == 0x2e10001cu) return FakeSkipFlag;
    return nullptr;
}
void RecompUnimplemented(Ctx*, uint32_t va, const char* what) {
    fprintf(stderr, "trap %08x %s\n", va, what);
    exit(2);
}
void RecompCpuid(Ctx*) {}
}

namespace {

uint32_t g_next = 0x20000000u;
uint32_t Alloc(uint32_t size) {
    uint32_t a = g_next;
    g_next += (size + 63) & ~63u;
    return a;
}

void MapAt(uint32_t addr, uint32_t size) {
    void* p = mmap(reinterpret_cast<void*>(uintptr_t(addr)), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != reinterpret_cast<void*>(uintptr_t(addr))) {
        perror("mmap");
        exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Engine data pages the functions read (constants, tables): dumped from the DLL by run_hot_test.sh as
    // records of {uint32 DLL address, 4096 bytes}.
    if (argc < 2) return 1;
    FILE* pages = fopen(argv[1], "rb");
    if (!pages) return 1;
    for (uint32_t dll_addr; fread(&dll_addr, 4, 1, pages) == 1;) {
        uint32_t a = dll_addr - 0x10000000u + native::kEngineBase;
        MapAt(a, 0x1000);
        if (fread(reinterpret_cast<void*>(uintptr_t(a)), 1, 0x1000, pages) != 0x1000) return 1;
    }
    fclose(pages);
    MapAt(0x2f000000u, 1u << 20);  // FakeAlloc scratch
    MapAt(0x2e000000u, 0x1000);     // fake render settings objects
    W32(0x2e000000u, 0x2e000100u);  // render object -> vtable
    W32(0x2e000100u + 0x60, 0x2e100060u);
    W32(0x2e000200u, 0x2e000300u);  // settings object -> vtable
    W32(0x2e000300u + 0x1c, 0x2e10001cu);
    W32(0x1127f354u - 0x10000000u + native::kEngineBase, 0x2e000000u);
    W8(0x111378b0u - 0x10000000u + native::kEngineBase, 0);  // debug asserts off
    MapAt(0x20000000u, 64u << 20);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uf(-1.0f, 1.0f);
    int failures = 0;
    for (int trial = 0; trial < 200; ++trial) {
        g_next = 0x20000000u;
        const uint32_t ring_size = 1u << (8 + trial % 4);
        const uint32_t channels = 1 + trial % 3, outputs = 1 + (trial / 3) % 3;
        uint32_t p1 = Alloc(0x40), p2 = Alloc(0x600), p3 = Alloc(24 * channels), p4 = Alloc(4 * 8 * outputs);
        memset(native::Ptr<void>(p2), 0, 0x600);
        for (uint32_t off = 0x44; off < 0x500; off += 4) WF32(p2 + off, 0.25f + 0.75f * (uf(rng) + 1.0f));
        W32(p1 + 0xc, outputs);
        W32(p1 + 0x10, trial % 2);
        W8(p2 + 0x42, trial % 3 == 0 ? 0 : 1);
        WF32(p2 + 0x48, 0.3f + (uf(rng) + 1.0f));
        W32(p2 + 0x94, channels);
        W32(p2 + 0x98, ring_size);
        W64(p2 + 0xa8, uint64_t(rng()) * 4096u + rng() % 4096);
        W32(p2 + 0x4f8, trial % 7 == 0 ? 0x100 : rng() % 256);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            W32(p2 + 0x4b0 + 4 * ch - 0x110, rng());
            W32(p2 + 0x4b0 + 4 * ch - 0x158, rng());
            uint32_t ring = Alloc(4 * ring_size);
            for (uint32_t i = 0; i < ring_size; ++i) WF32(ring + 4 * i, uf(rng));
            W32(p3 + 4 + 24 * ch, ring);
        }
        std::vector<uint32_t> bins;
        for (uint32_t b = 0; b < 8 * outputs; ++b) {
            uint32_t buf = Alloc(4 * 272);
            for (uint32_t i = 0; i < 272; ++i) WF32(buf + 4 * i, uf(rng));
            W32(p4 + 4 * b, buf);
            bins.push_back(buf);
        }
        // Snapshot everything the function may write, run both, compare.
        const uint32_t lo = 0x20000000u, size = g_next - lo;
        std::vector<uint8_t> start(size), ref(size);
        memcpy(start.data(), native::Ptr<void>(lo), size);
        uint32_t stack = Alloc(0x4000) + 0x3000;
        auto run = [&](void (*fn)(Ctx*)) {
            memset(native::Ptr<void>(stack - 0x3000), 0, 0x4000);  // same "garbage" past the block for both
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.fpu_cw = 0x007f;  // single precision: D3D9 sets it without D3DCREATE_FPU_PRESERVE
            c.esp = stack;
            Push32(&c, p4);
            Push32(&c, p3);
            Push32(&c, p2);
            Push32(&c, p1);
            Push32(&c, 0x12345678u);
            fn(&c);
            return c;
        };
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx a = run(f_585e8860);
        memcpy(ref.data(), native::Ptr<void>(lo), size);
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx b = run(Native_105e8860);
        if (a.esp != b.esp || a.ret_target != b.ret_target || a.st_top != b.st_top || a.ebx != b.ebx ||
            a.esi != b.esi || a.edi != b.edi || a.ebp != b.ebp) {
            printf("trial %d: register state differs (esp %08x/%08x)\n", trial, a.esp, b.esp);
            ++failures;
            continue;
        }
        // Compare as floats inside the bins and meters (the stack area is scratch).
        int diffs = 0;
        for (uint32_t off = 0; off + 4 <= stack - 0x3000 - lo; off += 4) {
            float fa, fb;
            memcpy(&fa, ref.data() + off, 4);
            memcpy(&fb, native::Ptr<uint8_t>(lo) + off, 4);
            if (memcmp(&fa, &fb, 4) != 0 && !(std::isnan(fa) && std::isnan(fb))) {
                if (diffs++ < 3) printf("trial %d: +%#x ref %.9g native %.9g\n", trial, off, fa, fb);
            }
        }
        if (diffs) {
            printf("trial %d: %d differing words\n", trial, diffs);
            ++failures;
        }
    }
    printf("mixer %s: %d/200 trials differ\n", failures ? "FAIL" : "OK", failures);
    int mixer_failures = failures;
    failures = 0;

    // FUN_105c62e0: biquad on random coefficients/state, misaligned pointers, various lengths.
    for (int trial = 0; trial < 300; ++trial) {
        g_next = 0x20000000u;
        const uint32_t count = trial % 37;
        uint32_t arg = Alloc(0x20), st = Alloc(0x40) + (trial % 4) * 4;  // aligned down to 16 by the code
        uint32_t in = Alloc(4 * 64) + (trial % 3), out = Alloc(4 * 64) + (trial % 2);
        for (uint32_t i = 0; i < 0x40; i += 4) WF32((st & ~0xfu) + i, uf(rng));
        WF32((st & ~0xfu) + 0xc, 0.5f + (uf(rng) + 1.0f));  // a0 away from zero
        for (uint32_t i = 0; i < 64; ++i) {
            WF32((in & ~3u) + 4 * i, uf(rng));
            WF32((out & ~3u) + 4 * i, uf(rng));
        }
        W32(arg + 0xc, st);
        W32(arg + 0x10, in);
        W32(arg + 0x14, out);
        W32(arg + 0x18, count);
        const uint32_t lo = 0x20000000u, size = g_next - lo;
        std::vector<uint8_t> start(size), ref(size);
        memcpy(start.data(), native::Ptr<void>(lo), size);
        uint32_t stack = Alloc(0x4000) + 0x3000;
        auto run = [&](void (*fn)(Ctx*)) {
            memset(native::Ptr<void>(stack - 0x3000), 0, 0x4000);
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.fpu_cw = 0x007f;
            c.esp = stack;
            Push32(&c, arg);
            Push32(&c, 0x12345678u);
            fn(&c);
            return c;
        };
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx a = run(f_585c62e0);
        memcpy(ref.data(), native::Ptr<void>(lo), size);
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx b = run(Native_105c62e0);
        bool same_regs = a.esp == b.esp && a.ret_target == b.ret_target && a.ebx == b.ebx && a.esi == b.esi &&
                         a.edi == b.edi && a.ebp == b.ebp;
        if (!same_regs || memcmp(ref.data(), native::Ptr<void>(lo), size) != 0) {
            if (failures < 3) printf("biquad trial %d (count %u) differs%s\n", trial, count, same_regs ? "" : " (registers)");
            ++failures;
        }
    }
    printf("biquad %s: %d/300 trials differ\n", failures ? "FAIL" : "OK", failures);
    int biquad_failures = failures;
    failures = 0;

    // FUN_105eb280: IMA ADPCM blocks with random headers/nibbles into per-channel buffers.
    for (int trial = 0; trial < 200; ++trial) {
        g_next = 0x20000000u;
        const uint32_t channels = 1 + trial % 3, blocks = trial % 5, first = trial % 4, stride = 20 * channels;
        uint32_t stream = Alloc(0x100), data = Alloc(stride * (first + blocks + 1) + 16);
        for (uint32_t i = 0; i < stride * (first + blocks + 1); ++i) W8(data + i, uint8_t(rng()));
        for (uint32_t b = 0; b < first + blocks; ++b)
            for (uint32_t ch = 0; ch < channels; ++ch) W16(data + b * stride + ch * 20, uint16_t(rng() % 89));
        W32(stream + 0x8c, data);
        W32(stream + 0x94, channels);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            uint32_t buf = Alloc(0x80 * (first + blocks + 1) + 16) + (trial % 4);  // aligned down to 4 by the code
            W32(stream + 0xc + 4 * ch, buf);
        }
        const uint32_t lo = 0x20000000u, size = g_next - lo;
        std::vector<uint8_t> start(size), ref(size);
        memcpy(start.data(), native::Ptr<void>(lo), size);
        uint32_t stack = Alloc(0x4000) + 0x3000;
        auto run = [&](void (*fn)(Ctx*)) {
            memset(native::Ptr<void>(stack - 0x3000), 0, 0x4000);
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.esp = stack;
            Push32(&c, blocks);
            Push32(&c, first);
            Push32(&c, stride);
            Push32(&c, stream);
            Push32(&c, 0x12345678u);
            g_alloc_calls = 0;
            fn(&c);
            return c;
        };
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx a = run(f_585eb280);
        uint32_t calls_a = g_alloc_calls;
        memcpy(ref.data(), native::Ptr<void>(lo), size);
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx b = run(Native_105eb280);
        bool same_regs = a.esp == b.esp && a.ret_target == b.ret_target && a.ebx == b.ebx && a.esi == b.esi &&
                         a.edi == b.edi && a.ebp == b.ebp && calls_a == g_alloc_calls;
        if (!same_regs || memcmp(ref.data(), native::Ptr<void>(lo), size) != 0) {
            if (failures < 3) printf("adpcm trial %d differs%s\n", trial, same_regs ? "" : " (registers/allocations)");
            ++failures;
        }
    }
    printf("adpcm %s: %d/200 trials differ\n", failures ? "FAIL" : "OK", failures);
    int adpcm_failures = failures;
    failures = 0;

    // FUN_1071b610: matrix blending with random bones, weights and frames.
    for (int trial = 0; trial < 200; ++trial) {
        g_next = 0x20000000u;
        const uint32_t count = trial % 9, nbones = 16;
        uint32_t self = Alloc(0x300), holder = Alloc(0x40), bones = Alloc(nbones * 0x40 + 16);
        bones = (bones + 15) & ~15u;
        uint32_t records = Alloc(0x50 * count + 16), out = Alloc(0x30 * count + 16) + (trial % 2) * 4;
        records = (records + 15) & ~15u;
        for (uint32_t i = 0; i < nbones * 16; ++i) WF32(bones + 4 * i, uf(rng) * 4.0f);
        for (uint32_t n = 0; n < count; ++n) {
            for (uint32_t i = 0; i < 16; ++i) WF32(records + 0x50 * n + 4 * i, uf(rng) * 3.0f);
            for (uint32_t k = 0; k < 4; ++k) W16(records + 0x50 * n + 0x40 + 2 * k, uint16_t(rng() % nbones));
        }
        W32(self + 0x70, holder);
        W32(holder + 0x38, bones);
        g_skip = trial % 10 == 0 ? 1 : 0;
        const uint32_t lo = 0x20000000u, size = g_next - lo;
        std::vector<uint8_t> start(size), ref(size);
        memcpy(start.data(), native::Ptr<void>(lo), size);
        uint32_t stack = Alloc(0x4000) + 0x3000;
        auto run = [&](void (*fn)(Ctx*)) {
            memset(native::Ptr<void>(stack - 0x3000), 0, 0x4000);
            Ctx c;
            memset(&c, 0, sizeof(c));
            c.esp = stack;
            c.ecx = self;
            Push32(&c, count);
            Push32(&c, records);
            Push32(&c, out);
            Push32(&c, 0x12345678u);
            fn(&c);
            return c;
        };
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx a = run(f_5871b610);
        memcpy(ref.data(), native::Ptr<void>(lo), size);
        memcpy(native::Ptr<void>(lo), start.data(), size);
        Ctx b = run(Native_1071b610);
        bool same_regs = a.esp == b.esp && a.ret_target == b.ret_target && a.ebx == b.ebx && a.esi == b.esi &&
                         a.edi == b.edi && a.ebp == b.ebp;
        if (!same_regs || memcmp(ref.data(), native::Ptr<void>(lo), size) != 0) {
            if (failures < 3) printf("skin trial %d differs%s\n", trial, same_regs ? "" : " (registers)");
            ++failures;
        }
    }
    printf("skin %s: %d/200 trials differ\n", failures ? "FAIL" : "OK", failures);
    return failures || mixer_failures || biquad_failures || adpcm_failures ? 1 : 0;
}
