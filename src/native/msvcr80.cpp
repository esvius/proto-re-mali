// MSVCR80 for the native host: the C runtime functions prototypeenginef.dll imports. Guest pointers are host
// pointers (flat guest memory), so most functions forward to the host C library directly. Variadic functions read
// their arguments from the guest stack with the 32-bit MSVC layout.
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "win.h"

namespace native {

// ---------------- guest printf/scanf ----------------
// Formats `fmt` with arguments starting at guest address `args` (each int/pointer 4 bytes, double/__int64 8).
std::string GuestFormat(const char* fmt, uint32_t args) {
    std::string out;
    char buf[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        if (p[1] == '%') {
            out += '%';
            p += 2;
            continue;
        }
        std::string spec = "%";
        ++p;
        while (strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(int32_t(R32(args)));
            args += 4;
            ++p;
        }
        while (isdigit((unsigned char)*p)) spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string(int32_t(R32(args)));
                args += 4;
                ++p;
            }
            while (isdigit((unsigned char)*p)) spec += *p++;
        }
        int size = 4;  // argument size: 4, 8, or 2 (h)
        bool wide = false;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { size = 8; p += 3; }
        else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
        else if (p[0] == 'l' && p[1] == 'l') { size = 8; p += 2; }
        else if (*p == 'l') { wide = true; ++p; }
        else if (*p == 'h') { ++p; }
        else if (*p == 'L') { ++p; }
        else if (*p == 'w') { wide = true; ++p; }
        char conv = *p ? *p++ : 0;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
                if (size == 8) {
                    spec += "ll";
                    spec += conv;
                    snprintf(buf, sizeof(buf), spec.c_str(), (long long)R64(args));
                    args += 8;
                } else {
                    spec += conv;
                    if (conv == 'd' || conv == 'i') snprintf(buf, sizeof(buf), spec.c_str(), int32_t(R32(args)));
                    else snprintf(buf, sizeof(buf), spec.c_str(), R32(args));
                    args += 4;
                }
                out += buf;
                break;
            case 'c': case 'C':
                spec += 'c';
                snprintf(buf, sizeof(buf), spec.c_str(), int(R32(args) & 0xff));
                args += 4;
                out += buf;
                break;
            case 'p':
                snprintf(buf, sizeof(buf), "%08X", R32(args));
                args += 4;
                out += buf;
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
                spec += conv;
                snprintf(buf, sizeof(buf), spec.c_str(), RF64(args));
                args += 8;
                out += buf;
                break;
            case 's': case 'S': {
                uint32_t s = R32(args);
                args += 4;
                std::string v;
                if (!s) v = "(null)";
                else if (wide || conv == 'S')
                    for (uint32_t k = 0; R16(s + 2 * k); ++k) v += char(R16(s + 2 * k));
                else v = Str(s);
                spec += 's';
                std::vector<char> big(v.size() + 512);
                snprintf(big.data(), big.size(), spec.c_str(), v.c_str());
                out += big.data();
                break;
            }
            case 'n':
                W32(R32(args), uint32_t(out.size()));
                args += 4;
                break;
            default:
                out += spec;
                if (conv) out += conv;
        }
    }
    return out;
}

