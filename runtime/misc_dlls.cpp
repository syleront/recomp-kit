// Small DLL probes for a runtime with no printer, workstation service or tray.
#include "imports.h"
#include "user32_internal.h"
#include "win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

namespace {
void zero_out(uint32_t p) {
    if (p && gm_valid(p, 4))
        wr32(p, 0);
}
void yes(X86 *c) {
    set_eax(c, 1);
}
void zero(X86 *c) {
    set_eax(c, 0);
}
void session_service_unavailable(X86 *c) {
    // No Remote Desktop Services endpoint is running in the guest runtime.
    // Expose the exports so callers receive the service error as a BOOL result.
    set_last_error(1702); // RPC_S_INVALID_BINDING
    set_eax(c, 0);
}
void buffered_paint_unavailable(X86 *c) {
    set_eax(c, 0x80004001u); // E_NOTIMPL: use the caller's ordinary GDI path.
}
// Visual styles are on, as on every Windows since Vista, but there is no theme
// data: OpenThemeData finds none, so a program draws its controls the classic
// way, and the per-part queries fail with E_HANDLE as they do for a NULL theme.
// A VCL program binds every export at start and calls through the pointers,
// so each one is here. DrawThemeParentBackground is the one that does work.
void e_handle(X86 *c) {
    set_eax(c, 0x80070006u); // E_HANDLE
}
void theme_app_properties(X86 *c) {
    set_eax(c, 7); // STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS | STAP_ALLOW_WEBCONTENT
}
// With no theme, the system metric queries answer from the system settings.
void theme_sys_value(X86 *c, const char *user32_export) {
    set_eax(c, guest_call(c, imports_resolve("USER32.dll", user32_export), arg(c, 1)));
}
void theme_sys_color(X86 *c) {
    theme_sys_value(c, "GetSysColor");
}
void theme_sys_color_brush(X86 *c) {
    theme_sys_value(c, "GetSysColorBrush");
}
void theme_sys_size(X86 *c) {
    theme_sys_value(c, "GetSystemMetrics");
}
// With no theme data, themed text is drawn the classic way: the DC's font, the
// colour DTTOPTS names if it names one, and no background, as themed text never
// has one. Windows answers a NULL theme with E_HANDLE, but a VCL program with
// styles on draws its captions through these calls, and would lose them all.
uint32_t theme_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t flags, uint32_t rect,
                    uint32_t options) {
    auto *dc = gdi::dc_of(hdc);
    if (!dc || !text || !rect)
        return 0x80070057u; // E_INVALIDARG
    const uint32_t option_flags = options && gm_valid(options, 12) ? rd32(options + 4) : 0;
    const uint32_t old_color = dc->text_color;
    const int old_mode = dc->bk_mode;
    if (option_flags & 0x1) // DTT_TEXTCOLOR
        dc->text_color = rd32(options + 8);
    if (option_flags & 0x200) // DTT_CALCRECT
        flags |= 0x400;       // DT_CALCRECT
    dc->bk_mode = 1;          // TRANSPARENT
    gdi::draw_text(hdc, text, count, rect, flags);
    if (auto *after = gdi::dc_of(hdc)) {
        after->text_color = old_color;
        after->bk_mode = old_mode;
    }
    return 0;
}
// DrawThemeText(hTheme, hdc, part, state, text, count, flags, flags2, rect)
void draw_theme_text(X86 *c) {
    set_eax(c, theme_text(arg(c, 1), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 8), 0));
}
// DrawThemeTextEx(hTheme, hdc, part, state, text, count, flags, rect, options)
void draw_theme_text_ex(X86 *c) {
    set_eax(c, theme_text(arg(c, 1), arg(c, 4), arg(c, 5), arg(c, 6), arg(c, 7), arg(c, 8)));
}
// No paint buffer and no animation: the caller paints its target directly.
void begin_buffered_paint(X86 *c) {
    if (recomp_env("TRACE_GDI")) {
        const uint32_t rc = arg(c, 1);
        LOGW("gdi: BeginBufferedPaint target=%08x rect=%d,%d,%d,%d format=%u", arg(c, 0),
             rc && gm_valid(rc, 16) ? (int)rd32(rc) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 4) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 8) : -1,
             rc && gm_valid(rc, 16) ? (int)rd32(rc + 12) : -1, arg(c, 2));
    }
    zero_out(arg(c, 4)); // *phdc
    set_eax(c, 0);
}
void begin_buffered_animation(X86 *c) {
    zero_out(arg(c, 6)); // *phdcFrom
    zero_out(arg(c, 7)); // *phdcTo
    set_eax(c, 0);
}
// DrawThemeParentBackground(hwnd, hdc, prc): the parent paints its background
// into the child's DC, the DC's origin moved so the parent's coordinates land
// on the child, through WM_ERASEBKGND and then WM_PRINTCLIENT. A transparent
// control - the VCL's TStaticText - draws its text over what that leaves.
void draw_parent_background(X86 *c) {
    uint32_t hwnd = arg(c, 0), hdc = arg(c, 1), rect = arg(c, 2);
    auto *w = user32::find_window(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!w || !dc) {
        set_eax(c, 0x80070006u); // E_HANDLE
        return;
    }
    const uint32_t parent = w->parent;
    if (!parent) {
        set_eax(c, 0);
        return;
    }
    int32_t cx = 0, cy = 0, px = 0, py = 0;
    user32::client_origin(hwnd, &cx, &cy);
    user32::client_origin(parent, &px, &py);
    const gdi::DcState saved = *dc;
    if (rect && gm_valid(rect, 16)) { // prc limits the paint, in the child's coordinates
        gdi::Rect cut = gdi::to_device(hdc, {int32_t(rd32(rect)), int32_t(rd32(rect + 4)),
                                             int32_t(rd32(rect + 8)), int32_t(rd32(rect + 12))});
        std::vector<gdi::Rect> clip;
        for (gdi::Rect r : dc->clipped ? dc->clip : std::vector<gdi::Rect>{cut}) {
            gdi::Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                          std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                clip.push_back(hit);
        }
        dc->clip = std::move(clip);
        dc->clipped = true;
    }
    dc->org_x += cx - px;
    dc->org_y += cy - py;
    host_dispatch_to_wndproc(c, parent, 0x0014 /* WM_ERASEBKGND */, hdc, 0);
    host_dispatch_to_wndproc(c, parent, 0x0318 /* WM_PRINTCLIENT */, hdc, 4 /* PRF_CLIENT */);
    if (auto *after = gdi::dc_of(hdc)) // the window procedures may have changed it
        static_cast<gdi::DcState &>(*after) = saved;
    set_eax(c, 0);
}
void composition_enabled(X86 *c) {
    uint32_t out = arg(c, 0);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, 0x80070057u); // E_INVALIDARG
        return;
    }
    wr32(out, 0); // No desktop composition or glass frame in the guest model.
    set_eax(c, 0);
}
void enum_printers(X86 *c) {
    zero_out(arg(c, 5));
    zero_out(arg(c, 6));
    set_eax(c, 1);
}
void default_printer(X86 *c) {
    zero_out(arg(c, 1));
    set_last_error(2);
    set_eax(c, 0);
}
void open_printer(X86 *c) {
    zero_out(arg(c, 1));
    set_last_error(1801); // ERROR_INVALID_PRINTER_NAME
    set_eax(c, 0);
}
void document_properties(X86 *c) {
    set_eax(c, 0xffffffffu);
}
// The Win32 prototype has three arguments, not four: server, level, buffer.
void workstation_info(X86 *c) {
    zero_out(arg(c, 2));
    set_eax(c, 50);
}
void crt_memcpy(X86 *c) {
    uint32_t dst = arg(c, 0), src = arg(c, 1), size = arg(c, 2);
    if (gm_valid(dst, size) && gm_valid(src, size) && size)
        memcpy(g_mem + dst, g_mem + src, size);
    set_eax(c, dst);
}
void crt_memset(X86 *c) {
    uint32_t dst = arg(c, 0), size = arg(c, 2);
    if (gm_valid(dst, size) && size)
        memset(g_mem + dst, int(arg(c, 1)), size);
    set_eax(c, dst);
}
// All supported per-user settings locations use the same Documents guest path;
// the file overlay decides where that path resides in the writable profile.
void folder_path(X86 *c) {
    uint32_t csidl = arg(c, 1), out = arg(c, 4);
    switch (csidl & 0xff) {
    case 5:
    case 26:
    case 28:
    case 35:
        break;
    default:
        set_eax(c, 0x80070057u);
        return;
    }
    if (!out || !gm_valid(out, 520)) {
        set_eax(c, 0x80070057u);
        return;
    }
    gm_put_wstr(out, shell_folder_guest_path(5, (csidl & 0x8000) != 0), 260);
    set_eax(c, 0);
}
// ---------------------------------------------------------------------------
// comdlg32: the common file dialogs, mapped onto the platform's own picker
// (the macOS host's NSSavePanel/NSOpenPanel) through os_file_dialog(). The
// Win32 OPENFILENAMEA is split into the plain request the seam takes - title,
// filter pairs, initial directory and name, flags - and the chosen path is
// written back into lpstrFile within nMaxFile with nFileOffset/nFileExtension
// set as Windows sets them. A cancel is FALSE with CommDlgExtendedError() == 0,
// exactly as Windows leaves it; a path that does not fit is FALSE with
// FNERR_BUFFERTOOSMALL.
// ---------------------------------------------------------------------------
uint32_t g_commdlg_error = 0;

