/* Floating point conversions with x86 semantics (out-of-range results produce the "integer indefinite" value
 * 0x80000000 as the hardware does, instead of C undefined behaviour). */
#ifndef PROTORE_RECOMP_RUNTIME_FP_H
#define PROTORE_RECOMP_RUNTIME_FP_H

#include <math.h>
#include <stdint.h>

#include "runtime.h"

static inline int32_t RecompTruncF32(float v) {
    if (!(v > -2147483649.0f && v < 2147483648.0f)) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* x87 store with the default round-to-nearest-even control word. */
static inline int32_t RecompRoundToInt(double v) {
    double r = nearbyint(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) {
        return INT32_MIN;
    }
    return (int32_t)r;
}

static inline int64_t RecompRoundToInt64(double v) {
    double r = nearbyint(v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) {
        return INT64_MIN;
    }
    return (int64_t)r;
}

static inline int64_t RecompTruncF64(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* ---- x87 model ---- */
#define X87(c, i) ((c)->st[((c)->st_top + (i)) & 7])
/* Localized x87 stack (recomp.py): a function's own copy r_st/r_top, see RLOCALS. */
#define X87L(i) (r_st[(r_top + (i)) & 7])
static inline void X87PushL(double* st, uint32_t* top, double v) { *top = (*top - 1) & 7; st[*top] = v; }
static inline void X87PopL(uint32_t* top) { *top = (*top + 1) & 7; }

static inline void X87Push(Ctx* c, double v) {
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
}
static inline void X87Pop(Ctx* c) { c->st_top = (c->st_top + 1) & 7; }

static inline void X87Init(Ctx* c) {
    c->st_top = 0;
    c->fpu_sw = 0;
    c->fpu_cw = 0x037f;
}

static inline uint16_t X87StatusWord(const Ctx* c) {
    return (uint16_t)((c->fpu_sw & ~0x3800u) | ((c->st_top & 7) << 11));
}
static inline uint16_t X87StatusWordL(const Ctx* c, uint32_t top) {
    return (uint16_t)((c->fpu_sw & ~0x3800u) | ((top & 7) << 11));
}

/* Precision control (control word bits 8-9): 00 single, 10 double, 11 extended (modelled as double). */
static inline double X87Precision(const Ctx* c, double v) {
    return ((c->fpu_cw >> 8) & 3) == 0 ? (double)(float)v : v;
}
/* Localized form: the function's copy of the control word (r_cw, see recomp.py). */
static inline double X87PrecisionCW(uint16_t cw, double v) { return ((cw >> 8) & 3) == 0 ? (double)(float)v : v; }

/* Rounding control (bits 10-11): nearest-even, down, up, toward zero. */
static inline double X87RoundRC(const Ctx* c, double v) {
    switch ((c->fpu_cw >> 10) & 3) {
        case 1: return floor(v);
        case 2: return ceil(v);
        case 3: return trunc(v);
        default: return nearbyint(v);
    }
}

static inline int16_t X87ToInt16(const Ctx* c, double v) {
    double r = X87RoundRC(c, v);
    return (r >= -32768.0 && r <= 32767.0) ? (int16_t)r : INT16_MIN;
}
static inline int32_t X87ToInt32(const Ctx* c, double v) {
    double r = X87RoundRC(c, v);
    return (r >= -2147483648.0 && r <= 2147483647.0) ? (int32_t)r : INT32_MIN;
}
static inline int64_t X87ToInt64(const Ctx* c, double v) {
    double r = X87RoundRC(c, v);
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (int64_t)r : INT64_MIN;
}

/* fcom family: C3 C2 C0 = 000 greater, 001 less, 100 equal, 111 unordered. */
static inline void X87CompareStatus(Ctx* c, double a, double b) {
    uint16_t cc;
    if (a != a || b != b) cc = 0x4500;
    else if (a < b) cc = 0x0100;
    else if (a == b) cc = 0x4000;
    else cc = 0;
    c->fpu_sw = (uint16_t)((c->fpu_sw & ~0x4700u) | cc);
}

/* fcomi family: ZF PF CF = 000 greater, 001 less, 100 equal, 111 unordered. */
static inline void X87CompareFlags(Ctx* c, double a, double b) {
    int un = a != a || b != b;
    c->zf = un || a == b;
    c->pf = un;
    c->cf = un || a < b;
    c->of = c->sf = c->af = 0;
}
static inline void X87CompareFlagsL(RFlags* c, double a, double b) {
    int un = a != a || b != b;
    c->zf = un || a == b;
    c->pf = un;
    c->cf = un || a < b;
    c->of = c->sf = c->af = 0;
}

/* 80-bit extended values, converted to/from double. */
static inline double RecompLoadF80(uint32_t a) {
    uint64_t mantissa = R64(a);
    uint16_t se = R16(a + 8);
    int sign = se >> 15;
    int exponent = se & 0x7fff;
    double v;
    if (exponent == 0 && mantissa == 0) v = 0.0;
    else if (exponent == 0x7fff) v = (mantissa << 1) ? NAN : INFINITY;
    else v = ldexp((double)mantissa, exponent - 16383 - 63);
    return sign ? -v : v;
}
static inline void RecompStoreF80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mantissa = 0;
    double m = fabs(v);
    if (m != m) { se |= 0x7fff; mantissa = 0xc000000000000000ull; }
    else if (isinf(m)) { se |= 0x7fff; mantissa = 0x8000000000000000ull; }
    else if (m != 0.0) {
        int e;
        double f = frexp(m, &e); /* m = f * 2^e, f in [0.5, 1) */
        se |= (uint16_t)(e - 1 + 16383);
        mantissa = (uint64_t)ldexp(f, 64);
    }
    W64(a, mantissa);
    W16(a + 8, se);
}

#endif