namespace {

// Scans `str` per `fmt`, storing through guest pointers starting at guest address `args`. `secure` (the _s
// variants) passes a buffer size after each %s/%c/%[ pointer.
int GuestScan(const char* str, const char* fmt, uint32_t args, bool secure) {
    int assigned = 0;
    const char* s = str;
    for (const char* p = fmt; *p;) {
        if (isspace((unsigned char)*p)) {
            while (isspace((unsigned char)*s)) ++s;
            ++p;
            continue;
        }
        if (*p != '%' || p[1] == '%') {
            if (*p == '%') ++p;
            if (*s != *p) break;
            ++s;
            ++p;
            continue;
        }
        const char* start = p++;
        bool suppress = false;
        if (*p == '*') { suppress = true; ++p; }
        while (isdigit((unsigned char)*p)) ++p;
        std::string len;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { len = "ll"; p += 3; }
        while (*p == 'l' || *p == 'h' || *p == 'L') len += *p++;
        std::string spec(start, p);
        char conv = *p;
        if (conv == '[') {
            const char* e = p + 1;
            if (*e == '^') ++e;
            if (*e == ']') ++e;
            while (*e && *e != ']') ++e;
            spec.append(p, e + 1);
            p = *e ? e + 1 : e;
        } else {
            spec += conv;
            ++p;
        }
        if (strchr("sc[", conv) && len == "l") {
            Log("scanf: wide conversion %s not supported", spec.c_str());
            break;
        }
        spec = "%" + spec.substr(1);
        // Host sscanf on one conversion; %n reports how far it read.
        int consumed = -1, r;
        std::string one = spec + "%n";
        if (suppress) {
            r = sscanf(s, one.c_str(), &consumed);
        } else {
            void* target = Ptr<void>(R32(args));
            args += 4;
            if (secure && strchr("sc[", conv)) args += 4;
            if (conv == 'p') {
                unsigned v;
                r = sscanf(s, (spec.substr(0, spec.size() - 1) + "x%n").c_str(), &v, &consumed);
                if (r == 1) *static_cast<uint32_t*>(target) = v;
            } else {
                r = sscanf(s, one.c_str(), target, &consumed);
            }
            if (r == 1) ++assigned;
        }
        if (consumed < 0) break;
        s += consumed;
    }
    return assigned ? assigned : (*str ? 0 : -1);
}

// ---------------- FILE ----------------
// Guest FILE objects are 32-byte blocks; the host FILE* is kept in a side table. __iob_func's array covers
// stdin/stdout/stderr.
std::mutex g_files_mutex;
std::map<uint32_t, FILE*> g_files;
uint32_t g_iob = 0;

uint32_t Iob() {
    std::lock_guard<std::mutex> lock(g_files_mutex);
    if (!g_iob) {
        g_iob = HeapAlloc(3 * 32);
        memset(Ptr<void>(g_iob), 0, 96);
        g_files[g_iob] = stdin;
        g_files[g_iob + 32] = stdout;
        g_files[g_iob + 64] = stderr;
    }
    return g_iob;
}

FILE* HostFile(uint32_t f) {
    Iob();
    std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(f);
    return it == g_files.end() ? nullptr : it->second;
}

uint32_t OpenGuestFile(const char* name, const char* mode) {
    std::string m;
    for (const char* p = mode; *p; ++p)
        if (strchr("rwab+", *p)) m += *p;
    std::string host = HostPath(name);
    FILE* f = fopen(host.c_str(), m.c_str());
    if (!f) return 0;
    uint32_t g = HeapAlloc(32);
    memset(Ptr<void>(g), 0, 32);
    std::lock_guard<std::mutex> lock(g_files_mutex);
    g_files[g] = f;
    return g;
}

// ---------------- misc state ----------------
thread_local uint32_t tl_errno_addr = 0;
uint32_t g_rand_state = 1;
std::mutex g_crt_locks[64];
std::vector<uint32_t> g_onexit;
uint32_t g_tm = 0, g_lconv = 0;
thread_local uint32_t tl_strtok = 0;

int32_t& Errno() {
    if (!tl_errno_addr) tl_errno_addr = HeapAlloc(4);
    return *Ptr<int32_t>(tl_errno_addr);
}

// x87 stack access for the _CI* functions (arguments and result on the FPU stack).
double St(Ctx* c, int i) { return c->st[(c->st_top + i) & 7]; }
void CiUnary(Ctx* c, double (*f)(double)) {
    c->st[c->st_top & 7] = f(St(c, 0));
    c->ret_target = Pop32(c);
}
void CiBinary(Ctx* c, double (*f)(double, double)) {
    double y = St(c, 0), x = St(c, 1);
    c->st_top = (c->st_top + 1) & 7;  // pop
    c->st[c->st_top] = f(x, y);
    c->ret_target = Pop32(c);
}

uint32_t WriteTm(const struct tm& t) {
    if (!g_tm) g_tm = HeapAlloc(36);
    int32_t v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    memcpy(Ptr<void>(g_tm), v, 36);
    return g_tm;
}

thread_local uint32_t tl_qsort_cmp = 0;
int QsortThunk(const void* a, const void* b) {
    return int32_t(GuestCall(tl_qsort_cmp, {Addr(a), Addr(b)}));
}

#define C(name) void m_##name(Ctx* c)

// memory and strings
C(memset) { Return(c, Addr(memset(Ptr<void>(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2)))); }
C(memcpy) { Return(c, Addr(memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2)))); }
C(memmove) { Return(c, Addr(memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 1)), Arg(c, 2)))); }
C(memmove_s) {
    memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 2)), Arg(c, 3) < Arg(c, 1) ? Arg(c, 3) : Arg(c, 1));
    Return(c, 0);
}
C(memcpy_s) {
    memmove(Ptr<void>(Arg(c, 0)), Ptr<void>(Arg(c, 2)), Arg(c, 3) < Arg(c, 1) ? Arg(c, 3) : Arg(c, 1));
    Return(c, 0);
}
C(memchr) { Return(c, Addr(memchr(Ptr<void>(Arg(c, 0)), int(Arg(c, 1)), Arg(c, 2)))); }
C(strchr) { Return(c, Addr(strchr(Str(Arg(c, 0)), int(Arg(c, 1))))); }
C(strrchr) { Return(c, Addr(strrchr(Str(Arg(c, 0)), int(Arg(c, 1))))); }
C(strstr) { Return(c, Addr(strstr(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strpbrk) { Return(c, Addr(strpbrk(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strspn) { Return(c, uint32_t(strspn(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strcspn) { Return(c, uint32_t(strcspn(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(strncmp) { Return(c, uint32_t(strncmp(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strcoll) { Return(c, uint32_t(strcmp(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(_stricmp) { Return(c, uint32_t(strcasecmp(Str(Arg(c, 0)), Str(Arg(c, 1))))); }
C(_strnicmp) { Return(c, uint32_t(strncasecmp(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strncpy) { Return(c, Addr(strncpy(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strncat) { Return(c, Addr(strncat(Str(Arg(c, 0)), Str(Arg(c, 1)), Arg(c, 2)))); }
C(strnlen) { Return(c, uint32_t(strnlen(Str(Arg(c, 0)), Arg(c, 1)))); }
C(strcpy_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1);
    const char* s = Str(Arg(c, 2));
    size_t n = strlen(s);
    if (n >= size) n = size - 1;
    memcpy(d, s, n);
    d[n] = 0;
    Return(c, 0);
}
C(strcat_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1);
    size_t have = strnlen(d, size);
    const char* s = Str(Arg(c, 2));
    size_t n = strlen(s);
    if (have + n >= size) n = size - have - 1;
    memcpy(d + have, s, n);
    d[have + n] = 0;
    Return(c, 0);
}
C(strncpy_s) {
    char* d = Str(Arg(c, 0));
    uint32_t size = Arg(c, 1), count = Arg(c, 3);
    const char* s = Str(Arg(c, 2));
    size_t n = strnlen(s, count == 0xffffffffu ? size : count);
    if (n >= size) n = size - 1;
    memcpy(d, s, n);
    d[n] = 0;
    Return(c, 0);
}
C(strtok) {
    uint32_t s = Arg(c, 0);
    const char* delim = Str(Arg(c, 1));
    if (!s) s = tl_strtok;
    if (!s) {
        Return(c, 0);
        return;
    }
    s += uint32_t(strspn(Str(s), delim));
    if (!R8(s)) {
        tl_strtok = 0;
        Return(c, 0);
        return;
    }
    uint32_t e = s + uint32_t(strcspn(Str(s), delim));
    if (R8(e)) {
        W8(e, 0);
        tl_strtok = e + 1;
    } else {
        tl_strtok = 0;
    }
    Return(c, s);
}
C(_strupr) {
    for (char* p = Str(Arg(c, 0)); *p; ++p) *p = char(toupper((unsigned char)*p));
    Return(c, Arg(c, 0));
}
C(_strlwr) {
    for (char* p = Str(Arg(c, 0)); *p; ++p) *p = char(tolower((unsigned char)*p));
    Return(c, Arg(c, 0));
}
C(strerror) {
    static uint32_t buf = 0;
    if (!buf) buf = HeapAlloc(128);
    snprintf(Str(buf), 128, "%s", strerror(int(Arg(c, 0))));
    Return(c, buf);
}
C(_itoa_s) {
    uint32_t radix = Arg(c, 3);
    int32_t v = int32_t(Arg(c, 0));
    char tmp[40];
    if (radix == 10) snprintf(tmp, sizeof(tmp), "%d", v);
    else if (radix == 16) snprintf(tmp, sizeof(tmp), "%x", uint32_t(v));
    else {
        uint32_t u = uint32_t(v);
        char r[40];
        int n = 0;
        do { r[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[u % radix]; u /= radix; } while (u);
        for (int i = 0; i < n; ++i) tmp[i] = r[n - 1 - i];
        tmp[n] = 0;
    }
    strncpy(Str(Arg(c, 1)), tmp, Arg(c, 2));
    Return(c, 0);
}

// character classes (C locale)
C(toupper) { Return(c, uint32_t(toupper(int(Arg(c, 0))))); }
C(tolower) { Return(c, uint32_t(tolower(int(Arg(c, 0))))); }
C(isspace) { Return(c, isspace(int(Arg(c, 0) & 0xff)) ? 8 : 0); }
C(isdigit) { Return(c, isdigit(int(Arg(c, 0) & 0xff)) ? 4 : 0); }
C(isxdigit) { Return(c, isxdigit(int(Arg(c, 0) & 0xff)) ? 0x80 : 0); }
C(isalpha) { Return(c, isalpha(int(Arg(c, 0) & 0xff)) ? 0x103 : 0); }
C(isalnum) { Return(c, isalnum(int(Arg(c, 0) & 0xff)) ? 0x107 : 0); }
C(islower) { Return(c, islower(int(Arg(c, 0) & 0xff)) ? 2 : 0); }
C(isupper) { Return(c, isupper(int(Arg(c, 0) & 0xff)) ? 1 : 0); }
C(ispunct) { Return(c, ispunct(int(Arg(c, 0) & 0xff)) ? 0x10 : 0); }
C(iscntrl) { Return(c, iscntrl(int(Arg(c, 0) & 0xff)) ? 0x20 : 0); }
C(iswalnum) { Return(c, iswalnum(wint_t(Arg(c, 0) & 0xffff)) ? 1 : 0); }
C(iswspace) { Return(c, iswspace(wint_t(Arg(c, 0) & 0xffff)) ? 1 : 0); }
C(towupper) { Return(c, uint32_t(towupper(wint_t(Arg(c, 0) & 0xffff))) & 0xffff); }
C(towlower) { Return(c, uint32_t(towlower(wint_t(Arg(c, 0) & 0xffff))) & 0xffff); }

// wide strings (16-bit wchar_t)
uint32_t WLen(uint32_t s) {
    uint32_t n = 0;
    while (R16(s + 2 * n)) ++n;
    return n;
}
C(wcscpy_s) {
    uint32_t d = Arg(c, 0), size = Arg(c, 1), s = Arg(c, 2), n = WLen(s);
    if (n >= size) n = size - 1;
    memmove(Ptr<void>(d), Ptr<void>(s), 2 * n);
    W16(d + 2 * n, 0);
    Return(c, 0);
}
C(wcscat_s) {
    uint32_t d = Arg(c, 0), size = Arg(c, 1), s = Arg(c, 2), have = WLen(d), n = WLen(s);
    if (have + n >= size) n = size - have - 1;
    memmove(Ptr<void>(d + 2 * have), Ptr<void>(s), 2 * n);
    W16(d + 2 * (have + n), 0);
    Return(c, 0);
}
C(wcsstr) {
    uint32_t h = Arg(c, 0), n = Arg(c, 1), nl = WLen(n);
    for (uint32_t p = h;; p += 2) {
        if (memcmp(Ptr<void>(p), Ptr<void>(n), 2 * nl) == 0) {
            Return(c, p);
            return;
        }
        if (!R16(p)) break;
    }
    Return(c, 0);
}
C(wcsrtombs_s) {
    uint32_t ret = Arg(c, 0), dst = Arg(c, 1), size = Arg(c, 2), srcp = Arg(c, 3), count = Arg(c, 4);
    uint32_t s = R32(srcp), n = 0;
    while (R16(s + 2 * n) && n < count && (!dst || n + 1 < size)) {
        if (dst) W8(dst + n, uint8_t(R16(s + 2 * n)));
        ++n;
    }
    if (dst) W8(dst + n, 0);
    if (ret) W32(ret, n + 1);
    Return(c, 0);
}
C(swscanf) {
    // Narrow both strings (the engine only scans ASCII content).
    std::string s, f;
    for (uint32_t p = Arg(c, 0); R16(p); p += 2) s += char(R16(p));
    for (uint32_t p = Arg(c, 1); R16(p); p += 2) f += char(R16(p));
    Return(c, uint32_t(GuestScan(s.c_str(), f.c_str(), c->esp + 12, false)));
}

// conversions
C(atoi) { Return(c, uint32_t(atoi(Str(Arg(c, 0))))); }
C(atof) { ReturnF(c, atof(Str(Arg(c, 0)))); }
C(strtod) {
    char* end;
    double v = strtod(Str(Arg(c, 0)), &end);
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    ReturnF(c, v);
}
C(strtol) {
    char* end;
    long v = strtol(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (v > INT32_MAX) v = INT32_MAX;
    if (v < INT32_MIN) v = INT32_MIN;
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    Return(c, uint32_t(int32_t(v)));
}
C(strtoul) {
    char* end;
    unsigned long v = strtoul(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (v > UINT32_MAX) v = UINT32_MAX;
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    Return(c, uint32_t(v));
}
C(_strtoui64) {
    char* end;
    unsigned long long v = strtoull(Str(Arg(c, 0)), &end, int(Arg(c, 2)));
    if (Arg(c, 1)) W32(Arg(c, 1), Addr(end));
    c->edx = uint32_t(v >> 32);
    Return(c, uint32_t(v));
}

// formatted I/O
void PutFormatted(uint32_t dst, uint32_t size, const std::string& s, bool truncate_ok) {
    (void)truncate_ok;
    if (!size) return;
    size_t n = s.size() < size - 1 ? s.size() : size - 1;
    memcpy(Str(dst), s.data(), n);
    Str(dst)[n] = 0;
}
C(sprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    memcpy(Str(Arg(c, 0)), s.c_str(), s.size() + 1);
    Return(c, uint32_t(s.size()));
}
C(sprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 2)), c->esp + 16);
    PutFormatted(Arg(c, 0), Arg(c, 1), s, false);
    Return(c, uint32_t(s.size()));
}
C(_snprintf) {
    std::string s = GuestFormat(Str(Arg(c, 2)), c->esp + 16);
    uint32_t size = Arg(c, 1);
    // MSVC _snprintf: no terminator when the result fills the buffer, -1 when it does not fit.
    memcpy(Str(Arg(c, 0)), s.data(), s.size() < size ? s.size() : size);
    if (s.size() < size) Str(Arg(c, 0))[s.size()] = 0;
    Return(c, s.size() <= size ? uint32_t(s.size()) : 0xffffffffu);
}
C(_snprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 3)), c->esp + 20);
    PutFormatted(Arg(c, 0), Arg(c, 1), s, true);
    Return(c, s.size() < Arg(c, 1) ? uint32_t(s.size()) : 0xffffffffu);
}
C(_vsnprintf) {
    std::string s = GuestFormat(Str(Arg(c, 2)), Arg(c, 3));
    uint32_t size = Arg(c, 1);
    memcpy(Str(Arg(c, 0)), s.data(), s.size() < size ? s.size() : size);
    if (s.size() < size) Str(Arg(c, 0))[s.size()] = 0;
    Return(c, s.size() <= size ? uint32_t(s.size()) : 0xffffffffu);
}
C(_vsnprintf_s) {
    std::string s = GuestFormat(Str(Arg(c, 3)), Arg(c, 4));
    PutFormatted(Arg(c, 0), Arg(c, 1), s, true);
    Return(c, s.size() < Arg(c, 1) ? uint32_t(s.size()) : 0xffffffffu);
}
C(sscanf) { Return(c, uint32_t(GuestScan(Str(Arg(c, 0)), Str(Arg(c, 1)), c->esp + 12, false))); }
C(sscanf_s) { Return(c, uint32_t(GuestScan(Str(Arg(c, 0)), Str(Arg(c, 1)), c->esp + 12, true))); }
C(fprintf) {
    std::string s = GuestFormat(Str(Arg(c, 1)), c->esp + 12);
    FILE* f = HostFile(Arg(c, 0));
    if (f) fwrite(s.data(), 1, s.size(), f);
    Return(c, uint32_t(s.size()));
}
C(_set_printf_count_output) { Return(c, 1); }

// FILE
C(__iob_func) { Return(c, Iob()); }
C(fopen) { Return(c, OpenGuestFile(Str(Arg(c, 0)), Str(Arg(c, 1)))); }
C(fopen_s) {
    uint32_t f = OpenGuestFile(Str(Arg(c, 1)), Str(Arg(c, 2)));
    W32(Arg(c, 0), f);
    Return(c, f ? 0 : 2);
}
C(fclose) {
    FILE* f = HostFile(Arg(c, 0));
    if (f && f != stdin && f != stdout && f != stderr) {
        fclose(f);
        std::lock_guard<std::mutex> lock(g_files_mutex);
        g_files.erase(Arg(c, 0));
    }
    Return(c, 0);
}
C(fread) {
    FILE* f = HostFile(Arg(c, 3));
    Return(c, f ? uint32_t(fread(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), f)) : 0);
}
C(fwrite) {
    FILE* f = HostFile(Arg(c, 3));
    Return(c, f ? uint32_t(fwrite(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), f)) : 0);
}
C(fseek) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(fseek(f, int32_t(Arg(c, 1)), int(Arg(c, 2)))) : 0xffffffffu);
}
C(ftell) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(ftell(f)) : 0xffffffffu);
}
C(rewind) {
    if (FILE* f = HostFile(Arg(c, 0))) rewind(f);
    Return(c, 0);
}
C(fflush) {
    if (FILE* f = HostFile(Arg(c, 0))) fflush(f);
    Return(c, 0);
}
C(feof) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(feof(f)) : 1);
}
C(ferror) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(ferror(f)) : 1);
}
C(fgets) {
    FILE* f = HostFile(Arg(c, 2));
    Return(c, f && fgets(Str(Arg(c, 0)), int(Arg(c, 1)), f) ? Arg(c, 0) : 0);
}
C(getc) {
    FILE* f = HostFile(Arg(c, 0));
    Return(c, f ? uint32_t(getc(f)) : 0xffffffffu);
}
C(ungetc) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(ungetc(int(Arg(c, 0)), f)) : 0xffffffffu);
}
C(fputs) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(fputs(Str(Arg(c, 0)), f)) : 0xffffffffu);
}
C(fputc) {
    FILE* f = HostFile(Arg(c, 1));
    Return(c, f ? uint32_t(fputc(int(Arg(c, 0)), f)) : 0xffffffffu);
}
C(_stat64) {
    struct stat st;
    if (stat(HostPath(Str(Arg(c, 0))).c_str(), &st) != 0) {
        Errno() = 2;
        Return(c, 0xffffffffu);
        return;
    }
    uint32_t b = Arg(c, 1);
    memset(Ptr<void>(b), 0, 56);
    uint16_t mode = (S_ISDIR(st.st_mode) ? 0x4000 : 0x8000) | 0x100 | ((st.st_mode & S_IWUSR) ? 0x80 : 0);
    W16(b + 6, mode);
    W16(b + 8, 1);
    W64(b + 24, uint64_t(st.st_size));
    W64(b + 32, uint64_t(st.st_atime));
    W64(b + 40, uint64_t(st.st_mtime));
    W64(b + 48, uint64_t(st.st_ctime));
    Return(c, 0);
}

// memory
C(malloc) { Return(c, HeapAlloc(Arg(c, 0))); }
C(_malloc_crt) { Return(c, HeapAlloc(Arg(c, 0))); }
C(free) {
    HeapFree(Arg(c, 0));
    Return(c, 0);
}
C(realloc) { Return(c, HeapRealloc(Arg(c, 0), Arg(c, 1))); }

// math (cdecl, double arguments on the stack, result in st0)
C(floor) { ReturnF(c, floor(ArgD(c, 0))); }
C(ceil) { ReturnF(c, ceil(ArgD(c, 0))); }
C(ldexp) { ReturnF(c, ldexp(ArgD(c, 0), int32_t(Arg(c, 2)))); }
C(frexp) {
    int e;
    double m = frexp(ArgD(c, 0), &e);
    W32(Arg(c, 2), uint32_t(e));
    ReturnF(c, m);
}
C(modf) {
    double ip;
    double f = modf(ArgD(c, 0), &ip);
    WF64(Arg(c, 2), ip);
    ReturnF(c, f);
}
C(_isnan) { Return(c, std::isnan(ArgD(c, 0)) ? 1 : 0); }
// _CI*: arguments on the x87 stack
C(_CIsqrt) { CiUnary(c, [](double x) { return std::sqrt(x); }); }
C(_CIcos) { CiUnary(c, [](double x) { return std::cos(x); }); }
C(_CIasin) { CiUnary(c, [](double x) { return std::asin(x); }); }
C(_CIacos) { CiUnary(c, [](double x) { return std::acos(x); }); }
C(_CItanh) { CiUnary(c, [](double x) { return std::tanh(x); }); }
C(_CIsinh) { CiUnary(c, [](double x) { return std::sinh(x); }); }
C(_CIcosh) { CiUnary(c, [](double x) { return std::cosh(x); }); }
C(_CIpow) { CiBinary(c, [](double x, double y) { return std::pow(x, y); }); }
C(_CIfmod) { CiBinary(c, [](double x, double y) { return std::fmod(x, y); }); }

C(rand) {
    g_rand_state = g_rand_state * 214013u + 2531011u;
    Return(c, (g_rand_state >> 16) & 0x7fff);
}
C(srand) {
    g_rand_state = Arg(c, 0);
    Return(c, 0);
}
C(qsort) {
    uint32_t saved = tl_qsort_cmp;
    tl_qsort_cmp = Arg(c, 3);
    qsort(Ptr<void>(Arg(c, 0)), Arg(c, 1), Arg(c, 2), QsortThunk);
    tl_qsort_cmp = saved;
    Return(c, 0);
}

// time
C(_localtime64) {
    time_t t = time_t(R64(Arg(c, 0)));
    struct tm tm;
    localtime_r(&t, &tm);
    Return(c, WriteTm(tm));
}
C(_gmtime64) {
    time_t t = time_t(R64(Arg(c, 0)));
    struct tm tm;
    gmtime_r(&t, &tm);
    Return(c, WriteTm(tm));
}
C(_ftime64) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    uint32_t b = Arg(c, 0);
    W64(b, uint64_t(tv.tv_sec));
    W16(b + 8, uint16_t(tv.tv_usec / 1000));
    W16(b + 10, 0);
    W16(b + 12, 0);
    Return(c, 0);
}

// runtime support
C(_initterm) {
    for (uint32_t p = Arg(c, 0); p < Arg(c, 1); p += 4) {
        if (uint32_t f = R32(p)) GuestCall(f, {});
    }
    Return(c, 0);
}
C(_initterm_e) {
    for (uint32_t p = Arg(c, 0); p < Arg(c, 1); p += 4) {
        if (uint32_t f = R32(p)) {
            if (uint32_t r = GuestCall(f, {})) {
                Return(c, r);
                return;
            }
        }
    }
    Return(c, 0);
}
C(_onexit) {
    g_onexit.push_back(Arg(c, 0));
    Return(c, Arg(c, 0));
}
C(__dllonexit) {
    g_onexit.push_back(Arg(c, 0));
    Return(c, Arg(c, 0));
}
C(_lock) {
    g_crt_locks[Arg(c, 0) & 63].lock();
    Return(c, 0);
}
C(_unlock) {
    g_crt_locks[Arg(c, 0) & 63].unlock();
    Return(c, 0);
}
C(_encode_pointer) { Return(c, Arg(c, 0)); }
C(_decode_pointer) { Return(c, Arg(c, 0)); }
C(_encoded_null) { Return(c, 0); }
C(_crt_debugger_hook) { Return(c, 0); }
C(__clean_type_info_names_internal) { Return(c, 0); }
C(_set_purecall_handler) { Return(c, 0); }
C(_errno) {
    Errno();
    Return(c, tl_errno_addr);
}
C(_controlfp_s) {
    // Precision and rounding go to the guest x87 control word, which the recompiled x87 code honours.
    uint32_t cur = Arg(c, 0), value = Arg(c, 1), mask = Arg(c, 2);
    uint16_t cw = c->fpu_cw;
    if (mask & 0x300) {  // _MCW_RC
        static const uint16_t rc[4] = {0, 1, 2, 3};
        cw = uint16_t((cw & ~0x0c00) | (rc[(value >> 8) & 3] << 10));
    }
    if (mask & 0x30000) {  // _MCW_PC: _PC_64 0, _PC_53 0x10000, _PC_24 0x20000
        static const uint16_t pc[4] = {3, 2, 0, 3};
        cw = uint16_t((cw & ~0x0300) | (pc[(value >> 16) & 3] << 8));
    }
    c->fpu_cw = cw;
    if (cur) {
        static const uint32_t pc_back[4] = {0x20000, 0, 0x10000, 0};
        W32(cur, ((uint32_t(cw >> 10) & 3) << 8) | pc_back[(cw >> 8) & 3] | 0x9001f);
    }
    Return(c, 0);
}
C(localeconv) {
    if (!g_lconv) {
        g_lconv = HeapAlloc(64);
        memset(Ptr<void>(g_lconv), 0, 64);
        uint32_t dot = GuestStrdup("."), empty = GuestStrdup("");
        W32(g_lconv, dot);
        for (int i = 1; i < 10; ++i) W32(g_lconv + 4 * i, empty);
    }
    Return(c, g_lconv);
}
C(exit) {
    Log("exit(%d)", int32_t(Arg(c, 0)));
    _exit(int(Arg(c, 0)));
}
C(_exit) { _exit(int(Arg(c, 0))); }
C(abort) { Fatal("abort() from %08x", R32(c->esp)); }
C(_amsg_exit) { Fatal("_amsg_exit(%u)", Arg(c, 0)); }
C(_purecall) { Fatal("pure virtual call from %08x", R32(c->esp)); }
C(terminate) { Fatal("terminate() from %08x", R32(c->esp)); }
C(_CxxThrowException) {
    Fatal("C++ exception thrown from %08x (object %08x, type %08x): not supported yet", R32(c->esp), Arg(c, 0),
          Arg(c, 1));
}
C(__CppXcptFilter) { Return(c, 0); }
C(_except_handler4_common) { Return(c, 1); }  // ExceptionContinueSearch

// ---- MSVC RTTI ----
// Complete object locator (vtable[-1]): signature, offset, cdOffset, pTypeDescriptor, pClassHierarchyDescriptor.
// Hierarchy: signature, attributes, numBaseClasses, pBaseClassArray. Base class descriptor: pTypeDescriptor,
// numContainedBases, mdisp, pdisp, vdisp, attributes. Type descriptor: vftable, spare, decorated name at +8.
bool SameType(uint32_t a, uint32_t b) { return a == b || strcmp(Str(a + 8), Str(b + 8)) == 0; }
uint32_t CompleteObject(uint32_t obj, uint32_t* col_out) {
    uint32_t col = R32(R32(obj) - 4);
    if (col_out) *col_out = col;
    return obj - R32(col + 4);
}
C(__RTtypeid) {
    uint32_t obj = Arg(c, 0);
    if (!obj) Fatal("typeid of a null pointer from %08x", R32(c->esp));
    Return(c, R32(R32(R32(obj) - 4) + 12));
}
C(__RTDynamicCast) {
    uint32_t obj = Arg(c, 0), target = Arg(c, 3), is_ref = Arg(c, 4);
    if (!obj) {
        Return(c, 0);
        return;
    }
    uint32_t col;
    uint32_t complete = CompleteObject(obj, &col);
    uint32_t chd = R32(col + 16);
    uint32_t n = R32(chd + 8), bases = R32(chd + 12);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t bcd = R32(bases + 4 * i);
        if (!SameType(R32(bcd), target)) continue;
        int32_t mdisp = int32_t(R32(bcd + 8)), pdisp = int32_t(R32(bcd + 12)), vdisp = int32_t(R32(bcd + 16));
        int32_t off = mdisp;
        if (pdisp >= 0) off += pdisp + int32_t(R32(R32(complete + uint32_t(pdisp)) + uint32_t(vdisp)));
        Return(c, complete + uint32_t(off));
        return;
    }
    if (is_ref) Fatal("dynamic_cast<&> failed (bad_cast) from %08x", R32(c->esp));
    Return(c, 0);
}
// type_info members (thiscall: this in ecx)
C(type_info_eq) { Return(c, SameType(c->ecx, Arg(c, 0)) ? 1 : 0, 1); }
C(type_info_before) { Return(c, strcmp(Str(c->ecx + 8), Str(Arg(c, 0) + 8)) < 0 ? 1 : 0, 1); }
C(type_info_name) { Return(c, c->ecx + 8, 1); }
C(type_info_dtor) { Return(c, 0); }

#undef C

#define E(name) {"msvcr80.dll", #name, m_##name}
const ImportDef kMsvcr80[] = {
    E(memset), E(memcpy), E(memmove), E(memmove_s), E(memcpy_s), E(memchr), E(strchr), E(strrchr), E(strstr),
    E(strpbrk), E(strspn), E(strcspn), E(strncmp), E(strcoll), E(_stricmp), E(_strnicmp), E(strncpy), E(strncat),
    E(strnlen), E(strcpy_s), E(strcat_s), E(strncpy_s), E(strtok), E(_strupr), E(_strlwr), E(strerror),
    E(_itoa_s), E(toupper), E(tolower), E(isspace), E(isdigit), E(isxdigit), E(isalpha), E(isalnum), E(islower),
    E(isupper), E(ispunct), E(iscntrl), E(iswalnum), E(iswspace), E(towupper), E(towlower), E(wcscpy_s),
    E(wcscat_s), E(wcsstr), E(wcsrtombs_s), E(swscanf), E(atoi), E(atof), E(strtod), E(strtol), E(strtoul),
    E(_strtoui64), E(sprintf), E(sprintf_s), E(_snprintf), E(_snprintf_s), E(_vsnprintf), E(_vsnprintf_s),
    E(sscanf), E(sscanf_s), E(fprintf), E(_set_printf_count_output), E(__iob_func), E(fopen), E(fopen_s),
    E(fclose), E(fread), E(fwrite), E(fseek), E(ftell), E(rewind), E(fflush), E(feof), E(ferror), E(fgets),
    E(getc), E(ungetc), E(fputs), E(fputc), E(_stat64), E(malloc), E(_malloc_crt), E(free), E(realloc),
    E(floor), E(ceil), E(ldexp), E(frexp), E(modf), E(_isnan), E(_CIsqrt), E(_CIcos), E(_CIasin), E(_CIacos),
    E(_CItanh), E(_CIsinh), E(_CIcosh), E(_CIpow), E(_CIfmod), E(rand), E(srand), E(qsort), E(_localtime64),
    E(_gmtime64), E(_ftime64), E(_initterm), E(_initterm_e), E(_onexit), E(__dllonexit), E(_lock), E(_unlock),
    E(_encode_pointer), E(_decode_pointer), E(_encoded_null), E(_crt_debugger_hook),
    E(__clean_type_info_names_internal), E(_set_purecall_handler), E(_errno), E(_controlfp_s), E(localeconv),
    E(exit), E(_exit), E(abort), E(_amsg_exit), E(_purecall), E(_CxxThrowException), E(__CppXcptFilter),
    E(_except_handler4_common), E(__RTtypeid), E(__RTDynamicCast),
    {"msvcr80.dll", "?terminate@@YAXXZ", m_terminate},
    {"msvcr80.dll", "??8type_info@@QBE_NABV0@@Z", m_type_info_eq},
    {"msvcr80.dll", "?before@type_info@@QBEHABV1@@Z", m_type_info_before},
    {"msvcr80.dll", "?_name_internal_method@type_info@@QBEPBDPAU__type_info_node@@@Z", m_type_info_name},
    {"msvcr80.dll", "?_type_info_dtor_internal_method@type_info@@QAEXXZ", m_type_info_dtor},
};
#undef E
ImportRegistrar g_msvcr80(kMsvcr80);

}  // namespace

}  // namespace native