// The Win32 filter string is pairs of NUL-terminated strings ending with a
// double NUL: description\0pattern\0...\0\0. The pattern is a
// semicolon-separated list of wildcards. Split it into the plain form the
// platform seam takes; malformed input stops at the first empty description.
int parse_ofn_filter(uint32_t lpstrFilter, OsFileFilter *out, char descs[][128], char pats[][256],
                     int max) {
    if (!lpstrFilter || !gm_valid(lpstrFilter, 1))
        return 0;
    int count = 0;
    uint32_t p = lpstrFilter;
    const uint32_t limit = lpstrFilter + 4096;
    while (count < max && p < limit) {
        std::string desc = gm_str(p, 128);
        if (desc.empty())
            break; // the double NUL that ends the list
        p += (uint32_t)desc.size() + 1;
        std::string pattern = gm_str(p, 256);
        p += (uint32_t)pattern.size() + 1;
        snprintf(descs[count], 128, "%s", desc.c_str());
        snprintf(pats[count], 256, "%s", pattern.c_str());
        out[count].description = descs[count];
        out[count].patterns = pats[count];
        ++count;
    }
    return count;
}

void common_dialog(X86 *c, bool save) {
    const char *kind = save ? "GetSaveFileNameA" : "GetOpenFileNameA";
    uint32_t ofn = arg(c, 0);
    g_commdlg_error = 0;
    if (!ofn || !gm_valid(ofn, 76)) {
        g_commdlg_error = 0x0002; // CDERR_INITIALIZATION
        LOGW("%s: no OPENFILENAMEA, reporting initialization failure", kind);
        set_eax(c, 0);
        return;
    }
    uint32_t size = rd32(ofn + 0);
    if (size < 76) {
        g_commdlg_error = 0x0001; // CDERR_STRUCTSIZE
        LOGW("%s: lStructSize %u is too small", kind, size);
        set_eax(c, 0);
        return;
    }
    uint32_t lpstrFilter = rd32(ofn + 12);
    uint32_t lpstrFile = rd32(ofn + 28);
    uint32_t nMaxFile = rd32(ofn + 32);
    uint32_t lpstrInitialDir = rd32(ofn + 44);
    uint32_t lpstrTitle = rd32(ofn + 48);
    uint32_t flags = rd32(ofn + 52);
    uint32_t lpstrDefExt = rd32(ofn + 60);
    if (!lpstrFile || nMaxFile == 0 || !gm_valid(lpstrFile, nMaxFile)) {
        g_commdlg_error = 0x0002; // CDERR_INITIALIZATION
        LOGW("%s: no writable lpstrFile buffer (nMaxFile %u)", kind, nMaxFile);
        set_eax(c, 0);
        return;
    }
    std::string initial = gm_str(lpstrFile, nMaxFile);
    std::string title = lpstrTitle ? gm_str(lpstrTitle, 512) : std::string();
    std::string dir = lpstrInitialDir ? gm_str(lpstrInitialDir, 520) : std::string();
    std::string ext = lpstrDefExt ? gm_str(lpstrDefExt, 32) : std::string();

    OsFileFilter filters[8];
    char descs[8][128], pats[8][256];
    int filter_count = parse_ofn_filter(lpstrFilter, filters, descs, pats, 8);

    std::vector<char> buf((size_t)nMaxFile, 0);
    if (!initial.empty())
        memcpy(buf.data(), initial.c_str(), std::min(initial.size(), buf.size() - 1));

    OsFileDialog d{};
    d.save = save ? 1 : 0;
    d.title = title.empty() ? nullptr : title.c_str();
    d.initial_dir = dir.empty() ? nullptr : dir.c_str();
    d.default_ext = ext.empty() ? nullptr : ext.c_str();
    d.file = buf.data();
    d.file_cap = nMaxFile;
    d.filters = filters;
    d.filter_count = filter_count;
    d.filter_index = (int)rd32(ofn + 24);
    d.flags = flags;
    int file_offset = 0, file_extension = 0;
    d.file_offset = &file_offset;
    d.file_extension = &file_extension;

    LOGW("%s: title \"%s\", initial \"%s\", %d filter(s), flags 0x%x", kind, title.c_str(),
         initial.c_str(), filter_count, flags);
    int r = os_file_dialog(&d);
    if (r == 1 || r == -2) {
        size_t n = 0;
        while (n + 1 < buf.size() && buf[n])
            ++n;
        // A path the platform chose is a real host path. Present it to the
        // guest as a drive-style Windows path (Z: is the host root) and tell
        // the overlay, so the guest can hand it straight back to CopyFileA/
        // CreateFileA and it reaches the host unchanged instead of being
        // re-rooted under the writable root (TD-098). A truncated path (r==-2)
        // is left as the platform wrote it; the caller only sees the error.
        if (r == 1) {
            std::string chosen(buf.data(), n);
            win32_grant_host_path(chosen); // a no-op unless it is absolute
            std::string guest = win32_guest_path_for_host(chosen);
            if (!guest.empty() && guest.size() + 1 <= buf.size()) {
                memcpy(buf.data(), guest.c_str(), guest.size() + 1);
                n = guest.size();
                size_t base = 0, dot = 0;
                for (size_t i = 0; i < n; ++i)
                    if (guest[i] == '/' || guest[i] == '\\')
                        base = i + 1;
                for (size_t i = base; i < n; ++i)
                    if (guest[i] == '.')
                        dot = i;
                file_offset = (int)base;
                file_extension = (int)dot;
            }
        }
        memcpy(g_mem + lpstrFile, buf.data(), n + 1);
        wr16(ofn + 56, (uint16_t)file_offset);
        wr16(ofn + 58, (uint16_t)file_extension);
        wr32(ofn + 24, (uint32_t)d.filter_index);
        if (r == -2) {
            g_commdlg_error = 0x3003; // FNERR_BUFFERTOOSMALL
            LOGW("%s: the chosen path does not fit nMaxFile %u", kind, nMaxFile);
            set_eax(c, 0);
            return;
        }
        LOGW("%s: chose \"%s\"", kind, buf.data());
        set_eax(c, 1); // TRUE
        return;
    }
    g_commdlg_error = 0; // a cancel, exactly as Windows leaves it
    LOGW("%s: cancelled", kind);
    set_eax(c, 0); // FALSE
}
void cd_GetOpenFileNameA(X86 *c) {
    common_dialog(c, false);
}
void cd_GetSaveFileNameA(X86 *c) {
    common_dialog(c, true);
}
void cd_ExtendedError(X86 *c) {
    set_eax(c, g_commdlg_error);
}

