// msvcrt.cpp - MSVCRT.dll for programs linked against the shared Visual C++ 6
// runtime: process start-up and exit, the C library subset such programs call
// (strings, formatting, stdio, time, sorting, x87 helpers) and the compiler
// support the generated code needs from it - MSVC C++ exceptions
// (__CxxFrameHandler, _CxxThrowException), SEH scope tables
// (_except_handler3) and RTTI casts (__RTDynamicCast).
//
// Guest data structures keep their Visual C++ 6 32-bit layouts: FILE (32
// bytes, fields read by the stdio macros), struct tm (nine ints), struct
// _stat (36 bytes, 32-bit times) and the compiler's exception tables. Host
// state lives in tables keyed by guest addresses, never in guest memory.
#include "imports.h"
#include "memory.h"
#include "seh.h"
#include "win32.h"
#include "../platform/os.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <map>
#include <string>
#include <vector>

namespace {

char *gp(uint32_t a) {
    return (char *)gm_ptr(a);
}

[[noreturn]] void crt_fatal(const char *what) {
    LOGW("MSVCRT: %s", what);
    fflush(stdout);
    fflush(stderr);
    abort();
}

// Calls another registered shim as the guest would: arguments and a return
// address pushed on the guest stack, then the dispatcher pops them.
uint32_t call_import(X86 *c, const char *dll, const char *name, const uint32_t *args, int n) {
    uint32_t tramp = imports_resolve(dll, name);
    if (!tramp) {
        LOGW("MSVCRT: %s!%s is not available", dll, name);
        return 0;
    }
    uint32_t esp = c->r[R_ESP], eip = c->eip, sp = esp;
    for (int i = n - 1; i >= 0; --i) {
        sp -= 4;
        wr32(sp, args[i]);
    }
    sp -= 4;
    wr32(sp, eip);
    c->r[R_ESP] = sp;
    imports_dispatch(c, tramp);
    c->r[R_ESP] = esp;
    c->eip = eip;
    return c->r[R_EAX];
}

// Small per-process guest variables handed out by address.
struct CrtGlobals {
    uint32_t block = 0; // fmode, commode, errno, tm buffer, argv storage
    uint32_t fmode() {
        ensure();
        return block;
    }
    uint32_t commode() {
        ensure();
        return block + 4;
    }
    uint32_t err() {
        ensure();
        return block + 8;
    }
    uint32_t tm() {
        ensure();
        return block + 16;
    }
    void ensure() {
        if (!block || !heap_owns(block))
            block = heap_alloc(64, true);
    }
};
CrtGlobals g;

void set_errno(int e) {
    wr32(g.err(), (uint32_t)e);
}

// ---------------------------------------------------------------------------
// Start-up and exit
// ---------------------------------------------------------------------------
std::vector<uint32_t> g_onexit;
uint32_t g_control_word = 0x0009001f; // _PC_53 | _RC_NEAR | every exception masked

uint32_t command_line(X86 *c) {
    return call_import(c, "KERNEL32.dll", "GetCommandLineA", nullptr, 0);
}

void publish_acmdln(X86 *c) {
    uint32_t slot = imports_data_address("MSVCRT.dll", "_acmdln");
    if (slot && !rd32(slot))
        wr32(slot, command_line(c));
}

void crt_set_app_type(X86 *c) {
    publish_acmdln(c);
}

// Windows' argv rules: whitespace separates, quotes group, backslashes before
// a quote escape it.
std::vector<std::string> split_command_line(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
            ++i;
        if (i >= s.size())
            break;
        std::string arg;
        bool quoted = false;
        while (i < s.size() && (quoted || (s[i] != ' ' && s[i] != '\t'))) {
            size_t slashes = 0;
            while (i < s.size() && s[i] == '\\') {
                ++slashes;
                ++i;
            }
            if (i < s.size() && s[i] == '"') {
                arg.append(slashes / 2, '\\');
                if (slashes % 2) {
                    arg += '"';
                } else {
                    quoted = !quoted;
                }
                ++i;
            } else {
                arg.append(slashes, '\\');
                if (i < s.size() && (quoted || (s[i] != ' ' && s[i] != '\t')))
                    arg += s[i++];
            }
        }
        out.push_back(arg);
    }
    return out;
}

void crt_getmainargs(X86 *c) {
    publish_acmdln(c);
    uint32_t pargc = arg(c, 0), pargv = arg(c, 1), penv = arg(c, 2);
    std::vector<std::string> args = split_command_line(gm_str(command_line(c)));
    uint32_t bytes = 4 * ((uint32_t)args.size() + 2);
    for (const std::string &a : args)
        bytes += (uint32_t)a.size() + 1;
    uint32_t block = heap_alloc(bytes, true);
    uint32_t text = block + 4 * ((uint32_t)args.size() + 2);
    for (size_t i = 0; i < args.size(); ++i) {
        wr32(block + 4 * (uint32_t)i, text);
        text += gm_put_str(text, args[i].c_str(), (uint32_t)args[i].size() + 1) + 1;
    }
    // argv[argc] = 0, then an empty environment array.
    if (pargc)
        wr32(pargc, (uint32_t)args.size());
    if (pargv)
        wr32(pargv, block);
    if (penv)
        wr32(penv, block + 4 * ((uint32_t)args.size() + 1));
    set_eax(c, 0);
}

void crt_initterm(X86 *c) {
    uint32_t begin = arg(c, 0), end = arg(c, 1);
    for (uint32_t p = begin; p < end; p += 4) {
        uint32_t fn = rd32(p);
        if (fn)
            guest_call(c, fn);
    }
    set_eax(c, 0);
}

void crt_onexit(X86 *c) {
    g_onexit.push_back(arg(c, 0));
    set_eax(c, arg(c, 0));
}

void crt_dllonexit(X86 *c) {
    g_onexit.push_back(arg(c, 0));
    set_eax(c, arg(c, 0));
}

void crt_exit(X86 *c) {
    uint32_t code = arg(c, 0);
    while (!g_onexit.empty()) {
        uint32_t fn = g_onexit.back();
        g_onexit.pop_back();
        guest_call(c, fn);
    }
    call_import(c, "KERNEL32.dll", "ExitProcess", &code, 1);
}

void crt__exit(X86 *c) {
    uint32_t code = arg(c, 0);
    call_import(c, "KERNEL32.dll", "ExitProcess", &code, 1);
}

void crt_xcptfilter(X86 *c) {
    set_eax(c, 0); // EXCEPTION_CONTINUE_SEARCH
}

void crt_controlfp(X86 *c) {
    uint32_t value = arg(c, 0), mask = arg(c, 1);
    g_control_word = (g_control_word & ~mask) | (value & mask);
    set_eax(c, g_control_word);
}

void crt_p_fmode(X86 *c) {
    set_eax(c, g.fmode());
}
void crt_p_commode(X86 *c) {
    set_eax(c, g.commode());
}
void crt_errno(X86 *c) {
    set_eax(c, g.err());
}
void crt_zero(X86 *c) {
    set_eax(c, 0);
}
void crt_purecall(X86 *) {
    crt_fatal("pure virtual function call");
}
void crt_terminate(X86 *) {
    crt_fatal("terminate() called");
}
void crt_getdrive(X86 *c) {
    set_eax(c, 3); // C:
}

