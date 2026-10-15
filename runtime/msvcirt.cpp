// msvcirt.cpp - MSVCIRT.dll shims: Visual C++ 6's old (<iostream.h>,
// <strstream.h>, <fstream.h>) run-time library.
//
// The game was linked against the shared VC6 iostream library and both calls
// it and derives from it. Fidelity here is about class layout, not just entry
// points: the game's own code reads ios, filebuf and streambuf fields at fixed
// offsets, installs its own vtables for objects whose virtual base is ios, and
// constructs istrstream/ifstream/ofstream objects on its own stack by calling
// the imported constructors. The structures below reproduce the layout Wine's
// dlls/msvcirt/msvcirt.c uses, which is binary-compatible with the shipped
// msvcirt.dll. Evidence from the game itself:
//
//   - an istrstream object is { vbptr, extract_delim, count, ios } with the
//     ios virtual base at +0x0c (its vbtable is {0, 0xc}); the game reads the
//     vbptr from [object] and the vbase offset from [vbptr+4], then patches
//     the ios subobject's vfptr. 00425a30.asm.
//   - an ostream/ofstream object is { vbptr, unknown, ios } with ios at +0x08;
//     the game reads ios->sb at [object+0xc] and filebuf::fd at [sb+0x4c],
//     which fixes sizeof(streambuf) at 0x4c (Wine's do_lock + 24-byte
//     CRITICAL_SECTION tail). 004d8aa0.asm.
//   - ios+0x34 is do_lock and ios+0x38 the lock; the game calls _mtlock with
//     the latter. 00425a30.asm.
//
// For classes with a virtual base the Microsoft object model puts the vbptr
// first and keeps the class's only vfptr inside the ios virtual base
// sub-object, so a virtual member is entered with ECX = the ios sub-object
// (see the game calling ??1istrstream with ECX = EBP-0x70, the ios address).
// Plain members take ECX = the object pointer.
//
// Because the game may own a derived class whose ios vfptr is its own code,
// every virtual the library invokes on an object is dispatched through the
// object's *guest* vtable (guest_call with ECX = object), never by a direct
// native call. The guest vtables this file hands out point their slots at
// import trampolines for the base implementations, so a game override is
// reached on the same path the real DLL would have used.
#include "imports.h"
#include "memory.h"
#include "win32.h"
#include "../platform/os.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// Guest layouts. Offsets are dwords unless the field is a single byte.
// ---------------------------------------------------------------------------

// streambuf (Wine: vtable, allocated, unbuffered, stored_char, base, ebuf,
// pbase, pptr, epptr, eback, gptr, egptr, do_lock, CRITICAL_SECTION).
enum {
    SB_VTABLE = 0,
    SB_ALLOCATED = 4,
    SB_UNBUFFERED = 8,
    SB_STORED_CHAR = 12,
    SB_BASE = 16,
    SB_EBUF = 20,
    SB_PBASE = 24,
    SB_PPTR = 28,
    SB_EPPTR = 32,
    SB_EBACK = 36,
    SB_GPTR = 40,
    SB_EGPTR = 44,
    SB_DO_LOCK = 48,
    SB_LOCK = 52,
    SB_SIZE = 76, // 0x4c: the game reads filebuf::fd here.
};

// filebuf adds { filedesc fd; int close; }.
enum { FB_FD = 76, FB_CLOSE = 80, FB_SIZE = 84 };

// strstreambuf adds { dynamic, increase, unknown, constant, f_alloc, f_free }.
enum {
    SS_DYNAMIC = 76,
    SS_INCREASE = 80,
    SS_UNKNOWN = 84,
    SS_CONSTANT = 88,
    SS_ALLOC = 92,
    SS_FREE = 96,
    SS_SIZE = 100,
};

// ios.
enum {
    IOS_VTABLE = 0,
    IOS_SB = 4,
    IOS_STATE = 8,
    IOS_SPECIAL = 12, // int[4]
    IOS_DELBUF = 28,
    IOS_TIE = 32,
    IOS_FLAGS = 36,
    IOS_PRECISION = 40,
    IOS_FILL = 44, // char, then 3 pad bytes
    IOS_WIDTH = 48,
    IOS_DO_LOCK = 52, // 0x34
    IOS_LOCK = 56,    // 0x38
    IOS_SIZE = 80,
};

// istream adds { extract_delim, count } after the vbptr; ios at +0x0c.
// ostream adds { unknown } (kept for the layout); ios at +0x08.
enum { IST_EXTRACT = 4, IST_COUNT = 8, IST_IOS = 12 };
enum { OST_UNKNOWN = 4, OST_IOS = 8 };

// ios state bits.
enum { ST_GOOD = 0, ST_EOF = 1, ST_FAIL = 2, ST_BAD = 4 };
// ios flags.
enum {
    F_SKIPWS = 0x0001,
    F_LEFT = 0x0002,
    F_RIGHT = 0x0004,
    F_INTERNAL = 0x0008,
    F_DEC = 0x0010,
    F_OCT = 0x0020,
    F_HEX = 0x0040,
    F_SHOWBASE = 0x0080,
    F_SHOWPOINT = 0x0100,
    F_UPPERCASE = 0x0200,
    F_SHOWPOS = 0x0400,
    F_SCIENTIFIC = 0x0800,
    F_FIXED = 0x1000,
    F_UNITBUF = 0x2000,
    F_STDIO = 0x4000,
};
// open modes (ios::open_mode) and seek directions.
enum {
    OM_IN = 0x1,
    OM_OUT = 0x2,
    OM_ATE = 0x4,
    OM_APP = 0x8,
    OM_TRUNC = 0x10,
    OM_NOCREATE = 0x20,
    OM_NOREPLACE = 0x40,
    OM_BINARY = 0x80,
};
enum { SD_BEG = 0, SD_CUR = 1, SD_END = 2 };

constexpr int32_t VEOF = -1; // EOF keeps its int32 value in the guest.

// Virtual table slots, in bytes of the vtable.
enum {
    VT_DTOR = 0,
    VT_SYNC = 4,
    VT_SETBUF = 8,
    VT_SEEKOFF = 12,
    VT_SEEKPOS = 16,
    VT_XSPUTN = 20,
    VT_XSGETN = 24,
    VT_OVERFLOW = 28,
    VT_UNDERFLOW = 32,
    VT_PBACKFAIL = 36,
    VT_DOALLOCATE = 40,
};

static inline int32_t gi(uint32_t a) {
    return (int32_t)rd32(a);
}
static inline void wi(uint32_t a, int32_t v) {
    wr32(a, (uint32_t)v);
}

// The ios virtual base sub-object of an ostream/istream object.
static inline uint32_t ios_of(uint32_t obj) {
    uint32_t vb = rd32(obj);
    return obj + rd32(vb + 4);
}

static inline uint32_t sb_of_ios(uint32_t io) {
    return rd32(io + IOS_SB);
}

// Heap wrappers for the library's operator new/delete.
uint32_t op_new(uint32_t size) {
    return heap_alloc(size ? size : 1, true);
}
void op_delete(uint32_t p) {
    if (p)
        heap_free(p);
}

// ---------------------------------------------------------------------------
// Virtual dispatch through a guest object's vtable. The target may be an
// import trampoline (a base implementation) or the game's own code (a
// derived-class override); either way it runs as a guest call with `this` in
// ECX. This is the only path the library may use to reach a virtual member.
// ---------------------------------------------------------------------------
uint32_t vt_call(X86 *c, uint32_t obj, int slot, const uint32_t *args, int n) {
    uint32_t target = rd32(rd32(obj) + (uint32_t)slot);
    if (!target) {
        LOGW("MSVCIRT: null vtable slot %d on object %08x", slot, obj);
        return 0;
    }
    uint32_t saved_ecx = c->r[R_ECX];
    c->r[R_ECX] = obj;
    uint32_t r = guest_call(c, target, args, n);
    c->r[R_ECX] = saved_ecx;
    return r;
}
uint32_t vt0(X86 *c, uint32_t obj, int slot) {
    return vt_call(c, obj, slot, nullptr, 0);
}
uint32_t vt1(X86 *c, uint32_t obj, int slot, uint32_t a0) {
    uint32_t a[1] = {a0};
    return vt_call(c, obj, slot, a, 1);
}
uint32_t vt2(X86 *c, uint32_t obj, int slot, uint32_t a0, uint32_t a1) {
    uint32_t a[2] = {a0, a1};
    return vt_call(c, obj, slot, a, 2);
}
uint32_t vt3(X86 *c, uint32_t obj, int slot, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t a[3] = {a0, a1, a2};
    return vt_call(c, obj, slot, a, 3);
}

// ---------------------------------------------------------------------------
// Guest vtables. Each is allocated in the guest heap so the object's vptr can
// hold a guest address, with a zero RTTI dword just before slot 0. Slots hold
// trampolines for the registered shim names, resolved on demand.
// ---------------------------------------------------------------------------
uint32_t build_vtable(uint32_t &cache, const char *const *slots, int n) {
    if (cache && heap_owns(cache - 4))
        return cache;
    uint32_t mem = heap_alloc(4u * (uint32_t)(n + 1), true);
    if (!mem)
        return 0;
    for (int i = 0; i < n; ++i) {
        uint32_t t = slots[i] ? imports_resolve("MSVCIRT.dll", slots[i]) : 0;
        if (!t)
            LOGW("MSVCIRT: no trampoline for vtable slot %s", slots[i] ? slots[i] : "(deleted)");
        wr32(mem + 4 + 4u * (uint32_t)i, t);
    }
    cache = mem + 4;
    return cache;
}

uint32_t build_vbtable(uint32_t &cache, uint32_t vbase_offset) {
    if (cache && heap_owns(cache))
        return cache;
    uint32_t mem = heap_alloc(8, true);
    if (!mem)
        return 0;
    wr32(mem, 0); // offset from the vbptr back to the complete object
    wr32(mem + 4, vbase_offset);
    cache = mem;
    return cache;
}