// ---------------------------------------------------------------------------
// HTML Help (hhctrl.ocx). The engine does not exist here; the platform seam
// logs the request and never fails the caller. HtmlHelpA/W return an HWND,
// which is NULL for a request that could not be shown, exactly as it is when
// the engine is missing on Windows.
// ---------------------------------------------------------------------------
void html_help(X86 *c, bool wide) {
    uint32_t file = arg(c, 1);
    std::string target = wide ? gm_wstr(file, 1024) : gm_str(file, 1024);
    os_help("HtmlHelpA", target.c_str(), nullptr);
    set_eax(c, 0); // NULL HWND
}
void hh_HtmlHelpA(X86 *c) {
    html_help(c, false);
}
void hh_HtmlHelpW(X86 *c) {
    html_help(c, true);
}

// ---------------------------------------------------------------------------
// RPCRT4: GUID generation and comparison. UuidCreate fills a version-4 UUID
// from a process-local xorshift seeded from the wall clock and the monotonic
// clock; UuidEqual is a byte comparison that always reports RPC_S_OK.
// ---------------------------------------------------------------------------
uint32_t g_uuid_rng = 0;
uint32_t uuid_rng_next() {
    if (!g_uuid_rng)
        g_uuid_rng = (uint32_t)os_wall_time_us() ^ (uint32_t)(os_monotonic_ns() >> 4) ^ 0x9e3779b9u;
    uint32_t x = g_uuid_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_uuid_rng = x;
    return x;
}
void r_UuidCreate(X86 *c) {
    uint32_t out = arg(c, 0);
    if (out && gm_valid(out, 16)) {
        for (int i = 0; i < 4; ++i)
            wr32(out + i * 4, uuid_rng_next());
        wr8(out + 6, (uint8_t)((rd8(out + 6) & 0x0f) | 0x40)); // version 4
        wr8(out + 8, (uint8_t)((rd8(out + 8) & 0x3f) | 0x80)); // RFC 4122 variant
    }
    set_eax(c, 0); // RPC_S_OK
}
void r_UuidEqual(X86 *c) {
    uint32_t a = arg(c, 0), b = arg(c, 1), status = arg(c, 2);
    bool valid = a && b && gm_valid(a, 16) && gm_valid(b, 16);
    bool equal = valid && memcmp(g_mem + a, g_mem + b, 16) == 0;
    if (status && gm_valid(status, 4))
        wr32(status, 0); // RPC_S_OK
    set_eax(c, equal ? 1u : 0u);
}