void crt_beginthread(X86 *c) {
    uint32_t args[6] = {0, arg(c, 1), arg(c, 0), arg(c, 2), 0, 0};
    uint32_t h = call_import(c, "KERNEL32.dll", "CreateThread", args, 6);
    set_eax(c, h ? h : 0xffffffffu);
}

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------
void crt_malloc(X86 *c) {
    uint32_t size = arg(c, 0);
    uint32_t p = heap_alloc(size ? size : 1);
    if (!p)
        set_errno(12);
    set_eax(c, p);
}
void crt_calloc(X86 *c) {
    uint64_t size = (uint64_t)arg(c, 0) * arg(c, 1);
    uint32_t p = size > 0x7fffffffu ? 0 : heap_alloc(size ? (uint32_t)size : 1, true);
    if (!p)
        set_errno(12);
    set_eax(c, p);
}
void crt_free(X86 *c) {
    if (arg(c, 0))
        heap_free(arg(c, 0));
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Strings and characters (the "C" locale)
// ---------------------------------------------------------------------------
int lower_c(int ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
}
int upper_c(int ch) {
    return ch >= 'a' && ch <= 'z' ? ch - 32 : ch;
}
int sign(int v) {
    return v < 0 ? -1 : v > 0 ? 1 : 0;
}

void crt_stricmp(X86 *c) {
    const unsigned char *a = (const unsigned char *)gp(arg(c, 0));
    const unsigned char *b = (const unsigned char *)gp(arg(c, 1));
    int d = 0;
    for (;; ++a, ++b) {
        d = lower_c(*a) - lower_c(*b);
        if (d || !*a)
            break;
    }
    set_eax(c, (uint32_t)sign(d));
}
void crt_strnicmp(X86 *c) {
    const unsigned char *a = (const unsigned char *)gp(arg(c, 0));
    const unsigned char *b = (const unsigned char *)gp(arg(c, 1));
    uint32_t n = arg(c, 2);
    int d = 0;
    for (; n; --n, ++a, ++b) {
        d = lower_c(*a) - lower_c(*b);
        if (d || !*a)
            break;
    }
    set_eax(c, (uint32_t)sign(d));
}
void crt_strncmp(X86 *c) {
    set_eax(c, (uint32_t)sign(strncmp(gp(arg(c, 0)), gp(arg(c, 1)), arg(c, 2))));
}
void crt_strchr(X86 *c) {
    uint32_t s = arg(c, 0);
    const char *r = strchr(gp(s), (char)arg(c, 1));
    set_eax(c, r ? s + (uint32_t)(r - gp(s)) : 0);
}
void crt_strrchr(X86 *c) {
    uint32_t s = arg(c, 0);
    const char *r = strrchr(gp(s), (char)arg(c, 1));
    set_eax(c, r ? s + (uint32_t)(r - gp(s)) : 0);
}
void crt_strstr(X86 *c) {
    uint32_t s = arg(c, 0);
    const char *r = strstr(gp(s), gp(arg(c, 1)));
    set_eax(c, r ? s + (uint32_t)(r - gp(s)) : 0);
}
void crt_strspn(X86 *c) {
    set_eax(c, (uint32_t)strspn(gp(arg(c, 0)), gp(arg(c, 1))));
}
void crt_strcspn(X86 *c) {
    set_eax(c, (uint32_t)strcspn(gp(arg(c, 0)), gp(arg(c, 1))));
}
void crt_strncpy(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
    uint32_t i = 0;
    for (; i < n && rd8(s + i); ++i)
        wr8(d + i, rd8(s + i));
    for (; i < n; ++i)
        wr8(d + i, 0);
    set_eax(c, d);
}
void crt_memmove(X86 *c) {
    uint32_t d = arg(c, 0), s = arg(c, 1), n = arg(c, 2);
    if (n && gm_valid(d, n) && gm_valid(s, n))
        memmove(gp(d), gp(s), n);
    set_eax(c, d);
}
void crt_strlwr(X86 *c) {
    for (char *p = gp(arg(c, 0)); *p; ++p)
        *p = (char)lower_c((unsigned char)*p);
    set_eax(c, arg(c, 0));
}
void crt_strrev(X86 *c) {
    char *p = gp(arg(c, 0));
    size_t n = strlen(p);
    for (size_t i = 0; i < n / 2; ++i) {
        char t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
    set_eax(c, arg(c, 0));
}
void crt_itoa(X86 *c) {
    int32_t value = (int32_t)arg(c, 0);
    uint32_t out = arg(c, 1), radix = arg(c, 2);
    if (radix < 2 || radix > 36)
        radix = 10;
    char buf[40];
    char *e = buf + sizeof buf;
    *--e = 0;
    bool neg = radix == 10 && value < 0;
    uint32_t u = neg ? 0u - (uint32_t)value : (uint32_t)value;
    do {
        uint32_t d = u % radix;
        *--e = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        u /= radix;
    } while (u);
    if (neg)
        *--e = '-';
    gm_put_str(out, e, (uint32_t)(buf + sizeof buf - e));
    set_eax(c, out);
}
void crt_atoi(X86 *c) {
    set_eax(c, (uint32_t)(int32_t)strtol(gp(arg(c, 0)), nullptr, 10));
}
void crt_tolower(X86 *c) {
    set_eax(c, (uint32_t)lower_c((int)arg(c, 0)));
}
void crt_toupper(X86 *c) {
    set_eax(c, (uint32_t)upper_c((int)arg(c, 0)));
}
bool ascii(int ch) {
    return ch >= 0 && ch < 128;
}
void crt_isdigit(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, ascii(ch) && isdigit(ch) ? 4 : 0);
}
void crt_isspace(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, ascii(ch) && isspace(ch) ? 8 : 0);
}
void crt_isalnum(X86 *c) {
    int ch = (int)arg(c, 0);
    set_eax(c, ascii(ch) && isalnum(ch) ? 0x107 : 0);
}
void crt_getenv(X86 *c) {
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// printf / scanf over guest varargs
// ---------------------------------------------------------------------------
struct VaReader {
    uint32_t ap;
    uint32_t next32() {
        uint32_t v = rd32(ap);
        ap += 4;
        return v;
    }
    uint64_t next64() {
        uint64_t lo = next32();
        return lo | (uint64_t)next32() << 32;
    }
    double nextd() {
        uint64_t b = next64();
        double d;
        memcpy(&d, &b, 8);
        return d;
    }
};

template <typename T> std::string fmt1(const std::string &spec, T v) {
    char buf[512];
    int n = snprintf(buf, sizeof buf, spec.c_str(), v);
    if (n < (int)sizeof buf)
        return std::string(buf, n > 0 ? (size_t)n : 0);
    std::string big((size_t)n + 1, '\0');
    snprintf(&big[0], big.size(), spec.c_str(), v);
    big.resize((size_t)n);
    return big;
}

// Visual C++ prints three exponent digits ("1.5e+000").
std::string msvc_exponent(std::string s) {
    for (size_t i = 0; i + 3 < s.size() + 1; ++i) {
        if ((s[i] == 'e' || s[i] == 'E') && i + 1 < s.size() &&
            (s[i + 1] == '+' || s[i + 1] == '-')) {
            size_t d = i + 2, e = d;
            while (e < s.size() && isdigit((unsigned char)s[e]))
                ++e;
            if (e - d == 2) {
                s.insert(d, 1, '0');
                if (!s.empty() && s[0] == ' ')
                    s.erase(0, 1);
            }
            break;
        }
    }
    return s;
}

std::string gformat(const char *p, VaReader &va) {
    std::string out;
    while (*p) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char *start = p++;
        if (*p == '%') {
            out += '%';
            ++p;
            continue;
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0", *p))
            spec += *p++;
        if (*p == '*') {
            spec += std::to_string((int32_t)va.next32());
            ++p;
        } else {
            while (isdigit((unsigned char)*p))
                spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string((int32_t)va.next32());
                ++p;
            } else {
                while (isdigit((unsigned char)*p))
                    spec += *p++;
            }
        }
        int len = 0; // 1 h, 2 l, 3 64-bit, 4 L
        if (*p == 'h') {
            len = 1;
            ++p;
        } else if (*p == 'l') {
            len = 2;
            ++p;
            if (*p == 'l') {
                len = 3;
                ++p;
            }
        } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
            len = 3;
            p += 3;
        } else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') {
            p += 3;
        } else if (*p == 'L') {
            len = 4;
            ++p;
        }
        char conv = *p ? *p++ : 0;
        switch (conv) {
        case 'd':
        case 'i': {
            long long v = len == 3   ? (long long)(int64_t)va.next64()
                          : len == 1 ? (long long)(int16_t)va.next32()
                                     : (long long)(int32_t)va.next32();
            out += fmt1(spec + "lld", v);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v = len == 3   ? (unsigned long long)va.next64()
                                   : len == 1 ? (unsigned long long)(uint16_t)va.next32()
                                              : (unsigned long long)va.next32();
            out += fmt1(spec + "ll" + conv, v);
            break;
        }
        case 'c':
        case 'C': {
            uint32_t v = va.next32();
            out += fmt1(spec + "c", (int)(v & 0xff));
            break;
        }
        case 's':
        case 'S': {
            uint32_t s = va.next32();
            bool wide = conv == 'S' ? len != 1 : len == 2;
            std::string text = !s ? "(null)" : wide ? gm_wstr(s) : gm_str(s);
            out += fmt1(spec + "s", text.c_str());
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            double v = va.nextd();
            std::string s = fmt1(spec + conv, v);
            out += conv == 'f' ? s : msvc_exponent(s);
            break;
        }
        case 'p':
            out += fmt1(std::string("%08X"), va.next32());
            break;
        case 'n':
            wr32(va.next32(), (uint32_t)out.size());
            break;
        default:
            out.append(start, p);
            break;
        }
    }
    return out;
}