// Class vtable descriptions (Wine's __ASM_VTABLE order).
const char *const k_streambuf_slots[] = {
    "??_Estreambuf@@UAEPAXI@Z",          "?sync@streambuf@@UAEHXZ",
    "?setbuf@streambuf@@UAEPAV1@PADH@Z", "?seekoff@streambuf@@UAEJJW4seek_dir@ios@@H@Z",
    "?seekpos@streambuf@@UAEJJH@Z",      "?xsputn@streambuf@@UAEHPBDH@Z",
    "?xsgetn@streambuf@@UAEHPADH@Z",     "?overflow@streambuf@@UAEHH@Z",
    "?underflow@streambuf@@UAEHXZ",      "?pbackfail@streambuf@@UAEHH@Z",
    "?doallocate@streambuf@@MAEHXZ",
};
const char *const k_filebuf_slots[] = {
    "??_Efilebuf@@UAEPAXI@Z",
    "?sync@filebuf@@UAEHXZ",
    "?setbuf@filebuf@@UAEPAVstreambuf@@PADH@Z",
    "?seekoff@filebuf@@UAEJJW4seek_dir@ios@@H@Z",
    "?seekpos@streambuf@@UAEJJH@Z",
    "?xsputn@streambuf@@UAEHPBDH@Z",
    "?xsgetn@streambuf@@UAEHPADH@Z",
    "?overflow@filebuf@@UAEHH@Z",
    "?underflow@filebuf@@UAEHXZ",
    "?pbackfail@streambuf@@UAEHH@Z",
    "?doallocate@streambuf@@MAEHXZ",
};
const char *const k_strstreambuf_slots[] = {
    "??_Estrstreambuf@@UAEPAXI@Z",
    "?sync@strstreambuf@@UAEHXZ",
    "?setbuf@strstreambuf@@UAEPAVstreambuf@@PADH@Z",
    "?seekoff@strstreambuf@@UAEJJW4seek_dir@ios@@H@Z",
    "?seekpos@streambuf@@UAEJJH@Z",
    "?xsputn@streambuf@@UAEHPBDH@Z",
    "?xsgetn@streambuf@@UAEHPADH@Z",
    "?overflow@strstreambuf@@UAEHH@Z",
    "?underflow@strstreambuf@@UAEHXZ",
    "?pbackfail@streambuf@@UAEHH@Z",
    "?doallocate@strstreambuf@@MAEHXZ",
};
const char *const k_ios_slots[] = {"??_Eios@@UAEPAXI@Z"};
const char *const k_ostream_slots[] = {"??_Eostream@@UAEPAXI@Z"};
const char *const k_istream_slots[] = {"??_Eistream@@UAEPAXI@Z"};
const char *const k_ostrstream_slots[] = {"??_Eostrstream@@UAEPAXI@Z"};
const char *const k_ofstream_slots[] = {"??_Eofstream@@UAEPAXI@Z"};
const char *const k_istrstream_slots[] = {"??_Eistrstream@@UAEPAXI@Z"};
const char *const k_ifstream_slots[] = {"??_Eifstream@@UAEPAXI@Z"};

struct Tables {
    uint32_t streambuf = 0, filebuf = 0, strstreambuf = 0, ios = 0;
    uint32_t ostream = 0, ostrstream = 0, ofstream = 0;
    uint32_t istream = 0, istrstream = 0, ifstream = 0;
    uint32_t ostream_vbt = 0, istream_vbt = 0;
};
Tables g_t;

void ensure_tables() {
    build_vbtable(g_t.ostream_vbt, OST_IOS);
    build_vbtable(g_t.istream_vbt, IST_IOS);
    build_vtable(g_t.streambuf, k_streambuf_slots, 11);
    build_vtable(g_t.filebuf, k_filebuf_slots, 11);
    build_vtable(g_t.strstreambuf, k_strstreambuf_slots, 11);
    build_vtable(g_t.ios, k_ios_slots, 1);
    build_vtable(g_t.ostream, k_ostream_slots, 1);
    build_vtable(g_t.istream, k_istream_slots, 1);
    build_vtable(g_t.ostrstream, k_ostrstream_slots, 1);
    build_vtable(g_t.ofstream, k_ofstream_slots, 1);
    build_vtable(g_t.istrstream, k_istrstream_slots, 1);
    build_vtable(g_t.ifstream, k_ifstream_slots, 1);
}

// ---------------------------------------------------------------------------
// streambuf field access and the non-virtual inline helpers the library's own
// member functions use. Virtual members go through vt_call.
// ---------------------------------------------------------------------------
static inline uint32_t sb_base(uint32_t s) {
    return rd32(s + SB_BASE);
}
static inline uint32_t sb_ebuf(uint32_t s) {
    return rd32(s + SB_EBUF);
}
static inline uint32_t sb_pbase(uint32_t s) {
    return rd32(s + SB_PBASE);
}
static inline uint32_t sb_pptr(uint32_t s) {
    return rd32(s + SB_PPTR);
}
static inline uint32_t sb_epptr(uint32_t s) {
    return rd32(s + SB_EPPTR);
}
static inline uint32_t sb_eback(uint32_t s) {
    return rd32(s + SB_EBACK);
}
static inline uint32_t sb_gptr(uint32_t s) {
    return rd32(s + SB_GPTR);
}
static inline uint32_t sb_egptr(uint32_t s) {
    return rd32(s + SB_EGPTR);
}
static inline int32_t sb_unbuffered(uint32_t s) {
    return gi(s + SB_UNBUFFERED);
}

void sb_setb(uint32_t s, uint32_t ba, uint32_t eb, int del) {
    if (gi(s + SB_ALLOCATED))
        op_delete(sb_base(s));
    wi(s + SB_ALLOCATED, del);
    wr32(s + SB_BASE, ba);
    wr32(s + SB_EBUF, eb);
}
void sb_setg(uint32_t s, uint32_t ek, uint32_t gp, uint32_t eg) {
    wr32(s + SB_EBACK, ek);
    wr32(s + SB_GPTR, gp);
    wr32(s + SB_EGPTR, eg);
}
void sb_setp(uint32_t s, uint32_t pb, uint32_t ep) {
    wr32(s + SB_PBASE, pb);
    wr32(s + SB_PPTR, pb);
    wr32(s + SB_EPPTR, ep);
}
uint32_t sb_setbuf(X86 *, uint32_t s, uint32_t buf, int len) {
    if (sb_base(s))
        return 0;
    if (!buf || !len) {
        wi(s + SB_UNBUFFERED, 1);
        wr32(s + SB_BASE, 0);
        wr32(s + SB_EBUF, 0);
    } else {
        wi(s + SB_UNBUFFERED, 0);
        wr32(s + SB_BASE, buf);
        wr32(s + SB_EBUF, buf + (uint32_t)len);
    }
    return s;
}

// Non-virtual streambuf helpers (Wine's inline members).
int sb_sgetc(X86 *c, uint32_t s) {
    if (sb_unbuffered(s)) {
        if (gi(s + SB_STORED_CHAR) == VEOF)
            wi(s + SB_STORED_CHAR, (int32_t)vt0(c, s, VT_UNDERFLOW));
        return gi(s + SB_STORED_CHAR);
    }
    return (int32_t)vt0(c, s, VT_UNDERFLOW);
}
int sb_sputc(X86 *c, uint32_t s, int ch) {
    if (sb_pptr(s) < sb_epptr(s)) {
        uint32_t p = sb_pptr(s);
        wr8(p, (uint8_t)ch);
        wr32(s + SB_PPTR, p + 1);
        return ch & 0xff;
    }
    return (int32_t)vt1(c, s, VT_OVERFLOW, (uint32_t)ch);
}
int sb_sbumpc(X86 *c, uint32_t s) {
    int ret;
    if (sb_unbuffered(s)) {
        ret = gi(s + SB_STORED_CHAR);
        wi(s + SB_STORED_CHAR, VEOF);
        if (ret == VEOF)
            ret = (int32_t)vt0(c, s, VT_UNDERFLOW);
    } else {
        ret = sb_gptr(s) < sb_egptr(s) ? (int)rd8(sb_gptr(s)) : (int32_t)vt0(c, s, VT_UNDERFLOW);
        wr32(s + SB_GPTR, sb_gptr(s) + 1);
    }
    return ret;
}
int sb_snextc(X86 *c, uint32_t s) {
    if (sb_unbuffered(s)) {
        if (gi(s + SB_STORED_CHAR) == VEOF)
            vt0(c, s, VT_UNDERFLOW);
        int v = (int32_t)vt0(c, s, VT_UNDERFLOW);
        wi(s + SB_STORED_CHAR, v);
        return v;
    }
    if (sb_gptr(s) >= sb_egptr(s))
        vt0(c, s, VT_UNDERFLOW);
    wr32(s + SB_GPTR, sb_gptr(s) + 1);
    return sb_gptr(s) < sb_egptr(s) ? (int)rd8(sb_gptr(s)) : (int32_t)vt0(c, s, VT_UNDERFLOW);
}
void sb_stossc(X86 *c, uint32_t s) {
    if (sb_unbuffered(s)) {
        if (gi(s + SB_STORED_CHAR) == VEOF)
            vt0(c, s, VT_UNDERFLOW);
        else
            wi(s + SB_STORED_CHAR, VEOF);
    } else {
        if (sb_gptr(s) >= sb_egptr(s))
            vt0(c, s, VT_UNDERFLOW);
        if (sb_gptr(s) < sb_egptr(s))
            wr32(s + SB_GPTR, sb_gptr(s) + 1);
    }
}
int sb_sgetn(X86 *c, uint32_t s, uint32_t buf, int n) {
    return (int32_t)vt2(c, s, VT_XSGETN, buf, (uint32_t)n);
}
int sb_sputn(X86 *c, uint32_t s, uint32_t buf, int n) {
    return (int32_t)vt2(c, s, VT_XSPUTN, buf, (uint32_t)n);
}
int sb_sputbackc(X86 *c, uint32_t s, int ch) {
    return (int32_t)vt1(c, s, VT_PBACKFAIL, (uint32_t)ch);
}
int sb_in_avail(uint32_t s) {
    return sb_egptr(s) > sb_gptr(s) ? (int)(sb_egptr(s) - sb_gptr(s)) : 0;
}
int sb_out_waiting(uint32_t s) {
    return sb_pptr(s) > sb_pbase(s) ? (int)(sb_pptr(s) - sb_pbase(s)) : 0;
}
int sb_allocate(X86 *c, uint32_t s) {
    if (sb_base(s) || sb_unbuffered(s))
        return 0;
    return (int32_t)vt0(c, s, VT_DOALLOCATE);
}

// Non-virtual ios helpers.
int ios_good(uint32_t io) {
    return gi(io + IOS_STATE) == ST_GOOD;
}
void ios_setstate(uint32_t io, int32_t st) {
    wi(io + IOS_STATE, st);
}
void ios_orstate(uint32_t io, int32_t bits) {
    wi(io + IOS_STATE, gi(io + IOS_STATE) | bits);
}
int ios_lockc(uint32_t io) {
    (void)io;
    return 0;
}
int ios_unlockc(uint32_t io) {
    (void)io;
    return 0;
}