// ---------------------------------------------------------------------------
// MSACM32: audio compression manager. Petz 5 stores its wave effects as mono
// ADPCM and converts them to PCM once at load through CWave::ConvertToPCM;
// nothing else in the game uses ACM. Ten waves are IMA ADPCM
// (WAVE_FORMAT_IMA_ADPCM, format tag 0x11, 4 bits) and five are Microsoft
// ADPCM (WAVE_FORMAT_ADPCM, format tag 2, 4 bits). The streams below implement
// exactly that conversion: IMA ADPCM, MS ADPCM or plain PCM in, 8- or 16-bit
// PCM out, at the same rate and channel count. Any other codec pair is refused
// with ACMERR_NOTPOSSIBLE so the caller takes its documented failure path
// rather than playing garbage. (Before MS ADPCM was handled, the tag-2 waves
// failed ConvertToPCM, and the caller fell back to a compressed DirectSound
// buffer that the sound shim played as if it were PCM - audible noise.)
// ---------------------------------------------------------------------------
// Microsoft ADPCM (WAVE_FORMAT_ADPCM, tag 2): the reference coefficient and
// adaptation tables from the Microsoft Multimedia Programmer's Reference. Five
// of the game's waves (the dh_angry* pet sounds) are this codec.
const int k_ms_coeff1[7] = {256, 512, 0, 192, 240, 460, 392};
const int k_ms_coeff2[7] = {0, -256, 0, 64, 0, -208, -232};
const int k_ms_adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
                            768, 614, 512, 409, 307, 230, 230, 230};