void crt_sprintf(X86 *c) {
    VaReader va{c->r[R_ESP] + 12};
    std::string s = gformat(gp(arg(c, 1)), va);
    gm_put_str(arg(c, 0), s.c_str(), (uint32_t)s.size() + 1);
    set_eax(c, (uint32_t)s.size());
}
void crt_vsprintf(X86 *c) {
    VaReader va{arg(c, 2)};
    std::string s = gformat(gp(arg(c, 1)), va);
    gm_put_str(arg(c, 0), s.c_str(), (uint32_t)s.size() + 1);
    set_eax(c, (uint32_t)s.size());
}

int gscan(const char *s, const char *f, VaReader &va) {
    const char *begin = s;
    int assigned = 0;
    bool converted = false;
    while (*f) {
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*s))
                ++s;
            ++f;
            continue;
        }
        if (*f != '%') {
            if (*s != *f)
                break;
            ++s;
            ++f;
            continue;
        }
        ++f;
        if (*f == '%') {
            while (isspace((unsigned char)*s))
                ++s;
            if (*s != '%')
                break;
            ++s;
            ++f;
            continue;
        }
        bool suppress = false;
        if (*f == '*') {
            suppress = true;
            ++f;
        }
        int width = 0;
        while (isdigit((unsigned char)*f))
            width = width * 10 + (*f++ - '0');
        int len = 0;
        if (*f == 'h') {
            len = 1;
            ++f;
        } else if (*f == 'l') {
            len = 2;
            ++f;
            if (*f == 'l') {
                len = 3;
                ++f;
            }
        } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
            len = 3;
            f += 3;
        } else if (*f == 'L') {
            len = 2;
            ++f;
        }
        char conv = *f ? *f++ : 0;
        if (conv == 'n') {
            if (!suppress)
                wr32(va.next32(), (uint32_t)(s - begin));
            continue;
        }
        if (conv != 'c' && conv != '[')
            while (isspace((unsigned char)*s))
                ++s;
        if (!*s)
            return converted || assigned ? assigned : -1;
        size_t limit = width > 0 ? (size_t)width : (size_t)-1;
        converted = true;
        switch (conv) {
        case 'd':
        case 'i':
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            char buf[64];
            size_t n = 0;
            while (s[n] && n < limit && n < sizeof buf - 1 && !isspace((unsigned char)s[n]))
                buf[n] = s[n], ++n;
            buf[n] = 0;
            int base = conv == 'i' ? 0 : (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : 10;
            char *end = nullptr;
            long long v = (conv == 'd' || conv == 'i') ? strtoll(buf, &end, base)
                                                       : (long long)strtoull(buf, &end, base);
            if (end == buf)
                return assigned;
            s += end - buf;
            if (!suppress) {
                uint32_t dst = va.next32();
                if (len == 1)
                    wr16(dst, (uint16_t)v);
                else if (len == 3) {
                    wr32(dst, (uint32_t)v);
                    wr32(dst + 4, (uint32_t)((uint64_t)v >> 32));
                } else
                    wr32(dst, (uint32_t)v);
                ++assigned;
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            char buf[128];
            size_t n = 0;
            while (s[n] && n < limit && n < sizeof buf - 1 && !isspace((unsigned char)s[n]))
                buf[n] = s[n], ++n;
            buf[n] = 0;
            char *end = nullptr;
            double v = strtod(buf, &end);
            if (end == buf)
                return assigned;
            s += end - buf;
            if (!suppress) {
                uint32_t dst = va.next32();
                if (len >= 2) {
                    uint64_t b;
                    memcpy(&b, &v, 8);
                    wr32(dst, (uint32_t)b);
                    wr32(dst + 4, (uint32_t)(b >> 32));
                } else {
                    float fv = (float)v;
                    uint32_t b;
                    memcpy(&b, &fv, 4);
                    wr32(dst, b);
                }
                ++assigned;
            }
            break;
        }
        case 's': {
            uint32_t dst = suppress ? 0 : va.next32();
            size_t n = 0;
            while (s[n] && n < limit && !isspace((unsigned char)s[n])) {
                if (dst)
                    wr8(dst + (uint32_t)n, (uint8_t)s[n]);
                ++n;
            }
            if (dst) {
                wr8(dst + (uint32_t)n, 0);
                ++assigned;
            }
            s += n;
            break;
        }
        case 'c': {
            uint32_t dst = suppress ? 0 : va.next32();
            size_t want = width > 0 ? (size_t)width : 1, n = 0;
            for (; n < want && s[n]; ++n)
                if (dst)
                    wr8(dst + (uint32_t)n, (uint8_t)s[n]);
            if (n < want)
                return assigned;
            s += n;
            if (dst)
                ++assigned;
            break;
        }
        case '[': {
            bool set[256] = {};
            bool negate = false;
            if (*f == '^') {
                negate = true;
                ++f;
            }
            if (*f == ']')
                set[(unsigned char)*f++] = true;
            while (*f && *f != ']') {
                if (f[1] == '-' && f[2] && f[2] != ']') {
                    for (int ch = (unsigned char)f[0]; ch <= (unsigned char)f[2]; ++ch)
                        set[ch] = true;
                    f += 3;
                } else {
                    set[(unsigned char)*f++] = true;
                }
            }
            if (*f == ']')
                ++f;
            uint32_t dst = suppress ? 0 : va.next32();
            size_t n = 0;
            while (s[n] && n < limit && set[(unsigned char)s[n]] != negate) {
                if (dst)
                    wr8(dst + (uint32_t)n, (uint8_t)s[n]);
                ++n;
            }
            if (!n)
                return assigned;
            if (dst) {
                wr8(dst + (uint32_t)n, 0);
                ++assigned;
            }
            s += n;
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

void crt_sscanf(X86 *c) {
    VaReader va{c->r[R_ESP] + 12};
    std::string input = gm_str(arg(c, 0));
    set_eax(c, (uint32_t)gscan(input.c_str(), gp(arg(c, 1)), va));
}

// ---------------------------------------------------------------------------
// stdio. A guest FILE is the 32-byte Visual C++ structure; _cnt stays 0 so the
// getc/putc macros always call into the runtime, and _flag carries the EOF and
// error bits feof()/ferror() read inline. Text mode drops the CR of a CRLF
// pair and stops at Ctrl-Z on input, and writes CRLF for LF on output.
// ---------------------------------------------------------------------------
constexpr uint32_t IOREAD = 1, IOWRT = 2, IOEOF = 0x10, IOERR = 0x20, IORW = 0x80;
struct GFile {
    FILE *f;
    bool text;
    uint32_t fd;
};
std::map<uint32_t, GFile> g_files;
uint32_t g_next_fd = 3;

GFile *gfile(uint32_t fp) {
    auto it = g_files.find(fp);
    return it == g_files.end() ? nullptr : &it->second;
}
void set_flag(uint32_t fp, uint32_t bits, bool on) {
    uint32_t v = rd32(fp + 12);
    wr32(fp + 12, on ? v | bits : v & ~bits);
}

void crt_fopen(X86 *c) {
    std::string name = gm_str(arg(c, 0)), mode = gm_str(arg(c, 1));
    bool plus = mode.find('+') != std::string::npos;
    char kind = mode.empty() ? 'r' : mode[0];
    bool text = mode.find('b') == std::string::npos && rd32(g.fmode()) != 0x8000;
    bool writes = kind == 'w' || kind == 'a' || plus;
    std::string host = win32_host_path(name, writes);
    std::string hmode(1, kind);
    if (plus)
        hmode += '+';
    hmode += 'b';
    FILE *f = host.empty() ? nullptr : fopen(host.c_str(), hmode.c_str());
    if (!f) {
        LOGV("fopen(%s, %s) -> %s: failed", name.c_str(), mode.c_str(), host.c_str());
        set_errno(2);
        set_eax(c, 0);
        return;
    }
    uint32_t fp = heap_alloc(32, true);
    uint32_t fd = g_next_fd++;
    g_files[fp] = GFile{f, text, fd};
    wr32(fp + 12, plus ? IORW : kind == 'r' ? IOREAD : IOWRT);
    wr32(fp + 16, fd);
    LOGV("fopen(%s, %s) -> %08x", name.c_str(), mode.c_str(), fp);
    set_eax(c, fp);
}

void crt_fclose(X86 *c) {
    uint32_t fp = arg(c, 0);
    GFile *gf = gfile(fp);
    if (!gf) {
        set_eax(c, 0xffffffffu);
        return;
    }
    int r = fclose(gf->f);
    g_files.erase(fp);
    heap_free(fp);
    set_eax(c, r ? 0xffffffffu : 0);
}

int read_char(uint32_t fp, GFile *gf) {
    int ch = fgetc(gf->f);
    if (gf->text) {
        if (ch == '\r') {
            int next = fgetc(gf->f);
            if (next == '\n')
                ch = '\n';
            else if (next != EOF)
                ungetc(next, gf->f);
        } else if (ch == 0x1a) {
            fseek(gf->f, -1, SEEK_CUR);
            ch = EOF;
        }
    }
    if (ch == EOF)
        set_flag(fp, IOEOF, true);
    return ch;
}

void crt_fgetc(X86 *c) {
    uint32_t fp = arg(c, 0);
    GFile *gf = gfile(fp);
    int ch = gf ? read_char(fp, gf) : EOF;
    set_eax(c, ch == EOF ? 0xffffffffu : (uint32_t)(ch & 0xff));
}

void crt_fgets(X86 *c) {
    uint32_t buf = arg(c, 0), fp = arg(c, 2);
    int32_t n = (int32_t)arg(c, 1);
    GFile *gf = gfile(fp);
    if (!gf || n <= 0) {
        set_eax(c, 0);
        return;
    }
    int32_t i = 0;
    while (i < n - 1) {
        int ch = read_char(fp, gf);
        if (ch == EOF)
            break;
        wr8(buf + (uint32_t)i++, (uint8_t)ch);
        if (ch == '\n')
            break;
    }
    wr8(buf + (uint32_t)i, 0);
    set_eax(c, i ? buf : 0);
}

void crt_fread(X86 *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2), fp = arg(c, 3);
    GFile *gf = gfile(fp);
    uint64_t total = (uint64_t)size * count;
    if (!gf || !total || !gm_valid(buf, (uint32_t)total)) {
        set_eax(c, 0);
        return;
    }
    size_t got = 0;
    if (!gf->text) {
        got = fread(gp(buf), 1, (size_t)total, gf->f);
        if (got < total)
            set_flag(fp, feof(gf->f) ? IOEOF : IOERR, true);
    } else {
        while (got < total) {
            int ch = read_char(fp, gf);
            if (ch == EOF)
                break;
            gp(buf)[got++] = (char)ch;
        }
    }
    set_eax(c, (uint32_t)(got / size));
}

void crt_fwrite(X86 *c) {
    uint32_t buf = arg(c, 0), size = arg(c, 1), count = arg(c, 2), fp = arg(c, 3);
    GFile *gf = gfile(fp);
    uint64_t total = (uint64_t)size * count;
    if (!gf || !total || !gm_valid(buf, (uint32_t)total)) {
        set_eax(c, 0);
        return;
    }
    size_t put = 0;
    if (!gf->text) {
        put = fwrite(gp(buf), 1, (size_t)total, gf->f);
    } else {
        for (; put < total; ++put) {
            char ch = gp(buf)[put];
            if (ch == '\n' && fputc('\r', gf->f) == EOF)
                break;
            if (fputc(ch, gf->f) == EOF)
                break;
        }
    }
    if (put < total)
        set_flag(fp, IOERR, true);
    set_eax(c, (uint32_t)(put / size));
}

void crt_fseek(X86 *c) {
    uint32_t fp = arg(c, 0);
    GFile *gf = gfile(fp);
    if (!gf) {
        set_eax(c, 0xffffffffu);
        return;
    }
    int origin = (int)arg(c, 2);
    int r = fseek(gf->f, (long)(int32_t)arg(c, 1),
                  origin == 1   ? SEEK_CUR
                  : origin == 2 ? SEEK_END
                                : SEEK_SET);
    set_flag(fp, IOEOF, false);
    set_eax(c, r ? 0xffffffffu : 0);
}

void crt_ftell(X86 *c) {
    GFile *gf = gfile(arg(c, 0));
    set_eax(c, gf ? (uint32_t)(int32_t)ftell(gf->f) : 0xffffffffu);
}

void write_stat(uint32_t buf, const OsStat &st) {
    for (uint32_t i = 0; i < 36; i += 4)
        wr32(buf + i, 0);
    uint16_t mode = st.is_dir ? 0x4000 | 0x40 : 0x8000;
    mode |= 0x100; // read
    if (!st.is_readonly)
        mode |= 0x80;
    wr32(buf, 2); // drive C:
    wr16(buf + 6, mode);
    wr16(buf + 8, 1);
    wr32(buf + 16, 2);
    wr32(buf + 20, (uint32_t)st.size);
    wr32(buf + 24, (uint32_t)st.atime);
    wr32(buf + 28, (uint32_t)st.mtime);
    wr32(buf + 32, (uint32_t)st.ctime);
}

void crt_stat(X86 *c) {
    std::string name = gm_str(arg(c, 0));
    std::string host = win32_host_path(name, false);
    OsStat st;
    if (host.empty() || os_stat(host.c_str(), &st) != 0) {
        set_errno(2);
        set_eax(c, 0xffffffffu);
        return;
    }
    write_stat(arg(c, 1), st);
    set_eax(c, 0);
}

void crt_fstat(X86 *c) {
    uint32_t fd = arg(c, 0);
    for (auto &kv : g_files)
        if (kv.second.fd == fd) {
            OsStat st;
            fflush(kv.second.f);
            if (os_fd_stat(fileno(kv.second.f), &st) == 0) {
                write_stat(arg(c, 1), st);
                set_eax(c, 0);
                return;
            }
        }
    set_errno(9);
    set_eax(c, 0xffffffffu);
}

// ---------------------------------------------------------------------------
// Time (32-bit time_t)
// ---------------------------------------------------------------------------
void read_tm(uint32_t a, struct tm *t) {
    memset(t, 0, sizeof *t);
    t->tm_sec = (int)rd32(a);
    t->tm_min = (int)rd32(a + 4);
    t->tm_hour = (int)rd32(a + 8);
    t->tm_mday = (int)rd32(a + 12);
    t->tm_mon = (int)rd32(a + 16);
    t->tm_year = (int)rd32(a + 20);
    t->tm_wday = (int)rd32(a + 24);
    t->tm_yday = (int)rd32(a + 28);
    t->tm_isdst = (int)rd32(a + 32);
}
void write_tm(uint32_t a, const struct tm *t) {
    const int v[9] = {t->tm_sec,  t->tm_min,  t->tm_hour, t->tm_mday, t->tm_mon,
                      t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst};
    for (int i = 0; i < 9; ++i)
        wr32(a + 4 * (uint32_t)i, (uint32_t)v[i]);
}

void crt_time(X86 *c) {
    uint32_t t = (uint32_t)time(nullptr);
    if (arg(c, 0))
        wr32(arg(c, 0), t);
    set_eax(c, t);
}
void crt_localtime(X86 *c) {
    if (!arg(c, 0)) {
        set_eax(c, 0);
        return;
    }
    struct tm tmv;
    if (os_localtime((int64_t)(int32_t)rd32(arg(c, 0)), &tmv) != 0) {
        set_eax(c, 0);
        return;
    }
    write_tm(g.tm(), &tmv);
    set_eax(c, g.tm());
}
void crt_mktime(X86 *c) {
    struct tm tmv;
    read_tm(arg(c, 0), &tmv);
    time_t t = mktime(&tmv);
    if (t != (time_t)-1)
        write_tm(arg(c, 0), &tmv);
    set_eax(c, (uint32_t)(int32_t)t);
}
void crt_difftime(X86 *c) {
    fpush(c, (double)(int32_t)arg(c, 0) - (double)(int32_t)arg(c, 1));
}
// MSVC's strftime accepts a '#' flag (the ISO C "alternate form") that removes
// leading zeros: "%#d" is "5", not "05"; "%#H" is "9", not "09". The host C
// library does not implement the flag, so passing "%B %#d, %#Y" straight to it
// reached the Petz Profile dialog as "October #d, #Y". Format one conversion at
// a time with the host library (whose output for the plain specifier matches
// MSVC), then apply the flag's rule to that conversion's text. Uses the host
// only for the pieces it knows; '#' never reaches it.
std::string msvc_strftime(const std::string &fmt, const struct tm *tmv) {
    std::string out;
    out.reserve(fmt.size() + 16);
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        size_t j = i + 1;
        bool alt = false;
        std::string flags;
        while (j < fmt.size() &&
               (fmt[j] == '#' || fmt[j] == '-' || fmt[j] == '0' || fmt[j] == '^')) {
            if (fmt[j] == '#')
                alt = true; // emulated below; never passed to the host
            else
                flags += fmt[j];
            ++j;
        }
        if (j >= fmt.size()) { // trailing '%' or flags: copy the rest verbatim
            out += fmt.substr(i);
            break;
        }
        const char conv = fmt[j];
        i = j;
        if (conv == '%') {
            out += '%';
            continue;
        }
        char spec[8];
        size_t n = 0;
        spec[n++] = '%';
        for (char f : flags)
            spec[n++] = f;
        spec[n++] = conv;
        spec[n] = '\0';
        char buf[128];
        size_t got = strftime(buf, sizeof(buf), spec, tmv);
        std::string s(buf, got);
        if (alt) {
            switch (conv) {
            case 'd':
            case 'H':
            case 'I':
            case 'j':
            case 'm':
            case 'M':
            case 'S':
            case 'U':
            case 'w':
            case 'W':
            case 'y':
            case 'Y': {
                size_t zero = s.find_first_not_of('0');
                if (zero == std::string::npos)
                    s = "0"; // "00" like "0" for a zero field
                else if (zero)
                    s.erase(0, zero);
                break;
            }
            case 'p':
            case 'Z': // the alternate form lowercases the text
                for (char &ch : s)
                    ch = char(tolower((unsigned char)ch));
                break;
            default: // "%#c"/"%#x" keep the locale's long form, as the host returns
                break;
            }
        }
        out += s;
    }
    return out;
}
void crt_strftime(X86 *c) {
    uint32_t out = arg(c, 0), max = arg(c, 1);
    struct tm tmv;
    read_tm(arg(c, 3), &tmv);
    std::string s = msvc_strftime(gm_str(arg(c, 2)), &tmv);
    if (!max || s.size() + 1 > max) { // the CRT writes nothing and returns 0
        set_eax(c, 0);
        return;
    }
    gm_put_str(out, s.c_str(), uint32_t(s.size()) + 1);
    set_eax(c, (uint32_t)s.size());
}

// ---------------------------------------------------------------------------
// Random numbers: Visual C++'s generator, so seeded sequences match.
// ---------------------------------------------------------------------------
uint32_t g_seed = 1;
void crt_srand(X86 *c) {
    g_seed = arg(c, 0);
    set_eax(c, 0);
}
void crt_rand(X86 *c) {
    g_seed = g_seed * 214013u + 2531011u;
    set_eax(c, (g_seed >> 16) & 0x7fff);
}

// ---------------------------------------------------------------------------
// Sorting with guest comparators. Merge sort over indices: stable, and safe
// with a comparator that is not a strict weak ordering.
// ---------------------------------------------------------------------------
void crt_qsort(X86 *c) {
    uint32_t base = arg(c, 0), n = arg(c, 1), w = arg(c, 2), cmp = arg(c, 3);
    if (n < 2 || !w) {
        set_eax(c, 0);
        return;
    }
    std::vector<uint32_t> idx(n), tmp(n);
    for (uint32_t i = 0; i < n; ++i)
        idx[i] = i;
    for (uint32_t width = 1; width < n; width *= 2) {
        for (uint32_t lo = 0; lo < n; lo += 2 * width) {
            uint32_t mid = lo + width < n ? lo + width : n;
            uint32_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            uint32_t a = lo, b = mid, k = lo;
            while (a < mid && b < hi) {
                int32_t r = (int32_t)guest_call(c, cmp, base + idx[a] * w, base + idx[b] * w);
                tmp[k++] = r <= 0 ? idx[a++] : idx[b++];
            }
            while (a < mid)
                tmp[k++] = idx[a++];
            while (b < hi)
                tmp[k++] = idx[b++];
        }
        idx.swap(tmp);
    }
    std::vector<char> copy(gp(base), gp(base) + (size_t)n * w);
    for (uint32_t i = 0; i < n; ++i)
        memcpy(gp(base + i * w), copy.data() + (size_t)idx[i] * w, w);
    set_eax(c, 0);
}

void crt_bsearch(X86 *c) {
    uint32_t key = arg(c, 0), base = arg(c, 1), n = arg(c, 2), w = arg(c, 3), cmp = arg(c, 4);
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int32_t r = (int32_t)guest_call(c, cmp, key, base + mid * w);
        if (r == 0) {
            set_eax(c, base + mid * w);
            return;
        }
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// x87 helpers: _CI* take and return their operands on the FPU stack.
// ---------------------------------------------------------------------------
void crt_ftol(X86 *c) {
    double v = fpop(c);
    int64_t r = (v != v || v >= 9.2233720368547758e18 || v < -9.2233720368547758e18) ? INT64_MIN
                                                                                     : (int64_t)v;
    set_eax64(c, (uint64_t)r);
}
void crt_cipow(X86 *c) {
    double y = fpop(c), x = fpop(c);
    fpush(c, pow(x, y));
}
void crt_ciasin(X86 *c) {
    fpush(c, asin(fpop(c)));
}
void crt_ciacos(X86 *c) {
    fpush(c, acos(fpop(c)));
}
double arg_double(X86 *c, int i) {
    uint64_t b = (uint64_t)arg(c, i) | (uint64_t)arg(c, i + 1) << 32;
    double d;
    memcpy(&d, &b, 8);
    return d;
}
void crt_floor(X86 *c) {
    fpush(c, floor(arg_double(c, 0)));
}

// ---------------------------------------------------------------------------
// RTTI. The complete object locator sits at vtable[-1]: {signature, offset,
// cdOffset, type descriptor, class hierarchy}. Type descriptors keep their
// decorated name at +8.
// ---------------------------------------------------------------------------
struct ThisOffsets {
    int32_t this_offset, vbase_descr, vbase_offset;
};
ThisOffsets read_offsets(uint32_t a) {
    return ThisOffsets{(int32_t)rd32(a), (int32_t)rd32(a + 4), (int32_t)rd32(a + 8)};
}
uint32_t this_pointer(const ThisOffsets &o, uint32_t obj) {
    if (!obj)
        return 0;
    uint32_t p = obj;
    if (o.vbase_descr >= 0) {
        p = obj + (uint32_t)o.vbase_descr;
        p += rd32(rd32(p) + (uint32_t)o.vbase_offset);
    }
    return p + (uint32_t)o.this_offset;
}
bool same_type(uint32_t a, uint32_t b) {
    return a == b || (a && b && strcmp(gp(a + 8), gp(b + 8)) == 0);
}

void crt_rtdynamiccast(X86 *c) {
    uint32_t obj = arg(c, 0), target = arg(c, 3), is_ref = arg(c, 4);
    if (!obj) {
        set_eax(c, 0);
        return;
    }
    uint32_t locator = rd32(rd32(obj) - 4);
    uint32_t complete = obj - rd32(locator + 4);
    uint32_t hierarchy = rd32(locator + 16);
    uint32_t count = rd32(hierarchy + 8), bases = rd32(hierarchy + 12);
    for (uint32_t i = 0; i < count && i < 256; ++i) {
        uint32_t desc = rd32(bases + 4 * i);
        if (same_type(rd32(desc), target)) {
            set_eax(c, this_pointer(read_offsets(desc + 8), complete));
            return;
        }
    }
    if (is_ref)
        crt_fatal("bad_cast from a reference dynamic_cast");
    set_eax(c, 0);
}

void crt_type_info_dtor(X86 *c) {
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// MSVC C++ exceptions.
//
// A frame using them links {next, handler, trylevel} with EBP = frame + 12
// and the function's ESP saved at frame - 4. Its handler is a stub
// `MOV EAX, funcinfo ; JMP __CxxFrameHandler`; funcinfo is
// {magic, unwind count, unwind map, try count, try map, ...}. Unwind and
// catch funclets run with the frame's EBP; a catch funclet returns the
// continuation address, which runs with the function's ESP and EBP restored.
// The transfer reuses the SEH landing machinery: global unwind to the frame,
// then an intercepted jump that lands on the frame's host checkpoint.
// ---------------------------------------------------------------------------
constexpr uint32_t CXX_EXCEPTION = 0xe06d7363;
constexpr uint32_t EH_UNWINDING = 2, EH_EXIT_UNWIND = 4;

struct CurrentException {
    uint32_t object, info;
};
thread_local CurrentException t_current[64];
thread_local uint32_t t_current_depth = 0;

// Runs a funclet with EBP set to `ebp`; the dispatcher's other registers are
// preserved, as Windows' call_ebp_func does.
uint32_t call_funclet(X86 *c, uint32_t fn, uint32_t ebp) {
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof saved);
    c->r[R_EBP] = ebp;
    uint32_t eax = guest_call(c, fn);
    for (int i = 0; i < 8; ++i)
        if (i != R_ESP)
            c->r[i] = saved[i];
    return eax;
}

void cxx_local_unwind(X86 *c, uint32_t frame, uint32_t funcinfo, int32_t last) {
    uint32_t count = rd32(funcinfo + 4), table = rd32(funcinfo + 8);
    int32_t level = (int32_t)rd32(frame + 8);
    while (level != last) {
        if (level < 0 || (uint32_t)level >= count) {
            LOGW("MSVCRT: bad trylevel %d in frame %08x (funcinfo %08x)", level, frame, funcinfo);
            crt_fatal("corrupt C++ unwind state");
        }
        uint32_t entry = table + 8 * (uint32_t)level;
        int32_t prev = (int32_t)rd32(entry);
        uint32_t handler = rd32(entry + 4);
        wr32(frame + 8, (uint32_t)prev);
        if (handler)
            call_funclet(c, handler, frame + 12);
        level = prev;
    }
    wr32(frame + 8, (uint32_t)last);
}

// The thrown type that a catch block accepts, or 0.
uint32_t caught_type(uint32_t catchblock, uint32_t throwinfo) {
    uint32_t ctype = rd32(catchblock + 4);
    uint32_t table = rd32(throwinfo + 12);
    uint32_t count = rd32(table);
    for (uint32_t i = 0; i < count && i < 64; ++i) {
        uint32_t type = rd32(table + 4 + 4 * i);
        if (!ctype || !rd8(ctype + 8))
            return type;
        if (!same_type(ctype, rd32(type + 4)))
            continue;
        uint32_t tflags = rd32(throwinfo), cflags = rd32(catchblock);
        if ((tflags & 1) && !(cflags & 1))
            continue;
        if ((tflags & 2) && !(cflags & 2))
            continue;
        return type;
    }
    return 0;
}

void copy_exception(X86 *c, uint32_t frame, uint32_t catchblock, uint32_t type, uint32_t object) {
    uint32_t ctype = rd32(catchblock + 4);
    int32_t offset = (int32_t)rd32(catchblock + 8);
    if (!ctype || !rd8(ctype + 8) || !offset || !type)
        return;
    uint32_t dest = frame + 12 + (uint32_t)offset;
    ThisOffsets offs = read_offsets(type + 8);
    uint32_t tflags = rd32(type), size = rd32(type + 20), ctor = rd32(type + 24);
    if (rd32(catchblock) & 8) {
        wr32(dest, this_pointer(offs, object));
    } else if (tflags & 1) {
        if (size && gm_valid(dest, size) && gm_valid(object, size))
            memmove(gp(dest), gp(object), size);
        if (size == 4)
            wr32(dest, this_pointer(offs, rd32(dest)));
    } else {
        uint32_t src = this_pointer(offs, object);
        if (ctor) {
            uint32_t saved[8];
            memcpy(saved, c->r, sizeof saved);
            c->r[R_ECX] = dest;
            if (tflags & 4)
                guest_call(c, ctor, src, 1);
            else
                guest_call(c, ctor, src);
            for (int i = 0; i < 8; ++i)
                if (i != R_ESP)
                    c->r[i] = saved[i];
        } else if (size && gm_valid(dest, size) && gm_valid(src, size)) {
            memmove(gp(dest), gp(src), size);
        }
    }
}

[[noreturn]] void continue_at(X86 *c, uint32_t frame, uint32_t esp, uint32_t ebp, uint32_t target) {
    LOGV("MSVCRT: resuming at %08x (frame %08x ESP=%08x EBP=%08x)", target, frame, esp, ebp);
    c->r[R_ESP] = esp;
    c->r[R_EBP] = ebp;
    recomp_seh_set_pending(c, frame);
    recomp_seh_intercept(c, target);
    crt_fatal("exception landing returned");
}

void crt_cxx_frame_handler(X86 *c) {
    uint32_t funcinfo = c->r[R_EAX];
    uint32_t rec = arg(c, 0), frame = arg(c, 1);
    uint32_t magic = rd32(funcinfo);
    if (magic < 0x19930520 || magic > 0x19930522) {
        LOGW("MSVCRT: funcinfo %08x has magic %08x", funcinfo, magic);
        crt_fatal("unknown C++ frame layout");
    }
    uint32_t flags = rd32(rec + 4);
    if (flags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        if (rd32(funcinfo + 4))
            cxx_local_unwind(c, frame, funcinfo, -1);
        set_eax(c, 1);
        return;
    }
    uint32_t code = rd32(rec);
    bool cxx = code == CXX_EXCEPTION && rd32(rec + 16) == 3 && rd32(rec + 20) >= 0x19930520 &&
               rd32(rec + 20) <= 0x19930522;
    uint32_t object = cxx ? rd32(rec + 24) : 0, throwinfo = cxx ? rd32(rec + 28) : 0;
    int32_t level = (int32_t)rd32(frame + 8);
    uint32_t tries = rd32(funcinfo + 12), trymap = rd32(funcinfo + 16);
    for (uint32_t t = 0; t < tries; ++t) {
        uint32_t tb = trymap + 20 * t;
        int32_t start = (int32_t)rd32(tb), end = (int32_t)rd32(tb + 4);
        if (level < start || level > end)
            continue;
        uint32_t ncatch = rd32(tb + 12), catches = rd32(tb + 16);
        for (uint32_t k = 0; k < ncatch; ++k) {
            uint32_t cb = catches + 16 * k;
            uint32_t type = 0;
            if (cxx) {
                type = caught_type(cb, throwinfo);
                if (!type)
                    continue;
            } else {
                uint32_t ctype = rd32(cb + 4);
                if (ctype && rd8(ctype + 8))
                    continue; // only catch(...) accepts a non-C++ exception
            }
            LOGV("MSVCRT: frame %08x catches exception %08x in try %u block %u", frame, code, t, k);
            if (cxx)
                copy_exception(c, frame, cb, type, object);
            // Global unwind: every frame above this one runs its cleanups.
            recomp_seh_unwind(c, frame, 0, rec, 0);
            recomp_seh_set_pending(c, 0);
            cxx_local_unwind(c, frame, funcinfo, start);
            wr32(frame + 8, (uint32_t)(end + 1));
            if (t_current_depth < 64)
                t_current[t_current_depth] = CurrentException{object, throwinfo};
            ++t_current_depth;
            uint32_t cont = call_funclet(c, rd32(cb + 12), frame + 12);
            --t_current_depth;
            if (cxx && rd32(throwinfo + 4) && object) {
                uint32_t saved[8];
                memcpy(saved, c->r, sizeof saved);
                c->r[R_ECX] = object;
                guest_call(c, rd32(throwinfo + 4));
                for (int i = 0; i < 8; ++i)
                    if (i != R_ESP)
                        c->r[i] = saved[i];
            }
            continue_at(c, frame, rd32(frame - 4), frame + 12, cont);
        }
    }
    set_eax(c, 1); // ExceptionContinueSearch
}

void crt_cxx_throw(X86 *c) {
    uint32_t object = arg(c, 0), info = arg(c, 1);
    if (!info) {
        if (!t_current_depth || t_current_depth > 64)
            crt_fatal("throw; with no exception being handled");
        object = t_current[t_current_depth - 1].object;
        info = t_current[t_current_depth - 1].info;
    }
    uint32_t params = heap_alloc(12);
    wr32(params, 0x19930520);
    wr32(params + 4, object);
    wr32(params + 8, info);
    LOGV("MSVCRT: throw object=%08x info=%08x (%s)", object, info,
         win32_describe_cxx_throw(CXX_EXCEPTION, 3, params).c_str());
    uint32_t args[4] = {CXX_EXCEPTION, 1, 3, params};
    call_import(c, "KERNEL32.dll", "RaiseException", args, 4);
    crt_fatal("C++ exception returned from RaiseException");
}

// ---------------------------------------------------------------------------
// _except_handler3: __try/__except/__finally. The registration is
// {next, handler, scope table, trylevel} with EBP = frame + 16, the
// EXCEPTION_POINTERS slot at frame - 4 and the saved ESP at frame - 8. Scope
// entries are {previous level, filter (0 for __finally), handler}.
// ---------------------------------------------------------------------------
void seh_local_unwind(X86 *c, uint32_t frame, int32_t last) {
    uint32_t table = rd32(frame + 8);
    int32_t level = (int32_t)rd32(frame + 12);
    for (int guard = 0; level != last && level != -1 && guard < 1024; ++guard) {
        uint32_t entry = table + 12 * (uint32_t)level;
        int32_t prev = (int32_t)rd32(entry);
        wr32(frame + 12, (uint32_t)prev);
        if (!rd32(entry + 4))
            call_funclet(c, rd32(entry + 8), frame + 16);
        level = prev;
    }
}

void crt_except_handler3(X86 *c) {
    uint32_t rec = arg(c, 0), frame = arg(c, 1), context = arg(c, 2);
    uint32_t flags = rd32(rec + 4);
    if (flags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        seh_local_unwind(c, frame, -1);
        set_eax(c, 1);
        return;
    }
    uint32_t pointers = heap_alloc(8);
    wr32(pointers, rec);
    wr32(pointers + 4, context);
    wr32(frame - 4, pointers);
    uint32_t table = rd32(frame + 8);
    int32_t level = (int32_t)rd32(frame + 12);
    for (int guard = 0; level != -1 && guard < 1024; ++guard) {
        uint32_t entry = table + 12 * (uint32_t)level;
        uint32_t filter = rd32(entry + 4);
        if (filter) {
            int32_t verdict = (int32_t)call_funclet(c, filter, frame + 16);
            if (verdict < 0) {
                LOGW("MSVCRT: __except filter %08x asked to continue execution", filter);
                crt_fatal("EXCEPTION_CONTINUE_EXECUTION is not supported");
            }
            if (verdict > 0) {
                recomp_seh_unwind(c, frame, 0, rec, 0);
                recomp_seh_set_pending(c, 0);
                seh_local_unwind(c, frame, level);
                wr32(frame + 12, rd32(entry));
                continue_at(c, frame, rd32(frame - 8), frame + 16, rd32(entry + 8));
            }
        }
        level = (int32_t)rd32(entry);
    }
    set_eax(c, 1);
}

const ImportShim shims[] = {
    {"MSVCRT.dll", "__set_app_type", ARGC_CDECL, crt_set_app_type},
    {"MSVCRT.dll", "__getmainargs", ARGC_CDECL, crt_getmainargs},
    {"MSVCRT.dll", "_initterm", ARGC_CDECL, crt_initterm},
    {"MSVCRT.dll", "_onexit", ARGC_CDECL, crt_onexit},
    {"MSVCRT.dll", "__dllonexit", ARGC_CDECL, crt_dllonexit},
    {"MSVCRT.dll", "exit", ARGC_CDECL, crt_exit},
    {"MSVCRT.dll", "_exit", ARGC_CDECL, crt__exit},
    {"MSVCRT.dll", "_XcptFilter", ARGC_CDECL, crt_xcptfilter},
    {"MSVCRT.dll", "_controlfp", ARGC_CDECL, crt_controlfp},
    {"MSVCRT.dll", "__p__fmode", ARGC_CDECL, crt_p_fmode},
    {"MSVCRT.dll", "__p__commode", ARGC_CDECL, crt_p_commode},
    {"MSVCRT.dll", "__setusermatherr", ARGC_CDECL, crt_zero},
    {"MSVCRT.dll", "_errno", ARGC_CDECL, crt_errno},
    {"MSVCRT.dll", "_purecall", ARGC_CDECL, crt_purecall},
    {"MSVCRT.dll", "?terminate@@YAXXZ", ARGC_CDECL, crt_terminate},
    {"MSVCRT.dll", "_chdrive", ARGC_CDECL, crt_zero},
    {"MSVCRT.dll", "_getdrive", ARGC_CDECL, crt_getdrive},
    {"MSVCRT.dll", "_beginthread", ARGC_CDECL, crt_beginthread},
    {"MSVCRT.dll", "getenv", ARGC_CDECL, crt_getenv},
    {"MSVCRT.dll", "malloc", ARGC_CDECL, crt_malloc},
    {"MSVCRT.dll", "calloc", ARGC_CDECL, crt_calloc},
    {"MSVCRT.dll", "free", ARGC_CDECL, crt_free},
    {"MSVCRT.dll", "_stricmp", ARGC_CDECL, crt_stricmp},
    {"MSVCRT.dll", "_strnicmp", ARGC_CDECL, crt_strnicmp},
    {"MSVCRT.dll", "strncmp", ARGC_CDECL, crt_strncmp},
    {"MSVCRT.dll", "strchr", ARGC_CDECL, crt_strchr},
    {"MSVCRT.dll", "strrchr", ARGC_CDECL, crt_strrchr},
    {"MSVCRT.dll", "strstr", ARGC_CDECL, crt_strstr},
    {"MSVCRT.dll", "strspn", ARGC_CDECL, crt_strspn},
    {"MSVCRT.dll", "strcspn", ARGC_CDECL, crt_strcspn},
    {"MSVCRT.dll", "strncpy", ARGC_CDECL, crt_strncpy},
    {"MSVCRT.dll", "memmove", ARGC_CDECL, crt_memmove},
    {"MSVCRT.dll", "_strlwr", ARGC_CDECL, crt_strlwr},
    {"MSVCRT.dll", "_strrev", ARGC_CDECL, crt_strrev},
    {"MSVCRT.dll", "_itoa", ARGC_CDECL, crt_itoa},
    {"MSVCRT.dll", "atoi", ARGC_CDECL, crt_atoi},
    {"MSVCRT.dll", "atol", ARGC_CDECL, crt_atoi},
    {"MSVCRT.dll", "tolower", ARGC_CDECL, crt_tolower},
    {"MSVCRT.dll", "toupper", ARGC_CDECL, crt_toupper},
    {"MSVCRT.dll", "isdigit", ARGC_CDECL, crt_isdigit},
    {"MSVCRT.dll", "isspace", ARGC_CDECL, crt_isspace},
    {"MSVCRT.dll", "isalnum", ARGC_CDECL, crt_isalnum},
    {"MSVCRT.dll", "sprintf", ARGC_CDECL, crt_sprintf},
    {"MSVCRT.dll", "vsprintf", ARGC_CDECL, crt_vsprintf},
    {"MSVCRT.dll", "sscanf", ARGC_CDECL, crt_sscanf},
    {"MSVCRT.dll", "fopen", ARGC_CDECL, crt_fopen},
    {"MSVCRT.dll", "fclose", ARGC_CDECL, crt_fclose},
    {"MSVCRT.dll", "fgetc", ARGC_CDECL, crt_fgetc},
    {"MSVCRT.dll", "fgets", ARGC_CDECL, crt_fgets},
    {"MSVCRT.dll", "fread", ARGC_CDECL, crt_fread},
    {"MSVCRT.dll", "fwrite", ARGC_CDECL, crt_fwrite},
    {"MSVCRT.dll", "fseek", ARGC_CDECL, crt_fseek},
    {"MSVCRT.dll", "ftell", ARGC_CDECL, crt_ftell},
    {"MSVCRT.dll", "_stat", ARGC_CDECL, crt_stat},
    {"MSVCRT.dll", "_fstat", ARGC_CDECL, crt_fstat},
    {"MSVCRT.dll", "time", ARGC_CDECL, crt_time},
    {"MSVCRT.dll", "localtime", ARGC_CDECL, crt_localtime},
    {"MSVCRT.dll", "mktime", ARGC_CDECL, crt_mktime},
    {"MSVCRT.dll", "difftime", ARGC_CDECL, crt_difftime},
    {"MSVCRT.dll", "strftime", ARGC_CDECL, crt_strftime},
    {"MSVCRT.dll", "srand", ARGC_CDECL, crt_srand},
    {"MSVCRT.dll", "rand", ARGC_CDECL, crt_rand},
    {"MSVCRT.dll", "qsort", ARGC_CDECL, crt_qsort},
    {"MSVCRT.dll", "bsearch", ARGC_CDECL, crt_bsearch},
    {"MSVCRT.dll", "_ftol", ARGC_CDECL, crt_ftol},
    {"MSVCRT.dll", "_CIpow", ARGC_CDECL, crt_cipow},
    {"MSVCRT.dll", "_CIasin", ARGC_CDECL, crt_ciasin},
    {"MSVCRT.dll", "_CIacos", ARGC_CDECL, crt_ciacos},
    {"MSVCRT.dll", "floor", ARGC_CDECL, crt_floor},
    {"MSVCRT.dll", "__RTDynamicCast", ARGC_CDECL, crt_rtdynamiccast},
    {"MSVCRT.dll", "??1type_info@@UAE@XZ", 0, crt_type_info_dtor},
    {"MSVCRT.dll", "__CxxFrameHandler", ARGC_CDECL, crt_cxx_frame_handler},
    {"MSVCRT.dll", "_CxxThrowException", 2, crt_cxx_throw},
    {"MSVCRT.dll", "_except_handler3", ARGC_CDECL, crt_except_handler3},
};
} // namespace

void msvcrt_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data("MSVCRT.dll", "_acmdln", 4);
    imports_register_data("MSVCRT.dll", "_adjust_fdiv", 4);
}