void st_active_ctor(uint32_t s, uint32_t vtable, uint32_t buf, int len) {
    wr32(s + SB_VTABLE, vtable);
    wi(s + SB_ALLOCATED, 0);
    wi(s + SB_STORED_CHAR, VEOF);
    wi(s + SB_DO_LOCK, -1);
    wr32(s + SB_BASE, 0);
    sb_setbuf(nullptr, s, buf, len);
    sb_setg(s, 0, 0, 0);
    sb_setp(s, 0, 0);
}
void sb_ctor(X86 *c, uint32_t s) {
    ensure_tables();
    st_active_ctor(s, g_t.streambuf, 0, 0);
    wi(s + SB_UNBUFFERED, 0);
    (void)c;
}
void streambuf_dtor_impl(uint32_t s) {
    if (gi(s + SB_ALLOCATED))
        op_delete(sb_base(s));
}
uint32_t streambuf_doallocate_impl(uint32_t s) {
    const uint32_t kReserve = 512;
    uint32_t buf = op_new(kReserve);
    if (!buf)
        return (uint32_t)VEOF;
    sb_setb(s, buf, buf + kReserve, 1);
    return 1;
}

// ---------------------------------------------------------------------------
// filebuf I/O over host descriptors. Text mode (the default for a stream
// opened without ios::binary) maps CRLF <-> LF and treats Ctrl-Z as EOF, as
// the VC6 filebuf does by leaving the descriptor in text mode.
// ---------------------------------------------------------------------------
std::map<uint32_t, int> g_fb_text; // filebuf -> text mode flag

int fb_text(uint32_t fb) {
    auto it = g_fb_text.find(fb);
    return it == g_fb_text.end() ? 1 : it->second;
}
int fb_fd(uint32_t fb) {
    return gi(fb + FB_FD);
}

int64_t raw_write(uint32_t fb, const char *p, int64_t n) {
    if (!fb_text(fb))
        return os_fd_write(fb_fd(fb), p, (size_t)n);
    int64_t written = 0;
    std::string out;
    out.reserve((size_t)n + 8);
    for (int64_t i = 0; i < n; ++i) {
        if (p[i] == '\n')
            out += '\r';
        out += p[i];
    }
    written = os_fd_write(fb_fd(fb), out.data(), out.size());
    return written < 0 ? -1 : n; // report source bytes, not expanded bytes
}
int64_t raw_seek(uint32_t fb, int64_t off, int whence) {
    return os_fd_seek(fb_fd(fb), off, whence);
}

uint32_t filebuf_open(X86 *, uint32_t fb, const char *name, int mode, int protection);
uint32_t filebuf_close(X86 *c, uint32_t fb) {
    if (fb_fd(fb) == -1)
        return 0;
    uint32_t ret;
    if ((int32_t)vt0(c, fb, VT_SYNC) == VEOF || os_fd_close(fb_fd(fb)) < 0) {
        ret = 0;
    } else {
        wi(fb + FB_FD, -1);
        ret = fb;
    }
    return ret;
}
uint32_t filebuf_open(X86 *c, uint32_t fb, const char *name, int mode, int protection) {
    (void)protection;
    if (fb_fd(fb) != -1)
        return 0;
    if (mode & (OM_APP | OM_TRUNC))
        mode |= OM_OUT;
    int inout = mode & (OM_IN | OM_OUT);
    int flags;
    switch (inout) {
    case OM_IN:
        flags = OS_O_RDONLY;
        break;
    case OM_OUT:
        flags = OS_O_WRONLY;
        break;
    case OM_IN | OM_OUT:
        flags = OS_O_RDWR;
        break;
    default:
        return 0;
    }
    if (mode & OM_TRUNC)
        flags |= OS_O_TRUNC;
    if ((mode & OM_OUT) && !(mode & (OM_IN | OM_APP | OM_ATE)))
        flags |= OS_O_TRUNC;
    if (!(mode & OM_NOCREATE))
        flags |= OS_O_CREAT;
    if (mode & OM_NOREPLACE)
        flags |= OS_O_EXCL;

    std::string host = win32_host_path(name, (mode & OM_OUT) != 0);
    if (host.empty())
        return 0;
    int fd = os_fd_open(host.c_str(), flags);
    if (fd < 0)
        return 0;

    g_fb_text[fb] = !(mode & OM_BINARY);
    wi(fb + FB_FD, fd);
    wi(fb + FB_CLOSE, 1);
    if ((mode & OM_APP) || (mode & OM_ATE))
        os_fd_seek(fd, 0, OS_SEEK_END);
    sb_allocate(c, fb);
    return fb;
}

// ---------------------------------------------------------------------------
// Virtual members - streambuf family. All take ECX = the streambuf.
// ---------------------------------------------------------------------------
int vt_streambuf_sync_h(X86 *, uint32_t s) {
    return (sb_gptr(s) >= sb_egptr(s) && sb_pbase(s) >= sb_pptr(s)) ? 0 : VEOF;
}
int vt_streambuf_overflow_h(X86 *, uint32_t s, int) {
    (void)s;
    return VEOF;
}
int vt_streambuf_underflow_h(X86 *, uint32_t s) {
    (void)s;
    return VEOF;
}
int vt_streambuf_xsgetn_impl(X86 *c, uint32_t s, uint32_t buffer, int count) {
    int copied = 0, chunk;
    if (sb_unbuffered(s)) {
        if (gi(s + SB_STORED_CHAR) == VEOF)
            wi(s + SB_STORED_CHAR, (int32_t)vt0(c, s, VT_UNDERFLOW));
        while (copied < count && gi(s + SB_STORED_CHAR) != VEOF) {
            wr8(buffer + (uint32_t)copied++, (uint8_t)gi(s + SB_STORED_CHAR));
            wi(s + SB_STORED_CHAR, (int32_t)vt0(c, s, VT_UNDERFLOW));
        }
    } else {
        while (copied < count) {
            if ((int32_t)vt0(c, s, VT_UNDERFLOW) == VEOF)
                break;
            chunk = (int)(sb_egptr(s) - sb_gptr(s));
            if (chunk > count - copied)
                chunk = count - copied;
            if (chunk <= 0)
                break;
            memcpy(gm_ptr(buffer + (uint32_t)copied), gm_ptr(sb_gptr(s)), (size_t)chunk);
            wr32(s + SB_GPTR, sb_gptr(s) + (uint32_t)chunk);
            copied += chunk;
        }
    }
    return copied;
}
int vt_streambuf_xsputn_impl(X86 *c, uint32_t s, uint32_t data, int length) {
    int copied = 0, chunk;
    while (copied < length) {
        if (sb_unbuffered(s) || sb_pptr(s) == sb_epptr(s)) {
            if ((int32_t)vt1(c, s, VT_OVERFLOW, (uint32_t)rd8(data + (uint32_t)copied)) == VEOF)
                break;
            ++copied;
        } else {
            chunk = (int)(sb_epptr(s) - sb_pptr(s));
            if (chunk > length - copied)
                chunk = length - copied;
            memcpy(gm_ptr(sb_pptr(s)), gm_ptr(data + (uint32_t)copied), (size_t)chunk);
            wr32(s + SB_PPTR, sb_pptr(s) + (uint32_t)chunk);
            copied += chunk;
        }
    }
    return copied;
}
int vt_streambuf_pbackfail_h(X86 *c, uint32_t s, int ch) {
    if (sb_gptr(s) > sb_eback(s)) {
        uint32_t p = sb_gptr(s) - 1;
        wr32(s + SB_GPTR, p);
        wr8(p, (uint8_t)ch);
        return ch & 0xff;
    }
    if ((int32_t)vt3(c, s, VT_SEEKOFF, (uint32_t)-1, SD_CUR, OM_IN) == VEOF)
        return VEOF;
    if (!sb_unbuffered(s) && sb_egptr(s)) {
        uint32_t n = sb_egptr(s) - sb_gptr(s);
        if (n > 1)
            memmove(gm_ptr(sb_gptr(s) + 1), gm_ptr(sb_gptr(s)), (size_t)(n - 1));
        wr8(sb_gptr(s), (uint8_t)ch);
    }
    return ch & 0xff;
}
uint32_t vt_streambuf_setbuf_h(X86 *, uint32_t s, uint32_t buffer, int length) {
    return sb_setbuf(nullptr, s, buffer, length);
}
int32_t vt_streambuf_seekoff_h(X86 *, uint32_t s, int32_t, int, int) {
    (void)s;
    return VEOF;
}
int32_t vt_streambuf_seekpos_h(X86 *c, uint32_t s, int32_t pos, int mode) {
    return (int32_t)vt3(c, s, VT_SEEKOFF, (uint32_t)pos, SD_BEG, (uint32_t)mode);
}

void vt_streambuf_vec_dtor_h(X86 *, uint32_t s, uint32_t) {
    streambuf_dtor_impl(s);
}