const int k_ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int k_ima_step[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct AcmFormat {
    uint32_t tag = 0, channels = 0, rate = 0, align = 0, bits = 0, samples = 0;
};
struct AcmStream {
    AcmFormat src, dst;
};

std::map<uint32_t, AcmStream> g_acm;
uint32_t g_next_acm = 0x41434d01;

// WAVEFORMATEX: tag(2) channels(2) rate(4) avgBytes(4) align(2) bits(2) cbSize(2)
// and, when cbSize >= 2, the codec's samples-per-block at +18.
bool acm_read_format(uint32_t p, AcmFormat &f) {
    if (!p || !gm_valid(p, 18))
        return false;
    f.tag = rd16(p);
    f.channels = rd16(p + 2);
    f.rate = rd32(p + 4);
    f.align = rd16(p + 12);
    f.bits = rd16(p + 14);
    f.samples = rd16(p + 16) >= 2 && gm_valid(p, 20) ? rd16(p + 18) : 0;
    return f.channels == 1 || f.channels == 2;
}
// What the Microsoft PCM converter on a stock Windows XP does, plus the two
// ADPCM decodes this game's waves use. The PCM converter is a rate/bit-depth/
// channel converter: any 8- or 16-bit PCM source at any rate and 1 or 2
// channels converts to any 8- or 16-bit PCM destination. The ADPCM decoders
// only decode to PCM at the source's own rate and channel count (the codec
// table has no separate resampler), so those keep the tighter test.
bool acm_supported(const AcmStream &s) {
    if (s.dst.tag != 1 || (s.dst.bits != 8 && s.dst.bits != 16) || !s.dst.rate ||
        (s.dst.channels != 1 && s.dst.channels != 2))
        return false;
    if (s.src.tag == 1)
        return (s.src.bits == 8 || s.src.bits == 16) && s.src.rate &&
               (s.src.channels == 1 || s.src.channels == 2);
    // Microsoft ADPCM (tag 2): mono only. The stereo block layout interleaves
    // the two channels' headers differently and no wave in this game uses it,
    // so refusing it (the caller's documented fallback) beats guessing.
    if (s.src.tag == 2)
        return s.src.bits == 4 && s.src.channels == 1 && s.dst.channels == 1 &&
               s.src.rate == s.dst.rate && s.src.align > 7;
    return s.src.tag == 0x11 && s.src.bits == 4 && s.src.channels == s.dst.channels &&
           s.src.rate == s.dst.rate && s.src.align >= 4u * s.src.channels;
}
uint32_t acm_samples_per_block(const AcmFormat &src) {
    if (src.samples)
        return src.samples;
    uint32_t align = src.align ? src.align : 512;
    uint32_t channels = src.channels ? src.channels : 1;
    // MS ADPCM: a seven-byte block header (predictor, delta, two seed samples)
    // then two samples per byte.
    if (src.tag == 2)
        return align > 7 ? (align - 7) * 2 + 2 : 1;
    if (align <= 4 * channels)
        return 1;
    return (align - 4 * channels) * 2 / channels + 1;
}
// One 4-bit IMA nibble at shift 3: the Microsoft reference arithmetic, the same
// expression FFmpeg's adpcm_ima_expand_nibble uses, so a block decoded here
// matches what dx/waveout.cpp's AV_CODEC_ID_ADPCM_IMA_WAV decoder produces.
int ima_nibble(int nibble, int &predictor, int &index) {
    int step = k_ima_step[index];
    int delta = nibble & 7;
    int diff = ((2 * delta + 1) * step) >> 3;
    predictor += (nibble & 8) ? -diff : diff;
    predictor = std::clamp(predictor, -32768, 32767);
    index = std::clamp(index + k_ima_index[nibble], 0, 88);
    return predictor;
}
void pcm_emit(std::vector<uint8_t> &out, int sample, uint32_t bits) {
    if (bits == 8)
        out.push_back((uint8_t)((sample >> 8) + 128)); // 8-bit PCM is unsigned
    else {
        out.push_back((uint8_t)(sample & 0xff));
        out.push_back((uint8_t)((sample >> 8) & 0xff));
    }
}
// One 4-bit MS ADPCM nibble: predict from the two previous samples with the
// block's coefficient pair, add the sign-extended nibble scaled by the running
// delta, clamp, then adapt the delta. Returns the decoded sample.
int ms_nibble(int nibble, int predictor, int &samp1, int &samp2, int &delta) {
    int pred = (samp1 * k_ms_coeff1[predictor] + samp2 * k_ms_coeff2[predictor]) / 256;
    int n = (nibble & 8) ? nibble - 16 : nibble;
    int samp = std::clamp(pred + n * delta, -32768, 32767);
    samp2 = samp1;
    samp1 = samp;
    delta = (k_ms_adapt[nibble] * delta) / 256;
    if (delta < 16)
        delta = 16;
    return samp;
}
// A PCM sample as a signed 16-bit value: 8-bit PCM is unsigned centred on 128.
int read_pcm_sample(const uint8_t *p, uint32_t bits) {
    return bits == 8 ? (int(p[0]) - 128) << 8 : (int)(int16_t)(p[0] | (p[1] << 8));
}
void write_pcm_sample(uint8_t *p, uint32_t bits, int sample) {
    sample = std::clamp(sample, -32768, 32767);
    if (bits == 8) {
        p[0] = (uint8_t)((sample >> 8) + 128);
    } else {
        p[0] = (uint8_t)(sample & 0xff);
        p[1] = (uint8_t)((sample >> 8) & 0xff);
    }
}
void acm_decode(const AcmStream &s, uint32_t src, uint32_t src_bytes, uint32_t dst,
                uint32_t dst_bytes, uint32_t &src_used, uint32_t &dst_used) {
    src_used = dst_used = 0;
    if (s.src.tag == 1) {
        // PCM to PCM. An identical format is the same samples byte for byte;
        // anything else is what the Microsoft PCM converter does: 8/16-bit
        // depth, 1/2 channels and any sample rate, linearly interpolated.
        uint32_t sframe = (s.src.bits / 8) * s.src.channels;
        uint32_t dframe = (s.dst.bits / 8) * s.dst.channels;
        if (!sframe || !dframe)
            return;
        if (s.src.bits == s.dst.bits && s.src.channels == s.dst.channels &&
            s.src.rate == s.dst.rate) {
            uint32_t n = std::min(src_bytes, dst_bytes);
            if (n)
                memcpy(g_mem + dst, g_mem + src, n);
            src_used = n;
            dst_used = n;
            return;
        }
        uint32_t in_frames = src_bytes / sframe; // complete source frames
        std::vector<int16_t> samples((size_t)in_frames * s.src.channels);
        for (uint32_t f = 0; f < in_frames; ++f)
            for (uint32_t ch = 0; ch < s.src.channels; ++ch)
                samples[(size_t)f * s.src.channels + ch] = (int16_t)read_pcm_sample(
                    g_mem + src + f * sframe + ch * (s.src.bits / 8), s.src.bits);
        uint32_t out_frames = s.dst.rate == s.src.rate || !s.src.rate
                                  ? in_frames
                                  : (uint32_t)((uint64_t)in_frames * s.dst.rate / s.src.rate);
        out_frames = std::min(out_frames, dst_bytes / dframe);
        for (uint32_t f = 0; f < out_frames; ++f) {
            double at = (s.dst.rate == s.src.rate || !s.src.rate)
                            ? double(f)
                            : double(f) * s.src.rate / double(s.dst.rate);
            uint32_t i0 = (uint32_t)at;
            double frac = at - i0;
            uint32_t i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
            for (uint32_t ch = 0; ch < s.dst.channels; ++ch) {
                uint32_t sc = ch < s.src.channels ? ch : s.src.channels - 1;
                double v = samples[(size_t)i0 * s.src.channels + sc] * (1.0 - frac) +
                           samples[(size_t)i1 * s.src.channels + sc] * frac;
                write_pcm_sample(g_mem + dst + f * dframe + ch * (s.dst.bits / 8), s.dst.bits,
                                 (int)(v < 0 ? v - 0.5 : v + 0.5));
            }
        }
        src_used = in_frames * sframe;
        dst_used = out_frames * dframe;
        return;
    }
    if (s.src.tag == 2) { // MS ADPCM, mono: seven-byte header then nibbles
        uint32_t align = s.src.align;
        if (s.src.channels != 1 || align < 8)
            return;
        std::vector<uint8_t> pcm;
        pcm.reserve((size_t)(src_bytes / align + 1) * acm_samples_per_block(s.src) *
                    (s.dst.bits / 8));
        uint32_t off = 0;
        // A wave need not be a whole number of blocks (the game's are not: the
        // last block is short), so the final one is decoded for as many bytes
        // as it has rather than dropped.
        while (off + 7 <= src_bytes) {
            uint32_t block = std::min(align, src_bytes - off);
            const uint8_t *p = g_mem + src + off;
            int predictor = p[0] < 7 ? p[0] : 0;
            int delta = (int16_t)(p[1] | (p[2] << 8));
            int samp1 = (int16_t)(p[3] | (p[4] << 8));
            int samp2 = (int16_t)(p[5] | (p[6] << 8));
            pcm_emit(pcm, samp2, s.dst.bits); // the two seed samples, in order
            pcm_emit(pcm, samp1, s.dst.bits);
            for (uint32_t i = 7; i < block; ++i) {
                uint8_t byte = p[i];
                pcm_emit(pcm, ms_nibble(byte >> 4, predictor, samp1, samp2, delta), s.dst.bits);
                pcm_emit(pcm, ms_nibble(byte & 0x0f, predictor, samp1, samp2, delta), s.dst.bits);
            }
            off += block;
        }
        uint32_t n = (uint32_t)std::min<size_t>(pcm.size(), dst_bytes);
        if (n)
            memcpy(g_mem + dst, pcm.data(), n);
        src_used = off;
        dst_used = n;
        return;
    }
    uint32_t channels = s.src.channels;
    uint32_t align = s.src.align;
    uint32_t blocks = align ? src_bytes / align : 0;
    if (!blocks)
        return;
    uint32_t spb = acm_samples_per_block(s.src); // samples per channel per block
    if (!spb)
        return;
    // After the 4-byte-per-channel headers, the block is groups of 4 bytes per
    // channel in channel order: within one group each byte is two samples, low
    // nibble then high, and the decoded sample count per channel per block is
    // (align - 4*channels) * 8 / (4*channels) + 1 - exactly what
    // acm_samples_per_block reports.
    uint32_t groups = align > 4 * channels ? (align - 4 * channels) / (4 * channels) : 0;
    std::vector<uint8_t> pcm;
    pcm.reserve((size_t)blocks * spb * channels * (s.dst.bits / 8));
    std::vector<int16_t> plane((size_t)channels * spb);
    for (uint32_t b = 0; b < blocks; ++b) {
        const uint8_t *p = g_mem + src + b * align;
        int predictor[2] = {0, 0}, index[2] = {0, 0};
        for (uint32_t ch = 0; ch < channels; ++ch) {
            predictor[ch] = (int16_t)(p[ch * 4] | (p[ch * 4 + 1] << 8));
            index[ch] = std::clamp((int)(int16_t)(p[ch * 4 + 2] | (p[ch * 4 + 3] << 8)), 0, 88);
            plane[(size_t)ch * spb] = (int16_t)predictor[ch]; // the header sample
        }
        uint32_t at = 4 * channels;
        for (uint32_t group = 0; group < groups; ++group) {
            for (uint32_t ch = 0; ch < channels; ++ch) {
                for (uint32_t m = 0; m < 8; m += 2) {
                    uint8_t byte = p[at++];
                    uint32_t base = 1 + group * 8 + m;
                    if (base < spb)
                        plane[(size_t)ch * spb + base] =
                            (int16_t)ima_nibble(byte & 0x0f, predictor[ch], index[ch]);
                    else
                        ima_nibble(byte & 0x0f, predictor[ch], index[ch]);
                    if (base + 1 < spb)
                        plane[(size_t)ch * spb + base + 1] =
                            (int16_t)ima_nibble(byte >> 4, predictor[ch], index[ch]);
                    else
                        ima_nibble(byte >> 4, predictor[ch], index[ch]);
                }
            }
        }
        for (uint32_t i = 0; i < spb; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
                pcm_emit(pcm, plane[(size_t)ch * spb + i], s.dst.bits);
    }
    uint32_t n = (uint32_t)std::min<size_t>(pcm.size(), dst_bytes);
    if (n)
        memcpy(g_mem + dst, pcm.data(), n);
    src_used = blocks * align;
    dst_used = n;
}

void acm_Metrics(X86 *c) {
    uint32_t metric = arg(c, 1), out = arg(c, 2);
    // The metrics a stock Windows XP with the Microsoft PCM converter and the
    // two ADPCM codecs reports (acm.h). The game only asks for
    // ACM_METRIC_MAX_SIZE_FORMAT, but a caller that enumerates asks these.
    uint32_t value;
    switch (metric) {
    case 1:  // ACM_METRIC_COUNT_DRIVERS
    case 20: // ACM_METRIC_COUNT_LOCAL_DRIVERS
        value = 1;
        break;
    case 2:        // ACM_METRIC_COUNT_CODECS
    case 21:       // ACM_METRIC_COUNT_LOCAL_CODECS
        value = 3; // PCM converter, IMA ADPCM, Microsoft ADPCM
        break;
    case 3:  // ACM_METRIC_COUNT_CONVERTERS
    case 22: // ACM_METRIC_COUNT_LOCAL_CONVERTERS
        value = 4;
        break;
    case 4:   // ACM_METRIC_COUNT_FILTERS
    case 5:   // ACM_METRIC_COUNT_DISABLED
    case 6:   // ACM_METRIC_COUNT_HARDWARE
    case 23:  // ACM_METRIC_COUNT_LOCAL_FILTERS
    case 24:  // ACM_METRIC_COUNT_LOCAL_DISABLED
    case 100: // ACM_METRIC_DRIVER_SUPPORT
    case 101: // ACM_METRIC_DRIVER_PRIORITY
        value = 0;
        break;
    case 30:                 // ACM_METRIC_HARDWARE_WAVE_INPUT
    case 31:                 // ACM_METRIC_HARDWARE_WAVE_OUTPUT
        value = 0xffffffffu; // WAVE_MAPPER: no hardware wave device
        break;
    case 50: // ACM_METRIC_MAX_SIZE_FORMAT
        value = 20;
        break;
    case 51: // ACM_METRIC_MAX_SIZE_FILTER
        value = 8;
        break;
    default:
        set_eax(c, 8); // 8 = MMSYSERR_NOTSUPPORTED
        return;
    }
    if (out && gm_valid(out, 4))
        wr32(out, value);
    set_eax(c, 0);
}
void acm_FormatSuggest(X86 *c) {
    uint32_t src = arg(c, 1), dst = arg(c, 2), cb = arg(c, 3);
    AcmFormat sf, df;
    if (!acm_read_format(src, sf) || !dst || !gm_valid(dst, 18) || cb < 18 ||
        !acm_read_format(dst, df)) {
        set_eax(c, 11); // MMSYSERR_INVALPARAM
        return;
    }
    if (df.tag == 0)
        df.tag = 1;
    if (!df.rate)
        df.rate = sf.rate; // an unset rate adopts the source's, as ACM does
    if (!df.channels)
        df.channels = 1;
    if (!df.bits)
        df.bits = 16;
    if (!df.align)
        df.align = df.channels * df.bits / 8;
    AcmStream s;
    s.src = sf;
    s.dst = df;
    if (df.tag != 1 || !acm_supported(s)) {
        set_eax(c, 512); // ACMERR_NOTPOSSIBLE
        return;
    }
    wr16(dst, (uint16_t)df.tag);
    wr16(dst + 2, (uint16_t)df.channels);
    wr32(dst + 4, df.rate);
    wr16(dst + 12, (uint16_t)df.align);
    wr16(dst + 14, (uint16_t)df.bits);
    set_eax(c, 0);
}
void acm_StreamOpen(X86 *c) {
    uint32_t phas = arg(c, 0), src = arg(c, 2), dst = arg(c, 3), filter = arg(c, 4);
    if (phas && gm_valid(phas, 4))
        wr32(phas, 0);
    AcmStream s;
    if (filter || !phas || !gm_valid(phas, 4) || !acm_read_format(src, s.src) ||
        !acm_read_format(dst, s.dst) || !acm_supported(s)) {
        set_eax(c, 512); // ACMERR_NOTPOSSIBLE
        return;
    }
    uint32_t handle = g_next_acm++;
    g_acm[handle] = s;
    wr32(phas, handle);
    set_eax(c, 0);
}
void acm_StreamSize(X86 *c) {
    uint32_t handle = arg(c, 0), cb = arg(c, 1), out = arg(c, 2), mode = arg(c, 3) & 0xf;
    auto it = g_acm.find(handle);
    if (it == g_acm.end()) {
        set_eax(c, 5); // MMSYSERR_INVALIDHANDLE
        return;
    }
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, 11);
        return;
    }
    const AcmStream &s = it->second;
    uint32_t result = cb;
    if (s.src.tag == 1) {
        // PCM: frames of (bits/8 * channels) bytes, re-rated by the ratio.
        uint32_t sframe = (s.src.bits / 8) * s.src.channels;
        uint32_t dframe = (s.dst.bits / 8) * s.dst.channels;
        if (sframe && dframe) {
            if (mode == 1) { // ACM_STREAMSIZEF_DESTINATION
                uint32_t d = cb / dframe;
                uint32_t sf =
                    (s.dst.rate && s.src.rate && s.dst.rate != s.src.rate)
                        ? (uint32_t)(((uint64_t)d * s.src.rate + s.dst.rate - 1) / s.dst.rate)
                        : d;
                result = sf * sframe;
            } else { // ACM_STREAMSIZEF_SOURCE
                uint32_t sf = cb / sframe;
                uint32_t d = (s.dst.rate && s.src.rate && s.dst.rate != s.src.rate)
                                 ? (uint32_t)((uint64_t)sf * s.dst.rate / s.src.rate)
                                 : sf;
                result = d * dframe;
            }
        }
    } else if (s.src.tag == 0x11 || s.src.tag == 2) {
        uint32_t bytes_per_sample = s.dst.bits / 8;
        uint32_t block_out = acm_samples_per_block(s.src) * s.dst.channels * bytes_per_sample;
        if (mode == 1) { // ACM_STREAMSIZEF_DESTINATION
            uint32_t blocks = block_out ? (cb + block_out - 1) / block_out : 0;
            result = blocks * s.src.align;
        } else if (block_out) { // ACM_STREAMSIZEF_SOURCE
            uint32_t blocks = (cb + s.src.align - 1) / s.src.align;
            result = blocks * block_out;
        }
    }
    wr32(out, result);
    set_eax(c, 0);
}
void acm_PrepareHeader(X86 *c) {
    uint32_t handle = arg(c, 0), pah = arg(c, 1);
    if (g_acm.find(handle) == g_acm.end()) {
        set_eax(c, 5);
        return;
    }
    if (!pah || !gm_valid(pah, 84) || rd32(pah) < 84) {
        set_eax(c, 11);
        return;
    }
    wr32(pah + 4, rd32(pah + 4) | 0x00020000u); // ACMSTREAMHEADER_STATUSF_PREPARED
    set_eax(c, 0);
}
void acm_Convert(X86 *c) {
    uint32_t handle = arg(c, 0), pah = arg(c, 1);
    auto it = g_acm.find(handle);
    if (it == g_acm.end()) {
        set_eax(c, 5);
        return;
    }
    if (!pah || !gm_valid(pah, 64)) {
        set_eax(c, 11);
        return;
    }
    uint32_t pb_src = rd32(pah + 12), cb_src = rd32(pah + 16);
    uint32_t pb_dst = rd32(pah + 28), cb_dst = rd32(pah + 32);
    if ((pb_src && !gm_valid(pb_src, cb_src)) || (pb_dst && !gm_valid(pb_dst, cb_dst))) {
        set_eax(c, 11);
        return;
    }
    uint32_t src_used = 0, dst_used = 0;
    acm_decode(it->second, pb_src, cb_src, pb_dst, cb_dst, src_used, dst_used);
    wr32(pah + 20, src_used);                   // cbSrcLengthUsed
    wr32(pah + 36, dst_used);                   // cbDstLengthUsed
    wr32(pah + 4, rd32(pah + 4) | 0x00010000u); // ACMSTREAMHEADER_STATUSF_DONE
    set_eax(c, 0);
}
void acm_UnprepareHeader(X86 *c) {
    uint32_t handle = arg(c, 0), pah = arg(c, 1);
    if (g_acm.find(handle) == g_acm.end()) {
        set_eax(c, 5);
        return;
    }
    if (!pah || !gm_valid(pah, 64)) {
        set_eax(c, 11);
        return;
    }
    wr32(pah + 4, rd32(pah + 4) & ~(0x00010000u | 0x00020000u));
    set_eax(c, 0);
}
void acm_StreamClose(X86 *c) {
    set_eax(c, g_acm.erase(arg(c, 0)) ? 0u : 5u);
}