// ---------------------------------------------------------------------------
// Virtual members - filebuf. ECX = the filebuf (its streambuf base).
// ---------------------------------------------------------------------------
int vt_filebuf_sync_h(X86 *, uint32_t fb) {
    if (fb_fd(fb) == -1)
        return VEOF;
    if (sb_unbuffered(fb))
        return 0;
    if (sb_pptr(fb) != 0) {
        int count = (int)(sb_pptr(fb) - sb_pbase(fb));
        if (count > 0 && raw_write(fb, (const char *)gm_ptr(sb_pbase(fb)), count) != count)
            return VEOF;
    }
    wr32(fb + SB_PBASE, 0);
    wr32(fb + SB_PPTR, 0);
    wr32(fb + SB_EPPTR, 0);
    if (sb_egptr(fb) != 0) {
        int64_t offset = (int64_t)(sb_egptr(fb) - sb_gptr(fb));
        if (offset > 0) {
            if (fb_text(fb)) {
                for (uint32_t p = sb_gptr(fb); p < sb_egptr(fb); ++p)
                    if (rd8(p) == '\n')
                        ++offset;
            }
            if (os_fd_seek(fb_fd(fb), -offset, OS_SEEK_CUR) < 0)
                return VEOF;
        }
    }
    wr32(fb + SB_EBACK, 0);
    wr32(fb + SB_GPTR, 0);
    wr32(fb + SB_EGPTR, 0);
    return 0;
}
int vt_filebuf_overflow_h(X86 *c, uint32_t fb, int ch) {
    if ((int32_t)vt0(c, fb, VT_SYNC) == VEOF)
        return VEOF;
    if (sb_unbuffered(fb)) {
        if (ch == VEOF)
            return 1;
        char b = (char)ch;
        return raw_write(fb, &b, 1) == 1 ? (ch & 0xff) : VEOF;
    }
    if (sb_allocate(c, fb) == VEOF)
        return VEOF;
    wr32(fb + SB_PBASE, sb_base(fb));
    wr32(fb + SB_PPTR, sb_base(fb));
    wr32(fb + SB_EPPTR, sb_ebuf(fb));
    if (ch != VEOF) {
        uint32_t p = sb_pptr(fb);
        wr8(p, (uint8_t)ch);
        wr32(fb + SB_PPTR, p + 1);
    }
    return 1;
}
int vt_filebuf_underflow_h(X86 *c, uint32_t fb) {
    if (sb_unbuffered(fb)) {
        char ch;
        int64_t n = os_fd_read(fb_fd(fb), &ch, 1);
        if (n < 1)
            return VEOF;
        if (fb_text(fb) && ch == '\r') {
            char next;
            int64_t m = os_fd_read(fb_fd(fb), &next, 1);
            if (m == 1 && next != '\n')
                os_fd_seek(fb_fd(fb), -1, OS_SEEK_CUR);
            ch = '\n';
        } else if (fb_text(fb) && (unsigned char)ch == 0x1a) {
            os_fd_seek(fb_fd(fb), -1, OS_SEEK_CUR);
            return VEOF;
        }
        return (unsigned char)ch;
    }
    if (sb_gptr(fb) >= sb_egptr(fb)) {
        if ((int32_t)vt0(c, fb, VT_SYNC) == VEOF)
            return VEOF;
        int buffer_size = (int)(sb_ebuf(fb) - sb_base(fb));
        int64_t read_bytes = os_fd_read(fb_fd(fb), gm_ptr(sb_base(fb)), (size_t)buffer_size);
        if (read_bytes <= 0)
            return VEOF;
        wr32(fb + SB_EBACK, sb_base(fb));
        wr32(fb + SB_GPTR, sb_base(fb));
        wr32(fb + SB_EGPTR, sb_base(fb) + (uint32_t)read_bytes);
    }
    return (int)rd8(sb_gptr(fb));
}
int32_t vt_filebuf_seekoff_h(X86 *c, uint32_t fb, int32_t offset, int dir, int mode) {
    (void)mode;
    if ((int32_t)vt0(c, fb, VT_SYNC) == VEOF)
        return VEOF;
    int64_t r = os_fd_seek(fb_fd(fb), offset,
                           dir == SD_CUR   ? OS_SEEK_CUR
                           : dir == SD_END ? OS_SEEK_END
                                           : OS_SEEK_SET);
    return r < 0 ? VEOF : (int32_t)r;
}
uint32_t vt_filebuf_setbuf_h(X86 *, uint32_t fb, uint32_t buffer, int length) {
    if (fb_fd(fb) != -1 && sb_base(fb) != 0)
        return 0;
    if (!buffer || !length) {
        wi(fb + SB_UNBUFFERED, 1);
    } else {
        if (gi(fb + SB_ALLOCATED)) {
            op_delete(sb_base(fb));
            wi(fb + SB_ALLOCATED, 0);
        }
        wr32(fb + SB_BASE, buffer);
        wr32(fb + SB_EBUF, buffer + (uint32_t)length);
    }
    return fb;
}
void vt_filebuf_vec_dtor_h(X86 *c, uint32_t fb, uint32_t) {
    if (gi(fb + FB_CLOSE))
        filebuf_close(c, fb);
    g_fb_text.erase(fb);
    streambuf_dtor_impl(fb);
}

// ---------------------------------------------------------------------------
// Virtual members - strstreambuf. ECX = the strstreambuf.
// ---------------------------------------------------------------------------
void strstreambuf_ctor_impl(uint32_t s) {
    ensure_tables();
    st_active_ctor(s, g_t.strstreambuf, 0, 0);
    wi(s + SB_UNBUFFERED, 0);
    wi(s + SS_DYNAMIC, 1);
    wi(s + SS_INCREASE, 1);
    wi(s + SS_CONSTANT, 0);
    wi(s + SS_ALLOC, 0);
    wi(s + SS_FREE, 0);
}
void strstreambuf_buffer_ctor_impl(uint32_t s, uint32_t buffer, int length, uint32_t put) {
    ensure_tables();
    uint32_t end_buffer;
    if (length > 0)
        end_buffer = buffer + (uint32_t)length;
    else if (length == 0)
        end_buffer = buffer + (uint32_t)strlen((const char *)gm_ptr(buffer));
    else
        end_buffer = 0xffffffffu;
    st_active_ctor(s, g_t.strstreambuf, 0, 0);
    sb_setb(s, buffer, end_buffer, 0);
    if (!put)
        sb_setg(s, buffer, buffer, end_buffer);
    else {
        sb_setg(s, buffer, buffer, put);
        sb_setp(s, put, end_buffer);
    }
    wi(s + SB_UNBUFFERED, 0);
    wi(s + SS_DYNAMIC, 0);
    wi(s + SS_INCREASE, 0);
    wi(s + SS_CONSTANT, 1);
    wi(s + SS_ALLOC, 0);
    wi(s + SS_FREE, 0);
}
uint32_t strstreambuf_doallocate_impl(X86 *c, uint32_t s) {
    uint32_t prev_buffer = sb_base(s);
    int32_t prev_size = (int32_t)(sb_ebuf(s) - sb_base(s));
    int32_t increase = gi(s + SS_INCREASE);
    int32_t new_size = (prev_size > 0 ? prev_size : 0) + (increase > 0 ? increase : 1);
    uint32_t new_buffer;
    uint32_t falloc = rd32(s + SS_ALLOC);
    if (falloc)
        new_buffer = guest_call(c, falloc, (uint32_t)new_size);
    else
        new_buffer = op_new((uint32_t)new_size);
    if (!new_buffer)
        return (uint32_t)VEOF;
    if (sb_ebuf(s)) {
        if (prev_size > 0)
            memcpy(gm_ptr(new_buffer), gm_ptr(prev_buffer), (size_t)prev_size);
        int32_t delta = (int32_t)(new_buffer - prev_buffer);
        if (sb_egptr(s)) {
            wr32(s + SB_EBACK, sb_eback(s) + (uint32_t)delta);
            wr32(s + SB_GPTR, sb_gptr(s) + (uint32_t)delta);
            wr32(s + SB_EGPTR, sb_egptr(s) + (uint32_t)delta);
        }
        if (sb_epptr(s)) {
            wr32(s + SB_PBASE, sb_pbase(s) + (uint32_t)delta);
            wr32(s + SB_PPTR, sb_pptr(s) + (uint32_t)delta);
            wr32(s + SB_EPPTR, sb_epptr(s) + (uint32_t)delta);
        }
        uint32_t ffree = rd32(s + SS_FREE);
        if (ffree)
            guest_call(c, ffree, prev_buffer);
        else
            op_delete(prev_buffer);
    }
    sb_setb(s, new_buffer, new_buffer + (uint32_t)new_size, 0);
    return 1;
}
int vt_strstreambuf_overflow_h(X86 *c, uint32_t s, int ch) {
    if (sb_pptr(s) >= sb_epptr(s)) {
        if (!gi(s + SS_DYNAMIC) || (int32_t)strstreambuf_doallocate_impl(c, s) == VEOF)
            return VEOF;
        if (!sb_epptr(s)) {
            uint32_t p = sb_egptr(s) ? sb_egptr(s) : sb_base(s);
            wr32(s + SB_PBASE, p);
            wr32(s + SB_PPTR, p);
        }
        wr32(s + SB_EPPTR, sb_ebuf(s));
    }
    if (ch != VEOF) {
        uint32_t p = sb_pptr(s);
        wr8(p, (uint8_t)ch);
        wr32(s + SB_PPTR, p + 1);
    }
    return 1;
}
int vt_strstreambuf_underflow_h(X86 *c, uint32_t s) {
    if (sb_gptr(s) < sb_egptr(s))
        return (int)rd8(sb_gptr(s));
    if (sb_egptr(s) < sb_pptr(s)) {
        wr32(s + SB_GPTR, sb_base(s) + (sb_gptr(s) - sb_eback(s)));
        wr32(s + SB_EBACK, sb_base(s));
        wr32(s + SB_EGPTR, sb_pptr(s));
    }
    (void)c;
    return sb_gptr(s) < sb_egptr(s) ? (int)rd8(sb_gptr(s)) : VEOF;
}
int32_t vt_strstreambuf_seekoff_h(X86 *c, uint32_t s, int32_t offset, int dir, int mode) {
    if (dir < 0 || dir > 2 || !(mode & (OM_IN | OM_OUT)))
        return VEOF;
    uint32_t base[3];
    if (mode & OM_IN) {
        vt0(c, s, VT_UNDERFLOW);
        base[SD_BEG] = sb_eback(s);
        base[SD_CUR] = sb_gptr(s);
        base[SD_END] = sb_egptr(s);
        int64_t target = (int64_t)base[dir] + offset;
        if (target < (int64_t)sb_eback(s) || target > (int64_t)sb_egptr(s))
            return VEOF;
        wr32(s + SB_GPTR, (uint32_t)target);
    }
    if (mode & OM_OUT) {
        if (!sb_epptr(s) && (int32_t)vt1(c, s, VT_OVERFLOW, (uint32_t)VEOF) == VEOF)
            return VEOF;
        base[SD_BEG] = sb_pbase(s);
        base[SD_CUR] = sb_pptr(s);
        base[SD_END] = sb_epptr(s);
        int64_t target = (int64_t)base[dir] + offset;
        if (target < (int64_t)sb_pbase(s))
            return VEOF;
        if (target > (int64_t)sb_epptr(s)) {
            if (!gi(s + SS_DYNAMIC))
                return VEOF;
            wi(s + SS_INCREASE, offset);
            if ((int32_t)strstreambuf_doallocate_impl(c, s) == VEOF)
                return VEOF;
        }
        wr32(s + SB_PPTR, (uint32_t)target);
        return (int32_t)((uint32_t)target - base[SD_BEG]);
    }
    return (int32_t)(sb_gptr(s) - base[SD_BEG]);
}
uint32_t vt_strstreambuf_setbuf_h(X86 *, uint32_t s, uint32_t, int length) {
    if (length)
        wi(s + SS_INCREASE, length);
    return s;
}
int vt_strstreambuf_sync_h(X86 *, uint32_t) {
    return 0;
}
void strstreambuf_dtor_impl(uint32_t s) {
    if (gi(s + SS_DYNAMIC) && sb_base(s)) {
        uint32_t ffree = rd32(s + SS_FREE);
        if (ffree)
            /* the free function is a guest callback; a dtor cannot take the
               dispatcher, so this is only reachable from a guest-driven
               delete. Fall back to the heap otherwise. */
            heap_free(sb_base(s));
        else
            op_delete(sb_base(s));
    }
    streambuf_dtor_impl(s);
}
void vt_strstreambuf_vec_dtor_h(X86 *, uint32_t s, uint32_t) {
    strstreambuf_dtor_impl(s);
}

// ---------------------------------------------------------------------------
// ios.
// ---------------------------------------------------------------------------
void ios_ctor_impl(uint32_t io) {
    ensure_tables();
    wr32(io + IOS_VTABLE, g_t.ios);
    wr32(io + IOS_SB, 0);
    wi(io + IOS_STATE, ST_BAD);
    for (int i = 0; i < 4; ++i)
        wi(io + IOS_SPECIAL + 4 * i, 0);
    wi(io + IOS_DELBUF, 0);
    wr32(io + IOS_TIE, 0);
    wi(io + IOS_FLAGS, 0);
    wi(io + IOS_PRECISION, 6);
    wr8(io + IOS_FILL, ' ');
    wi(io + IOS_WIDTH, 0);
    wi(io + IOS_DO_LOCK, -1);
}
void ios_sb_ctor_impl(uint32_t io, uint32_t sb) {
    ios_ctor_impl(io);
    wr32(io + IOS_SB, sb);
    wi(io + IOS_STATE, sb ? ST_GOOD : ST_BAD);
}
void ios_dtor_impl(X86 *c, uint32_t io) {
    uint32_t sb = sb_of_ios(io);
    if (gi(io + IOS_DELBUF) && sb)
        vt1(c, sb, VT_DTOR, 1);
    wr32(io + IOS_SB, 0);
    wi(io + IOS_STATE, ST_BAD);
}
void ios_init(X86 *c, uint32_t io, uint32_t sb) {
    uint32_t old = sb_of_ios(io);
    if (gi(io + IOS_DELBUF) && old)
        vt1(c, old, VT_DTOR, 1);
    wr32(io + IOS_SB, sb);
    if (!sb)
        ios_orstate(io, ST_BAD);
    else
        wi(io + IOS_STATE, gi(io + IOS_STATE) & ~ST_BAD);
}

// ---------------------------------------------------------------------------
// ostream.
// ---------------------------------------------------------------------------
void ostream_ctor_impl(X86 *c, uint32_t obj, int virt_init) {
    ensure_tables();
    uint32_t io;
    if (virt_init) {
        wr32(obj, g_t.ostream_vbt);
        io = ios_of(obj);
        ios_ctor_impl(io);
    } else {
        io = ios_of(obj);
    }
    wr32(io + IOS_VTABLE, g_t.ostream);
    wr32(obj + OST_UNKNOWN, 0);
    (void)c;
}
void ostream_sb_ctor_impl(X86 *c, uint32_t obj, uint32_t sb, int virt_init) {
    ostream_ctor_impl(c, obj, virt_init);
    ios_init(c, ios_of(obj), sb);
}
void ostream_dtor_impl(uint32_t) {
    // The ostream part owns nothing; the ios virtual base is destroyed by the
    // most-derived destructor (or the _vbase_destructor_).
}
void ostream_vbase_dtor_impl(X86 *c, uint32_t obj) {
    uint32_t io = obj + OST_IOS;
    ostream_dtor_impl(io);
    ios_dtor_impl(c, io);
}

void ostream_flush(X86 *c, uint32_t obj);
int ostream_opfx(X86 *c, uint32_t obj) {
    uint32_t io = ios_of(obj);
    if (!ios_good(io)) {
        ios_orstate(io, ST_FAIL);
        return 0;
    }
    ios_lockc(io);
    if (rd32(io + IOS_TIE))
        ostream_flush(c, rd32(io + IOS_TIE));
    return 1;
}
void ostream_osfx(X86 *c, uint32_t obj) {
    uint32_t io = ios_of(obj);
    ios_unlockc(io);
    wi(io + IOS_WIDTH, 0);
    if (gi(io + IOS_FLAGS) & F_UNITBUF)
        ostream_flush(c, obj);
    if (gi(io + IOS_FLAGS) & F_STDIO) {
        fflush(stdout);
        fflush(stderr);
    }
}
void ostream_flush(X86 *c, uint32_t obj) {
    uint32_t io = ios_of(obj);
    if ((int32_t)vt0(c, sb_of_ios(io), VT_SYNC) == VEOF)
        ios_orstate(io, ST_FAIL);
}
// Width/fill padded write. str2 may contain NUL, so its length is explicit.
void ostream_writepad(X86 *c, uint32_t obj, uint32_t s1, int l1, uint32_t s2, int l2) {
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    int flags = gi(io + IOS_FLAGS);
    int width = gi(io + IOS_WIDTH);
    char fill = (char)rd8(io + IOS_FILL);
    if (flags & (F_LEFT | F_INTERNAL)) {
        if (l1 && sb_sputn(c, sb, s1, l1) != l1)
            ios_orstate(io, ST_FAIL | ST_BAD);
        if (!(flags & F_INTERNAL) && l2 && sb_sputn(c, sb, s2, l2) != l2)
            ios_orstate(io, ST_FAIL | ST_BAD);
    }
    for (int i = l1 + l2; i < width; ++i)
        if (sb_sputc(c, sb, (unsigned char)fill) == VEOF)
            ios_orstate(io, ST_FAIL | ST_BAD);
    if ((flags & (F_LEFT | F_INTERNAL)) != F_LEFT) {
        if (!(flags & (F_LEFT | F_INTERNAL)) && l1 && sb_sputn(c, sb, s1, l1) != l1)
            ios_orstate(io, ST_FAIL | ST_BAD);
        if (l2 && sb_sputn(c, sb, s2, l2) != l2)
            ios_orstate(io, ST_FAIL | ST_BAD);
    }
}
void ostream_writepad_str(X86 *c, uint32_t obj, const char *prefix, const char *number) {
    uint32_t p1 = 0, p2 = 0;
    int l1 = (int)strlen(prefix), l2 = (int)strlen(number);
    uint32_t h1 = heap_alloc((uint32_t)l1 + 1, true);
    uint32_t h2 = heap_alloc((uint32_t)l2 + 1, true);
    if (l1) {
        memcpy(gm_ptr(h1), prefix, (size_t)l1 + 1);
        p1 = h1;
    }
    if (l2) {
        memcpy(gm_ptr(h2), number, (size_t)l2 + 1);
        p2 = h2;
    }
    ostream_writepad(c, obj, p1, l1, p2, l2);
    if (h1)
        heap_free(h1);
    if (h2)
        heap_free(h2);
}

// ---------------------------------------------------------------------------
// istream.
// ---------------------------------------------------------------------------
void istream_ctor_impl(X86 *c, uint32_t obj, int virt_init) {
    ensure_tables();
    uint32_t io;
    if (virt_init) {
        wr32(obj, g_t.istream_vbt);
        io = ios_of(obj);
        ios_ctor_impl(io);
    } else {
        io = ios_of(obj);
    }
    wr32(io + IOS_VTABLE, g_t.istream);
    wi(io + IOS_FLAGS, gi(io + IOS_FLAGS) | F_SKIPWS);
    wi(obj + IST_EXTRACT, 0);
    wi(obj + IST_COUNT, 0);
    (void)c;
}
void istream_sb_ctor_impl(X86 *c, uint32_t obj, uint32_t sb, int virt_init) {
    istream_ctor_impl(c, obj, virt_init);
    ios_init(c, ios_of(obj), sb);
}
void istream_dtor_impl(uint32_t) {}
void istream_vbase_dtor_impl(X86 *c, uint32_t obj) {
    uint32_t io = obj + IST_IOS;
    istream_dtor_impl(io);
    ios_dtor_impl(c, io);
}

void istream_eatwhite(X86 *c, uint32_t obj) {
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    int ch;
    for (ch = sb_sgetc(c, sb); ch != VEOF && isspace(ch); ch = sb_snextc(c, sb)) {
    }
    if (ch == VEOF)
        ios_orstate(io, ST_EOF);
}
int istream_ipfx(X86 *c, uint32_t obj, int need) {
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    if (need)
        wi(obj + IST_COUNT, 0);
    if (!ios_good(io)) {
        ios_orstate(io, ST_FAIL);
        return 0;
    }
    ios_lockc(io);
    uint32_t tie = rd32(io + IOS_TIE);
    if (tie && (!need || sb_in_avail(sb) < need))
        ostream_flush(c, tie);
    if ((gi(io + IOS_FLAGS) & F_SKIPWS) && !need) {
        istream_eatwhite(c, obj);
        if (gi(io + IOS_STATE) & ST_EOF) {
            ios_orstate(io, ST_FAIL);
            ios_unlockc(io);
            return 0;
        }
    }
    return 1;
}
void istream_isfx(X86 *c, uint32_t obj) {
    ios_unlockc(ios_of(obj));
}