const ImportShim shims[] = {
    {"DWMAPI.dll", "DwmIsCompositionEnabled", 1, composition_enabled},
    {"DWMAPI.dll", "DwmExtendFrameIntoClientArea", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "IsThemeActive", 0, yes},
    {"UXTHEME.dll", "IsAppThemed", 0, yes},
    {"UXTHEME.dll", "OpenThemeData", 2, zero},
    {"UXTHEME.dll", "GetWindowTheme", 1, zero},
    {"UXTHEME.dll", "CloseThemeData", 1, zero},
    {"UXTHEME.dll", "SetWindowTheme", 3, zero},
    {"UXTHEME.dll", "EnableThemeDialogTexture", 2, zero},
    {"UXTHEME.dll", "IsThemeDialogTextureEnabled", 1, zero},
    {"UXTHEME.dll", "EnableTheming", 1, zero},
    {"UXTHEME.dll", "GetThemeAppProperties", 0, theme_app_properties},
    {"UXTHEME.dll", "SetThemeAppProperties", 1, zero},
    {"UXTHEME.dll", "IsThemePartDefined", 3, zero},
    {"UXTHEME.dll", "IsThemeBackgroundPartiallyTransparent", 3, zero},
    {"UXTHEME.dll", "GetThemeSysBool", 2, zero},
    {"UXTHEME.dll", "GetThemeSysColor", 2, theme_sys_color},
    {"UXTHEME.dll", "GetThemeSysColorBrush", 2, theme_sys_color_brush},
    {"UXTHEME.dll", "GetThemeSysSize", 2, theme_sys_size},
    {"UXTHEME.dll", "GetThemeSysFont", 3, e_handle},
    {"UXTHEME.dll", "GetThemeSysString", 4, e_handle},
    {"UXTHEME.dll", "GetThemeSysInt", 3, e_handle},
    {"UXTHEME.dll", "DrawThemeBackground", 6, e_handle},
    {"UXTHEME.dll", "DrawThemeText", 9, draw_theme_text},
    {"UXTHEME.dll", "DrawThemeTextEx", 9, draw_theme_text_ex},
    {"UXTHEME.dll", "OpenThemeDataForDpi", 3, zero},
    {"UXTHEME.dll", "BeginBufferedPaint", 5, begin_buffered_paint},
    {"UXTHEME.dll", "EndBufferedPaint", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintSetAlpha", 3, buffered_paint_unavailable},
    {"UXTHEME.dll", "BeginBufferedAnimation", 8, begin_buffered_animation},
    {"UXTHEME.dll", "EndBufferedAnimation", 2, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintRenderAnimation", 2, zero},
    {"UXTHEME.dll", "BufferedPaintStopAllAnimations", 1, zero},
    {"UXTHEME.dll", "GetThemeBackgroundContentRect", 6, e_handle},
    {"UXTHEME.dll", "GetThemeBackgroundExtent", 6, e_handle},
    {"UXTHEME.dll", "GetThemePartSize", 7, e_handle},
    {"UXTHEME.dll", "GetThemeTextExtent", 9, e_handle},
    {"UXTHEME.dll", "GetThemeTextMetrics", 5, e_handle},
    {"UXTHEME.dll", "GetThemeBackgroundRegion", 6, e_handle},
    {"UXTHEME.dll", "HitTestThemeBackground", 10, e_handle},
    {"UXTHEME.dll", "DrawThemeEdge", 8, e_handle},
    {"UXTHEME.dll", "DrawThemeIcon", 7, e_handle},
    {"UXTHEME.dll", "GetThemeColor", 5, e_handle},
    {"UXTHEME.dll", "GetThemeMetric", 6, e_handle},
    {"UXTHEME.dll", "GetThemeString", 6, e_handle},
    {"UXTHEME.dll", "GetThemeBool", 5, e_handle},
    {"UXTHEME.dll", "GetThemeInt", 5, e_handle},
    {"UXTHEME.dll", "GetThemeEnumValue", 5, e_handle},
    {"UXTHEME.dll", "GetThemePosition", 5, e_handle},
    {"UXTHEME.dll", "GetThemeFont", 6, e_handle},
    {"UXTHEME.dll", "GetThemeRect", 5, e_handle},
    {"UXTHEME.dll", "GetThemeMargins", 7, e_handle},
    {"UXTHEME.dll", "GetThemeIntList", 5, e_handle},
    {"UXTHEME.dll", "GetThemePropertyOrigin", 5, e_handle},
    {"UXTHEME.dll", "GetThemeFilename", 6, e_handle},
    {"UXTHEME.dll", "GetCurrentThemeName", 6, e_handle},
    {"UXTHEME.dll", "GetThemeDocumentationProperty", 4, e_handle},
    {"UXTHEME.dll", "DrawThemeParentBackground", 3, draw_parent_background},
    {"UXTHEME.dll", "BufferedPaintInit", 0, buffered_paint_unavailable},
    {"UXTHEME.dll", "BufferedPaintUnInit", 0, zero},
    {"WTSAPI32.dll", "WTSRegisterSessionNotification", 2, session_service_unavailable},
    {"WTSAPI32.dll", "WTSUnRegisterSessionNotification", 1, session_service_unavailable},
    {"WINSPOOL.DRV", "EnumPrintersW", 7, enum_printers},
    {"WINSPOOL.DRV", "GetDefaultPrinterW", 2, default_printer},
    {"WINSPOOL.DRV", "OpenPrinterW", 3, open_printer},
    {"WINSPOOL.DRV", "ClosePrinter", 1, yes},
    {"WINSPOOL.DRV", "DocumentPropertiesW", 6, document_properties},
    {"NETAPI32.dll", "NetWkstaGetInfo", 3, workstation_info},
    {"NETAPI32.dll", "NetApiBufferFree", 1, zero},
    {"msvcrt.dll", "memcpy", ARGC_CDECL, crt_memcpy},
    {"msvcrt.dll", "memset", ARGC_CDECL, crt_memset},
    {"SHFOLDER.dll", "SHGetFolderPathW", 5, folder_path},
    {"SHELL32.dll", "Shell_NotifyIconW", 2, yes},
    {"SHELL32.dll", "SHAppBarMessage", 2, zero},
    {"SHELL32.dll", "ShellExecuteW", 6, zero},
    {"URLMON.dll", "URLDownloadToFileW", 5, nullptr},
    // comdlg32: the platform's own file picker through os_file_dialog().
    {"comdlg32.dll", "GetOpenFileNameA", 1, cd_GetOpenFileNameA},
    {"comdlg32.dll", "GetSaveFileNameA", 1, cd_GetSaveFileNameA},
    {"comdlg32.dll", "CommDlgExtendedError", 0, cd_ExtendedError},
    // hhctrl.ocx: HTML Help, logged and never fatal (a NULL HWND).
    {"hhctrl.ocx", "HtmlHelpA", 4, hh_HtmlHelpA},
    {"hhctrl.ocx", "HtmlHelpW", 4, hh_HtmlHelpW},
    // RPCRT4: GUID generation and comparison.
    {"RPCRT4.dll", "UuidCreate", 1, r_UuidCreate},
    {"RPCRT4.dll", "UuidEqual", 3, r_UuidEqual},
    // MSACM32: IMA ADPCM (and PCM) to PCM, the one conversion this game asks
    // for. Unsupported codec pairs fail with ACMERR_NOTPOSSIBLE (512).
    {"MSACM32.dll", "acmMetrics", 3, acm_Metrics},
    {"MSACM32.dll", "acmFormatSuggest", 5, acm_FormatSuggest},
    {"MSACM32.dll", "acmStreamOpen", 8, acm_StreamOpen},
    {"MSACM32.dll", "acmStreamSize", 4, acm_StreamSize},
    {"MSACM32.dll", "acmStreamPrepareHeader", 3, acm_PrepareHeader},
    {"MSACM32.dll", "acmStreamConvert", 3, acm_Convert},
    {"MSACM32.dll", "acmStreamUnprepareHeader", 3, acm_UnprepareHeader},
    {"MSACM32.dll", "acmStreamClose", 2, acm_StreamClose},
};
} // namespace
void misc_dlls_register() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}