int istream_getint(X86 *c, uint32_t obj, uint32_t str) {
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    int ch, num_base = 0, i = 0;
    bool scan_sign = true, scan_prefix = true, scan_x = false, valid_integer = false;
    if (istream_ipfx(c, obj, 0)) {
        int flags = gi(io + IOS_FLAGS);
        num_base = (flags & F_DEC) ? 10 : (flags & F_HEX) ? 16 : (flags & F_OCT) ? 8 : 0;
        for (ch = sb_sgetc(c, sb); i < 15; ch = sb_snextc(c, sb)) {
            if ((ch == '+' || ch == '-') && scan_sign) {
                scan_sign = false;
            } else if ((ch == 'x' || ch == 'X') && scan_x) {
                scan_x = valid_integer = false;
                num_base = 16;
            } else if (ch == '0' && scan_prefix) {
                scan_sign = scan_prefix = false;
                scan_x = !num_base || num_base == 16;
                valid_integer = true;
                if (!num_base)
                    num_base = 8;
            } else if (ch != VEOF &&
                       ((num_base == 8 && (ch >= '0' && ch <= '7')) ||
                        (num_base == 16 && isxdigit(ch)) || (!num_base && isdigit(ch)) ||
                        (num_base == 10 && isdigit(ch)))) {
                scan_sign = scan_prefix = scan_x = false;
                valid_integer = true;
            } else {
                if (!valid_integer) {
                    ios_orstate(io, ST_FAIL);
                    while (i > 0)
                        if (sb_sputbackc(c, sb, (int)rd8(str + (uint32_t)(--i))) == VEOF)
                            ios_orstate(io, ST_BAD);
                } else if (ch == VEOF) {
                    ios_orstate(io, ST_EOF);
                    if (scan_x && !(flags & (F_DEC | F_OCT | F_HEX)))
                        num_base = 0;
                }
                break;
            }
            wr8(str + (uint32_t)i, (uint8_t)ch);
            ++i;
        }
        wr8(str + (uint32_t)i, 0);
        istream_isfx(c, obj);
    }
    return num_base;
}
bool istream_read_integer(X86 *c, uint32_t obj, int32_t min_value, int32_t max_value, bool set_flag,
                          int32_t *out) {
    uint32_t io = ios_of(obj);
    uint32_t buf = heap_alloc(16, true);
    int num_base = istream_getint(c, obj, buf);
    const char *p = (const char *)gm_ptr(buf);
    errno = 0;
    long ret = strtol(p, nullptr, num_base);
    if (set_flag && errno == ERANGE)
        ios_orstate(io, ST_FAIL);
    else if (ret > max_value) {
        ios_orstate(io, ST_FAIL);
        ret = max_value;
    } else if (ret < min_value) {
        ios_orstate(io, ST_FAIL);
        ret = min_value;
    }
    *out = (int32_t)ret;
    heap_free(buf);
    return true;
}
int istream_getdouble(X86 *c, uint32_t obj, uint32_t str, int count) {
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    int ch, i = 0;
    bool scan_sign = true, scan_dot = true, scan_exp = true;
    bool valid_mantissa = false, valid_exponent = false;
    if (istream_ipfx(c, obj, 0)) {
        if (!count) {
            ios_orstate(io, ST_FAIL);
            i = -1;
        } else {
            for (ch = sb_sgetc(c, sb); i < count; ch = sb_snextc(c, sb)) {
                if ((ch == '+' || ch == '-') && scan_sign) {
                    scan_sign = false;
                } else if (ch == '.' && scan_dot) {
                    scan_sign = scan_dot = false;
                } else if ((ch == 'e' || ch == 'E') && scan_exp && valid_mantissa) {
                    scan_sign = true;
                    scan_dot = scan_exp = false;
                    valid_exponent = true;
                } else if (ch >= '0' && ch <= '9') {
                    valid_mantissa = true;
                } else {
                    if (!valid_mantissa) {
                        ios_orstate(io, ST_FAIL);
                        while (i > 0)
                            if (sb_sputbackc(c, sb, (int)rd8(str + (uint32_t)(--i))) == VEOF)
                                ios_orstate(io, ST_BAD);
                    } else if (ch == VEOF) {
                        ios_orstate(io, ST_EOF);
                    }
                    break;
                }
                wr8(str + (uint32_t)i, (uint8_t)ch);
                ++i;
            }
            wr8(str + (uint32_t)i, 0);
        }
        istream_isfx(c, obj);
    }
    (void)valid_exponent;
    return i > 0 ? 1 : (i == 0 ? 0 : -1);
}
bool istream_read_float(X86 *c, uint32_t obj, int max_chars, double *out) {
    uint32_t buf = heap_alloc(32, true);
    bool read = false;
    if (istream_ipfx(c, obj, 0)) {
        if (istream_getdouble(c, obj, buf, max_chars) > 0) {
            *out = strtod((const char *)gm_ptr(buf), nullptr);
            read = true;
        }
        istream_isfx(c, obj);
    }
    heap_free(buf);
    return read;
}

// ---------------------------------------------------------------------------
// Imported shims - constructors and destructors.
// ---------------------------------------------------------------------------
void ms_streambuf_ctor(X86 *c) {
    sb_ctor(c, c->r[R_ECX]);
    set_eax(c, c->r[R_ECX]);
}
void ms_streambuf_dtor(X86 *c) {
    streambuf_dtor_impl(c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_ios_ctor(X86 *c) {
    ios_ctor_impl(c->r[R_ECX]);
    set_eax(c, c->r[R_ECX]);
}
void ms_ios_dtor(X86 *c) {
    ios_dtor_impl(c, c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_ostream_dtor(X86 *c) {
    ostream_dtor_impl(c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_istream_dtor(X86 *c) {
    istream_dtor_impl(c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_ostream_sb_ctor(X86 *c) {
    ostream_sb_ctor_impl(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1));
    set_eax(c, c->r[R_ECX]);
}
void ms_ifstream_vbase_dtor(X86 *c) {
    istream_vbase_dtor_impl(c, c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_ofstream_vbase_dtor(X86 *c) {
    ostream_vbase_dtor_impl(c, c->r[R_ECX]);
    set_eax(c, 0);
}
void ms_istrstream_str_ctor(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t str = arg(c, 0);
    int virt_init = (int32_t)arg(c, 1);
    uint32_t ssb = heap_alloc(SS_SIZE, true);
    strstreambuf_buffer_ctor_impl(ssb, str, 0, 0);
    istream_sb_ctor_impl(c, obj, ssb, virt_init);
    wi(ios_of(obj) + IOS_DELBUF, 1);
    set_eax(c, obj);
}
void ms_ostrstream_ctor(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    int virt_init = (int32_t)arg(c, 0);
    uint32_t ssb = heap_alloc(SS_SIZE, true);
    strstreambuf_ctor_impl(ssb);
    ostream_sb_ctor_impl(c, obj, ssb, virt_init);
    wi(ios_of(obj) + IOS_DELBUF, 1);
    set_eax(c, obj);
}
void ms_ifstream_open_ctor(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    std::string name = gm_str(arg(c, 0));
    int mode = (int32_t)arg(c, 1);
    int prot = (int32_t)arg(c, 2);
    int virt_init = (int32_t)arg(c, 3);
    uint32_t fb = heap_alloc(FB_SIZE, true);
    st_active_ctor(fb, g_t.filebuf, 0, 0);
    wi(fb + SB_UNBUFFERED, 0);
    wi(fb + FB_FD, -1);
    wi(fb + FB_CLOSE, 0);
    istream_sb_ctor_impl(c, obj, fb, virt_init);
    wi(ios_of(obj) + IOS_DELBUF, 1);
    if (!filebuf_open(c, fb, name.c_str(), mode | OM_IN, prot))
        ios_orstate(ios_of(obj), ST_FAIL);
    set_eax(c, obj);
}
void ms_ofstream_open_ctor(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    std::string name = gm_str(arg(c, 0));
    int mode = (int32_t)arg(c, 1);
    int prot = (int32_t)arg(c, 2);
    int virt_init = (int32_t)arg(c, 3);
    uint32_t fb = heap_alloc(FB_SIZE, true);
    st_active_ctor(fb, g_t.filebuf, 0, 0);
    wi(fb + SB_UNBUFFERED, 0);
    wi(fb + FB_FD, -1);
    wi(fb + FB_CLOSE, 0);
    ostream_sb_ctor_impl(c, obj, fb, virt_init);
    wi(ios_of(obj) + IOS_DELBUF, 1);
    if (!filebuf_open(c, fb, name.c_str(), mode | OM_OUT, prot))
        ios_orstate(ios_of(obj), ST_FAIL);
    set_eax(c, obj);
}

// ---------------------------------------------------------------------------
// Imported shims - streambuf virtuals and strstreambuf/filebuf members.
// ---------------------------------------------------------------------------
void ms_xsgetn(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_xsgetn_impl(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1)));
}
void ms_xsputn(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_xsputn_impl(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1)));
}
void ms_streambuf_pbackfail(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_pbackfail_h(c, c->r[R_ECX], (int32_t)arg(c, 0)));
}
void ms_streambuf_doallocate(X86 *c) {
    set_eax(c, streambuf_doallocate_impl(c->r[R_ECX]));
}
void ms_streambuf_setbuf(X86 *c) {
    set_eax(c, sb_setbuf(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1)));
}
void ms_streambuf_seekoff(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_seekoff_h(c, c->r[R_ECX], (int32_t)arg(c, 0),
                                                (int32_t)arg(c, 1), (int32_t)arg(c, 2)));
}
void ms_streambuf_seekpos(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_seekpos_h(c, c->r[R_ECX], (int32_t)arg(c, 0),
                                                (int32_t)arg(c, 1)));
}
void ms_strstreambuf_str(X86 *c) {
    uint32_t s = c->r[R_ECX];
    wi(s + SS_DYNAMIC, 0); // freeze(1) then return the buffer
    set_eax(c, sb_base(s));
}
void ms_strstreambuf_freeze(X86 *c) {
    uint32_t s = c->r[R_ECX];
    if (!gi(s + SS_CONSTANT))
        wi(s + SS_DYNAMIC, arg(c, 0) ? 0 : 1);
    set_eax(c, 0);
}
void ms_filebuf_setmode(X86 *c) {
    uint32_t fb = c->r[R_ECX];
    int mode = (int32_t)arg(c, 0);
    if (mode != 0x4000 && mode != 0x8000) { // _O_TEXT / _O_BINARY
        set_eax(c, 0xffffffffu);
        return;
    }
    if ((int32_t)vt0(c, fb, VT_SYNC) == VEOF) {
        set_eax(c, 0xffffffffu);
        return;
    }
    g_fb_text[fb] = mode == 0x4000;
    set_eax(c, (uint32_t)mode);
}
void ms_ostrstream_rdbuf(X86 *c) {
    set_eax(c, sb_of_ios(ios_of(c->r[R_ECX])));
}

// ---------------------------------------------------------------------------
// Imported shims - ostream.
// ---------------------------------------------------------------------------
void ms_ostream_put(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    if (ostream_opfx(c, obj)) {
        if (sb_sputc(c, sb_of_ios(io), (int32_t)arg(c, 0)) == VEOF)
            ios_setstate(io, ST_BAD | ST_FAIL);
        ostream_osfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_ostream_write(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    int n = (int32_t)arg(c, 1);
    if (ostream_opfx(c, obj)) {
        if (sb_sputn(c, sb_of_ios(io), arg(c, 0), n) != n)
            ios_setstate(io, ST_BAD | ST_FAIL);
        ostream_osfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_ostream_flush(X86 *c) {
    ostream_flush(c, c->r[R_ECX]);
    set_eax(c, c->r[R_ECX]);
}
void ms_ostream_print_char(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    char ch = (char)arg(c, 0);
    uint32_t h = heap_alloc(1, true);
    wr8(h, (uint8_t)ch);
    if (ostream_opfx(c, obj)) {
        ostream_writepad(c, obj, 0, 0, h, 1);
        ostream_osfx(c, obj);
    }
    heap_free(h);
    set_eax(c, obj);
}
void ms_ostream_print_str(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    std::string s = gm_str(arg(c, 0));
    if (ostream_opfx(c, obj)) {
        ostream_writepad_str(c, obj, "", s.c_str());
        ostream_osfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_ostream_print_int(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    int n = (int32_t)arg(c, 0);
    if (ostream_opfx(c, obj)) {
        int flags = gi(io + IOS_FLAGS);
        char prefix[3] = {0, 0, 0};
        char fmt[8] = "%d";
        if (flags & F_HEX) {
            fmt[1] = (flags & F_UPPERCASE) ? 'X' : 'x';
            if (flags & F_SHOWBASE) {
                prefix[0] = '0';
                prefix[1] = (flags & F_UPPERCASE) ? 'X' : 'x';
            }
        } else if (flags & F_OCT) {
            fmt[1] = 'o';
            if (flags & F_SHOWBASE)
                prefix[0] = '0';
        } else {
            if ((flags & F_SHOWPOS) && n != 0 && n > 0)
                prefix[0] = '+';
        }
        char buf[32];
        snprintf(buf, sizeof buf, fmt, n);
        ostream_writepad_str(c, obj, prefix, buf);
        ostream_osfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_ostream_print_double(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint64_t bits = (uint64_t)arg(c, 0) | (uint64_t)arg(c, 1) << 32;
    double d;
    memcpy(&d, &bits, 8);
    if (ostream_opfx(c, obj)) {
        int flags = gi(io + IOS_FLAGS);
        char prefix[3] = {0, 0, 0};
        if ((flags & F_SHOWPOS) && d > 0)
            prefix[0] = '+';
        char fmt[8] = "%.*f";
        if ((flags & (F_SCIENTIFIC | F_FIXED)) == F_SCIENTIFIC)
            fmt[3] = (flags & F_UPPERCASE) ? 'E' : 'e';
        else if ((flags & (F_SCIENTIFIC | F_FIXED)) != F_FIXED)
            fmt[3] = (flags & F_UPPERCASE) ? 'G' : 'g';
        int prec = gi(io + IOS_PRECISION);
        if (prec < 0 || prec > 15)
            prec = 15;
        char buf[64];
        snprintf(buf, sizeof buf, fmt, prec, d);
        ostream_writepad_str(c, obj, prefix, buf);
        ostream_osfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_ostream_print_int_H(X86 *c) { // identical to << (int)
    ms_ostream_print_int(c);
}
void ms_endl(X86 *c) {
    uint32_t obj = arg(c, 0); // cdecl: the ostream& is a stack argument
    uint32_t io = ios_of(obj);
    if (ostream_opfx(c, obj)) {
        if (sb_sputc(c, sb_of_ios(io), '\n') == VEOF)
            ios_setstate(io, ST_BAD | ST_FAIL);
        ostream_osfx(c, obj);
    }
    ostream_flush(c, obj);
    set_eax(c, obj);
}

// ---------------------------------------------------------------------------
// Imported shims - istream.
// ---------------------------------------------------------------------------
void ms_istream_get(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    int ret = VEOF;
    if (istream_ipfx(c, obj, 1)) {
        if ((ret = sb_sbumpc(c, sb_of_ios(io))) != VEOF)
            wi(obj + IST_COUNT, 1);
        else {
            ios_orstate(io, ST_EOF);
            ios_orstate(io, ST_FAIL);
        }
        istream_isfx(c, obj);
    }
    set_eax(c, (uint32_t)ret);
}
void ms_istream_peek(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    int ret = VEOF;
    if (istream_ipfx(c, obj, 1)) {
        ret = sb_sgetc(c, sb_of_ios(ios_of(obj)));
        istream_isfx(c, obj);
    }
    set_eax(c, (uint32_t)ret);
}
void ms_istream_putback(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    if (ios_good(io)) {
        if (sb_sputbackc(c, sb_of_ios(io), (int32_t)arg(c, 0)) == VEOF)
            ios_orstate(io, ST_FAIL);
    }
    set_eax(c, obj);
}
void ms_istream_read(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint32_t buf = arg(c, 0);
    int count = (int32_t)arg(c, 1);
    if (istream_ipfx(c, obj, 1)) {
        int got = sb_sgetn(c, sb_of_ios(io), buf, count);
        wi(obj + IST_COUNT, got);
        if (got != count)
            ios_setstate(io, ST_EOF | ST_FAIL);
        istream_isfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_istream_read_char(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint32_t p = arg(c, 0);
    if (istream_ipfx(c, obj, 0)) {
        int ret = sb_sbumpc(c, sb_of_ios(io));
        if (ret == VEOF)
            ios_orstate(io, ST_EOF | ST_FAIL);
        else
            wr8(p, (uint8_t)ret);
        istream_isfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_istream_read_str(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    uint32_t str = arg(c, 0);
    int count = 0;
    if (istream_ipfx(c, obj, 0)) {
        int ch;
        uint32_t width = (uint32_t)gi(io + IOS_WIDTH);
        for (ch = sb_sgetc(c, sb); (uint32_t)count < width - 1 && ch != VEOF && !isspace(ch);
             ch = sb_snextc(c, sb)) {
            wr8(str + (uint32_t)count, (uint8_t)ch);
            ++count;
        }
        if (ch == VEOF)
            ios_orstate(io, ST_EOF);
        if (!count)
            ios_orstate(io, ST_FAIL);
        else
            wr8(str + (uint32_t)count, 0);
        wi(io + IOS_WIDTH, 0);
        istream_isfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_istream_read_int(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    int32_t v = 0;
    if (istream_ipfx(c, obj, 0)) {
        istream_read_integer(c, obj, INT32_MIN, INT32_MAX, false, &v);
        istream_isfx(c, obj);
    }
    wr32(arg(c, 0), (uint32_t)v);
    set_eax(c, obj);
}
void ms_istream_read_long(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    int32_t v = 0;
    if (istream_ipfx(c, obj, 0)) {
        istream_read_integer(c, obj, INT32_MIN, INT32_MAX, true, &v);
        istream_isfx(c, obj);
    }
    wr32(arg(c, 0), (uint32_t)v);
    set_eax(c, obj);
}
void ms_istream_read_double(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    double d = 0;
    istream_read_float(c, obj, 28, &d);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    wr32(arg(c, 0), (uint32_t)bits);
    wr32(arg(c, 0) + 4, (uint32_t)(bits >> 32));
    set_eax(c, obj);
}
void ms_istream_read_manip(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t fn = arg(c, 0);
    set_eax(c, guest_call(c, fn, obj));
}
void ms_istream_get_sb(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    uint32_t dst = arg(c, 0);
    int delim = (int32_t)arg(c, 1);
    if (istream_ipfx(c, obj, 1)) {
        int ch;
        for (ch = sb_sgetc(c, sb); ch != delim; ch = sb_snextc(c, sb)) {
            if (ch == VEOF) {
                ios_orstate(io, ST_EOF);
                break;
            }
            if (sb_sputc(c, dst, ch) == VEOF)
                ios_orstate(io, ST_FAIL);
            wi(obj + IST_COUNT, gi(obj + IST_COUNT) + 1);
        }
        istream_isfx(c, obj);
    }
    set_eax(c, obj);
}
void ms_istream_get_str_delim(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t io = ios_of(obj);
    uint32_t sb = sb_of_ios(io);
    uint32_t str = arg(c, 0);
    int count = (int32_t)arg(c, 1);
    int delim = (int32_t)arg(c, 2);
    int ch, i = 0;
    if (istream_ipfx(c, obj, 1)) {
        while (i < count - 1) {
            if ((ch = sb_sgetc(c, sb)) == VEOF) {
                ios_orstate(io, ST_EOF);
                if (!i)
                    ios_orstate(io, ST_FAIL);
                break;
            }
            if (ch == delim) {
                if (gi(obj + IST_EXTRACT)) {
                    sb_stossc(c, sb);
                    wi(obj + IST_COUNT, gi(obj + IST_COUNT) + 1);
                }
                break;
            }
            if (str)
                wr8(str + (uint32_t)i, (uint8_t)ch);
            sb_stossc(c, sb);
            ++i;
        }
        wi(obj + IST_COUNT, gi(obj + IST_COUNT) + i);
        istream_isfx(c, obj);
    }
    if (str && count)
        wr8(str + (uint32_t)i, 0);
    wi(obj + IST_EXTRACT, 0);
    set_eax(c, obj);
}
void ms_ws(X86 *c) {
    uint32_t obj = arg(c, 0);
    istream_eatwhite(c, obj);
    set_eax(c, obj);
}
void ms_ios_unlock(X86 *c) {
    ios_unlockc(arg(c, 0));
    set_eax(c, 0);
}
void ms_ifstream_close(X86 *c) {
    uint32_t obj = c->r[R_ECX];
    uint32_t fb = sb_of_ios(ios_of(obj));
    if (!filebuf_close(c, fb))
        ios_orstate(ios_of(obj), ST_FAIL);
    else
        ios_setstate(ios_of(obj), ST_GOOD);
    set_eax(c, 0);
}
void ms_ofstream_close(X86 *c) {
    ms_ifstream_close(c);
}
void ms_mtlock(X86 *) {
    // No-op: the runtime owns a single cooperative guest thread.
}
void ms_mtunlock(X86 *) {}

// Internal vector-deleting destructors (registered so guest vtables can point
// at a trampoline for them). ECX is the ios sub-object for the class families
// whose vfptr lives in the virtual base, the streambuf itself otherwise.
void vt_streambuf_vec_dtor(X86 *c) {
    uint32_t s = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    streambuf_dtor_impl(s);
    if (flags & 1)
        heap_free(s);
    set_eax(c, s);
}
void vt_filebuf_vec_dtor(X86 *c) {
    uint32_t fb = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    vt_filebuf_vec_dtor_h(c, fb, flags);
    if (flags & 1)
        heap_free(fb);
    set_eax(c, fb);
}
void vt_strstreambuf_vec_dtor(X86 *c) {
    uint32_t s = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    strstreambuf_dtor_impl(s);
    if (flags & 1)
        heap_free(s);
    set_eax(c, s);
}
void vt_ios_vec_dtor(X86 *c) {
    uint32_t io = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    ios_dtor_impl(c, io);
    if (flags & 1)
        heap_free(io);
    set_eax(c, io);
}
void vt_ostream_vec_dtor(X86 *c) {
    uint32_t io = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    ostream_vbase_dtor_impl(c, io - OST_IOS);
    if (flags & 1)
        heap_free(io - OST_IOS);
    set_eax(c, io - OST_IOS);
}
void vt_istream_vec_dtor(X86 *c) {
    uint32_t io = c->r[R_ECX];
    uint32_t flags = arg(c, 0);
    istream_vbase_dtor_impl(c, io - IST_IOS);
    if (flags & 1)
        heap_free(io - IST_IOS);
    set_eax(c, io - IST_IOS);
}
void vt_streambuf_sync(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_sync_h(c, c->r[R_ECX]));
}
void vt_streambuf_overflow(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_overflow_h(c, c->r[R_ECX], (int32_t)arg(c, 0)));
}
void vt_streambuf_underflow(X86 *c) {
    set_eax(c, (uint32_t)vt_streambuf_underflow_h(c, c->r[R_ECX]));
}
void vt_filebuf_sync(X86 *c) {
    set_eax(c, (uint32_t)vt_filebuf_sync_h(c, c->r[R_ECX]));
}
void vt_filebuf_overflow(X86 *c) {
    set_eax(c, (uint32_t)vt_filebuf_overflow_h(c, c->r[R_ECX], (int32_t)arg(c, 0)));
}
void vt_filebuf_underflow(X86 *c) {
    set_eax(c, (uint32_t)vt_filebuf_underflow_h(c, c->r[R_ECX]));
}
void vt_filebuf_setbuf(X86 *c) {
    set_eax(c, vt_filebuf_setbuf_h(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1)));
}
void vt_filebuf_seekoff(X86 *c) {
    set_eax(c, (uint32_t)vt_filebuf_seekoff_h(c, c->r[R_ECX], (int32_t)arg(c, 0),
                                              (int32_t)arg(c, 1), (int32_t)arg(c, 2)));
}
void vt_strstreambuf_sync(X86 *c) {
    set_eax(c, (uint32_t)vt_strstreambuf_sync_h(c, c->r[R_ECX]));
}
void vt_strstreambuf_overflow(X86 *c) {
    set_eax(c, (uint32_t)vt_strstreambuf_overflow_h(c, c->r[R_ECX], (int32_t)arg(c, 0)));
}
void vt_strstreambuf_underflow(X86 *c) {
    set_eax(c, (uint32_t)vt_strstreambuf_underflow_h(c, c->r[R_ECX]));
}
void vt_strstreambuf_setbuf(X86 *c) {
    set_eax(c, vt_strstreambuf_setbuf_h(c, c->r[R_ECX], arg(c, 0), (int32_t)arg(c, 1)));
}
void vt_strstreambuf_seekoff(X86 *c) {
    set_eax(c, (uint32_t)vt_strstreambuf_seekoff_h(c, c->r[R_ECX], (int32_t)arg(c, 0),
                                                   (int32_t)arg(c, 1), (int32_t)arg(c, 2)));
}
void vt_strstreambuf_doallocate(X86 *c) {
    set_eax(c, strstreambuf_doallocate_impl(c, c->r[R_ECX]));
}

// ---------------------------------------------------------------------------
// Shim table.
// ---------------------------------------------------------------------------
#define TC(name, argc, fn) {"MSVCIRT.dll", name, argc, fn}
#define CD(name, fn) {"MSVCIRT.dll", name, ARGC_CDECL, fn}

const ImportShim shims[] = {
    // Imported by Petz 5.exe (55 names).
    TC("?xsgetn@streambuf@@UAEHPADH@Z", 2, ms_xsgetn),
    TC("??0ostream@@QAE@PAVstreambuf@@@Z", 2, ms_ostream_sb_ctor),
    TC("??1streambuf@@UAE@XZ", 0, ms_streambuf_dtor),
    TC("??1ostream@@UAE@XZ", 0, ms_ostream_dtor),
    TC("?rdbuf@ostrstream@@QBEPAVstrstreambuf@@XZ", 0, ms_ostrstream_rdbuf),
    TC("??5istream@@QAEAAV0@AAJ@Z", 1, ms_istream_read_long),
    TC("??6ostream@@QAEAAV0@J@Z", 1, ms_ostream_print_int),
    TC("?get@istream@@QAEAAV1@AAVstreambuf@@D@Z", 2, ms_istream_get_sb),
    CD("_mtunlock", ms_mtunlock),
    TC("?put@ostream@@QAEAAV1@E@Z", 1, ms_ostream_put),
    TC("?str@strstreambuf@@QAEPADXZ", 0, ms_strstreambuf_str),
    TC("??1ostrstream@@UAE@XZ", 0, ms_ostream_dtor),
    TC("??0ostrstream@@QAE@XZ", 1, ms_ostrstream_ctor),
    TC("?freeze@strstreambuf@@QAEXH@Z", 1, ms_strstreambuf_freeze),
    TC("?get@istream@@QAEHXZ", 0, ms_istream_get),
    TC("?setmode@filebuf@@QAEHH@Z", 1, ms_filebuf_setmode),
    TC("?flush@ostream@@QAEAAV1@XZ", 0, ms_ostream_flush),
    TC("??6ostream@@QAEAAV0@H@Z", 1, ms_ostream_print_int_H),
    TC("??6ostream@@QAEAAV0@N@Z", 2, ms_ostream_print_double),
    TC("??5istream@@QAEAAV0@AAN@Z", 1, ms_istream_read_double),
    TC("??6ostream@@QAEAAV0@PBD@Z", 1, ms_ostream_print_str),
    CD("?endl@@YAAAVostream@@AAV1@@Z", ms_endl),
    TC("??6ostream@@QAEAAV0@E@Z", 1, ms_ostream_print_char),
    TC("?pbackfail@streambuf@@UAEHH@Z", 1, ms_streambuf_pbackfail),
    TC("?doallocate@streambuf@@MAEHXZ", 0, ms_streambuf_doallocate),
    TC("?putback@istream@@QAEAAV1@D@Z", 1, ms_istream_putback),
    TC("??5istream@@QAEAAV0@AAH@Z", 1, ms_istream_read_int),
    TC("??0ios@@IAE@XZ", 0, ms_ios_ctor),
    TC("??0istrstream@@QAE@PAD@Z", 2, ms_istrstream_str_ctor),
    CD("?ws@@YAAAVistream@@AAV1@@Z", ms_ws),
    TC("??5istream@@QAEAAV0@PAD@Z", 1, ms_istream_read_str),
    TC("??5istream@@QAEAAV0@AAD@Z", 1, ms_istream_read_char),
    CD("_mtlock", ms_mtlock),
    TC("?get@istream@@IAEAAV1@PADHH@Z", 3, ms_istream_get_str_delim),
    CD("?unlock@ios@@QAAXXZ", ms_ios_unlock),
    TC("??1istrstream@@UAE@XZ", 0, ms_istream_dtor),
    // ?openprot@filebuf@@2HB is data, registered below.
    TC("??0ifstream@@QAE@PBDHH@Z", 4, ms_ifstream_open_ctor),
    TC("?close@ifstream@@QAEXXZ", 0, ms_ifstream_close),
    TC("??0ofstream@@QAE@PBDHH@Z", 4, ms_ofstream_open_ctor),
    TC("?close@ofstream@@QAEXXZ", 0, ms_ofstream_close),
    TC("??1ofstream@@UAE@XZ", 0, ms_ostream_dtor),
    TC("??1ios@@UAE@XZ", 0, ms_ios_dtor),
    TC("??1ifstream@@UAE@XZ", 0, ms_istream_dtor),
    TC("??_Difstream@@QAEXXZ", 0, ms_ifstream_vbase_dtor),
    TC("??_Dofstream@@QAEXXZ", 0, ms_ofstream_vbase_dtor),
    TC("?read@istream@@QAEAAV1@PADH@Z", 2, ms_istream_read),
    TC("?write@ostream@@QAEAAV1@PBDH@Z", 2, ms_ostream_write),
    TC("?xsputn@streambuf@@UAEHPBDH@Z", 2, ms_xsputn),
    TC("?seekpos@streambuf@@UAEJJH@Z", 2, ms_streambuf_seekpos),
    TC("?seekoff@streambuf@@UAEJJW4seek_dir@ios@@H@Z", 3, ms_streambuf_seekoff),
    TC("??5istream@@QAEAAV0@P6AAAV0@AAV0@@Z@Z", 1, ms_istream_read_manip),
    TC("?peek@istream@@QAEHXZ", 0, ms_istream_peek),
    TC("?setbuf@streambuf@@UAEPAV1@PADH@Z", 2, ms_streambuf_setbuf),
    TC("??0streambuf@@IAE@XZ", 0, ms_streambuf_ctor),

    // Virtual base implementations the guest vtables point at but the exe does
    // not import by name.
    TC("??_Estreambuf@@UAEPAXI@Z", 1, vt_streambuf_vec_dtor),
    TC("?sync@streambuf@@UAEHXZ", 0, vt_streambuf_sync),
    TC("?overflow@streambuf@@UAEHH@Z", 1, vt_streambuf_overflow),
    TC("?underflow@streambuf@@UAEHXZ", 0, vt_streambuf_underflow),
    TC("??_Efilebuf@@UAEPAXI@Z", 1, vt_filebuf_vec_dtor),
    TC("?sync@filebuf@@UAEHXZ", 0, vt_filebuf_sync),
    TC("?setbuf@filebuf@@UAEPAVstreambuf@@PADH@Z", 2, vt_filebuf_setbuf),
    TC("?seekoff@filebuf@@UAEJJW4seek_dir@ios@@H@Z", 3, vt_filebuf_seekoff),
    TC("?overflow@filebuf@@UAEHH@Z", 1, vt_filebuf_overflow),
    TC("?underflow@filebuf@@UAEHXZ", 0, vt_filebuf_underflow),
    TC("??_Estrstreambuf@@UAEPAXI@Z", 1, vt_strstreambuf_vec_dtor),
    TC("?sync@strstreambuf@@UAEHXZ", 0, vt_strstreambuf_sync),
    TC("?setbuf@strstreambuf@@UAEPAVstreambuf@@PADH@Z", 2, vt_strstreambuf_setbuf),
    TC("?seekoff@strstreambuf@@UAEJJW4seek_dir@ios@@H@Z", 3, vt_strstreambuf_seekoff),
    TC("?overflow@strstreambuf@@UAEHH@Z", 1, vt_strstreambuf_overflow),
    TC("?underflow@strstreambuf@@UAEHXZ", 0, vt_strstreambuf_underflow),
    TC("?doallocate@strstreambuf@@MAEHXZ", 0, vt_strstreambuf_doallocate),
    TC("??_Eios@@UAEPAXI@Z", 1, vt_ios_vec_dtor),
    TC("??_Eostream@@UAEPAXI@Z", 1, vt_ostream_vec_dtor),
    TC("??_Eistream@@UAEPAXI@Z", 1, vt_istream_vec_dtor),
    TC("??_Eostrstream@@UAEPAXI@Z", 1, vt_ostream_vec_dtor),
    TC("??_Eofstream@@UAEPAXI@Z", 1, vt_ostream_vec_dtor),
    TC("??_Eistrstream@@UAEPAXI@Z", 1, vt_istream_vec_dtor),
    TC("??_Eifstream@@UAEPAXI@Z", 1, vt_istream_vec_dtor),
};

} // namespace

void msvcirt_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    // filebuf::openprot is a static const int (0x1a4 = 420); the guest reads it
    // through its IAT slot before constructing an ofstream/ifstream.
    imports_register_data("MSVCIRT.dll", "?openprot@filebuf@@2HB", 4);
    uint32_t p = imports_alloc_data("MSVCIRT.dll", "?openprot@filebuf@@2HB");
    if (p)
        wr32(p, 420);
}
