// user32.cpp - USER32 shims: window classes and windows, the message queue,
// paint/DC stubs and the input helpers the host layer feeds.
//
// Windows are bookkeeping only: no pixels are produced here. The host layer
// (Task 7) pushes real events in with host_post_message() and the guest pulls
// them out through PeekMessageA/GetMessageA exactly as it would on Win32.
#include "user32_internal.h"
#include "gdi_image.h"
#include "win32.h"
#include "resources.h"
#include "../platform/os.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <map>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>

// Defined in kernel32.cpp with the scheduler.
void sched_checkpoint();

namespace user32 {

std::map<std::string, WndClass> &classes() {
    static std::map<std::string, WndClass> m;
    return m;
}
std::map<uint32_t, Window> &windows() {
    static std::map<uint32_t, Window> m;
    return m;
}
std::deque<Msg> &queue() {
    static std::deque<Msg> q;
    return q;
}

uint32_t g_next_hwnd = 0x00020004;
uint32_t alloc_window_handle() {
    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    return hwnd;
}
uint32_t g_next_gdi = 0x00028004;
uint32_t g_main_hwnd = 0;
uint32_t g_cursor = 0;
int32_t g_cursor_x = 0, g_cursor_y = 0;
int32_t g_cursor_show = 0;
int32_t g_clip[4] = {0, 0, 0, 0};
bool g_clipped = false;
bool (*g_message_waiter)() = nullptr;
void (*g_window_shown)(uint32_t) = nullptr;
uint8_t g_key_state[256] = {0};
uint8_t g_host_char_pending[256] = {0};

// WS_VISIBLE: a window created with it is shown as part of creation.
const uint32_t WS_VISIBLE = 0x10000000u;

std::string lower(std::string s) {
    for (char &ch : s)
        ch = (char)tolower((unsigned char)ch);
    return s;
}

// Windows' MulDiv rounding (half away from zero); a dialog unit is one quarter
// of the font's average character width across, one eighth of its cell height
// down, and the template's DLUs are the numerator.
int32_t dialog_map_unit(int32_t dlu, int32_t base, int32_t denom) {
    if (!denom)
        return 0;
    int64_t product = int64_t(dlu) * base;
    int64_t magnitude = product < 0 ? -product : product;
    int64_t d = denom < 0 ? -denom : denom;
    int64_t result = (magnitude + d / 2) / d;
    return int32_t(product < 0 ? -result : result);
}
// The base units a template's DS_SETFONT face maps its DLUs with. The runtime
// measures the same substituted font it draws the dialog with; a raster face it
// cannot measure, or a template with no font, uses the 8x16 bitmap cell.
DlgBaseUnit dialog_base_units(const std::string &face, int32_t pointsize, int32_t weight) {
    DlgBaseUnit base;
    int32_t ave = 0, height = 0;
    if (gdi::font_dialog_units(face, pointsize, weight, &ave, &height)) {
        if (ave > 0)
            base.x = ave;
        if (height > 0)
            base.y = height;
    }
    return base;
}
// The font object a DS_SETFONT template selects into its dialog and controls.
// Built from the same face and point size dialog_base_units measures, so the
// layout and the drawn glyphs agree, and cached by those three fields because
// the object is immutable and dialogs are created repeatedly (Petz 5 opens one
// per screen). Zero when the face has no measurable font, so the caller keeps
// the 8x16 bitmap default and nothing changes for a template the runtime cannot
// measure.
uint32_t dialog_font(const std::string &face, int32_t pointsize, int32_t weight) {
    if (face.empty() || pointsize <= 0)
        return 0;
    int32_t ave = 0, height = 0;
    if (!gdi::font_dialog_units(face, pointsize, weight, &ave, &height))
        return 0;
    static std::map<std::string, uint32_t> cache;
    std::string key = face + "|" + std::to_string(pointsize) + "|" + std::to_string(weight);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    uint32_t handle = gdi::make_object(gdi::dialog_font_object(face, pointsize, weight));
    cache[key] = handle;
    return handle;
}

// Class names arrive either as a pointer to a string or as an atom (< 0x10000).
std::string class_key(uint32_t p, bool wide) {
    if (p && p < 0x10000) {
        char buf[32];
        snprintf(buf, sizeof buf, "#atom%u", p);
        return buf;
    }
    return lower(wide ? gm_wstr(p, 256) : gm_str(p, 256));
}

Window *find_window(uint32_t hwnd) {
    if (hwnd == desktop_handle) {
        static Window desktop;
        uint32_t w = 1024, h = 768, bpp = 32;
        win32_display_mode(&w, &h, &bpp);
        desktop.hwnd = desktop_handle;
        desktop.w = w;
        desktop.h = h;
        desktop.visible = true;
        desktop.unicode = true;
        return &desktop;
    }
    auto it = windows().find(hwnd);
    return it == windows().end() ? nullptr : &it->second;
}

// Windows tells a window where it is and how big it is as soon as it exists,
// and again whenever that changes; a game sizes its blit rectangle from those
// two messages and never asks again. No non-client area is modelled here.
static void post_geometry(uint32_t hwnd, const Window *w, bool moved, bool sized) {
    if (moved)
        host_post_message(hwnd, 0x0003 /* WM_MOVE */, 0,
                          ((uint32_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
    if (sized)
        host_post_message(hwnd, 0x0005 /* WM_SIZE */, 0 /* SIZE_RESTORED */,
                          ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
}

void store_msg(uint32_t p, const Msg &m) {
    if (!p)
        return;
    wr32(p + 0, m.hwnd);
    wr32(p + 4, m.message);
    wr32(p + 8, m.wparam);
    wr32(p + 12, m.lparam);
    wr32(p + 16, m.time);
    wr32(p + 20, m.ptx);
    wr32(p + 24, m.pty);
}

} // namespace user32

using namespace user32;

// ---------------------------------------------------------------------------
// Dialog non-client area
// ---------------------------------------------------------------------------
// Classic (non-themed) metrics for a dialog's non-client area. A dialog's
// template rectangle is its CLIENT size (Windows AdjustWindowRect semantics),
// so these insets grow the window around the DLU-mapped client: the non-client
// area is what pushes the client origin in. Only the dialog class (#32770) is
// framed this way — the game's own frame window, its popups and its menus are
// created as other classes and keep their frameless geometry exactly as before.
struct NcInsets {
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    bool caption = false;
};
NcInsets dialog_nc_insets(uint32_t style, uint32_t exstyle) {
    const uint32_t WS_BORDER = 0x00800000u, WS_DLGFRAME = 0x00400000u, WS_CAPTION = 0x00c00000u;
    const uint32_t DS_MODALFRAME = 0x00000080u;
    const uint32_t WS_EX_WINDOWEDGE = 0x00000100u, WS_EX_CLIENTEDGE = 0x00000200u;
    int32_t border = 0;
    if (style & WS_CAPTION) // WS_CAPTION is WS_BORDER|WS_DLGFRAME
        border = 3;         // SM_CXDLGFRAME
    else if (style & (WS_DLGFRAME | DS_MODALFRAME))
        border = 3; // a modal frame without a title bar
    else if (style & WS_BORDER)
        border = 1; // SM_CXBORDER
    if (exstyle & WS_EX_CLIENTEDGE)
        border += 2; // an etched edge around the client
    else if ((exstyle & WS_EX_WINDOWEDGE) && border < 1)
        border = 1;
    NcInsets n;
    n.left = n.right = n.bottom = border;
    // Windows draws the caption at the very top of the window; its height
    // includes what would otherwise be the top border, so the title bar sits
    // flush against the outer edge as it does on XP.
    n.caption = (style & WS_CAPTION) == WS_CAPTION;
    n.top = n.caption ? 19 /*SM_CYCAPTION*/ : border;
    return n;
}
static void apply_dialog_nc(Window &w) {
    NcInsets n = dialog_nc_insets(w.style, w.exstyle);
    w.nc_left = n.left;
    w.nc_top = n.top;
    w.nc_right = n.right;
    w.nc_bottom = n.bottom;
    w.nc_caption = n.caption;
}

// ---------------------------------------------------------------------------
// Host bridge
// ---------------------------------------------------------------------------
void host_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    // Record timer delivery on the same cadence seam as other host messages.
    if (msg == 0x0113)
        host_note_cadence("WM_TIMER");
    Msg m{hwnd, msg, wparam, lparam, host_millis(), (uint32_t)g_cursor_x, (uint32_t)g_cursor_y};
    queue().push_back(m);
}
uint32_t host_main_window() {
    return g_main_hwnd;
}
// The host asking the program to close is a user closing a window, and Windows
// delivers WM_CLOSE to the window the user closed - a visible top-level window,
// not the invisible application window host_main_window names. A program like
// Catz 5 makes its first window an off-screen WS_POPUP helper and only its
// second window visible, so posting WM_CLOSE to host_main_window reaches a
// window whose own close handler refuses to quit (the guest compares the hwnd
// against its recorded main window). Deliver it to every visible top-level
// window; the program's real close handler is among them. With no visible
// window, fall back to host_main_window so a program that hides its windows
// still gets the request.
void host_post_close_messages() {
    bool any = false;
    for (auto &kv : windows()) {
        const Window &w = kv.second;
        if (w.parent || !w.visible)
            continue;
        host_post_message(w.hwnd, 0x0010 /* WM_CLOSE */, 0, 0);
        any = true;
    }
    if (!any && g_main_hwnd)
        host_post_message(g_main_hwnd, 0x0010 /* WM_CLOSE */, 0, 0);
}
// A host choosing a menu command is what a user clicking the menu is: Windows
// sends WM_COMMAND to the window that owns the menu. The port projects the
// guest's play window and has no real menu bar, so deliver it to every visible
// top-level window - whichever owns the command handles it, the rest ignore it.
// This is the same fan-out as host_post_close_messages, for the same reason: the
// program's frame is not the first window host_main_window names.
void host_post_command(uint32_t id) {
    bool any = false;
    for (auto &kv : windows()) {
        const Window &w = kv.second;
        if (w.parent || !w.visible)
            continue;
        host_post_message(w.hwnd, 0x0111 /* WM_COMMAND */, id, 0);
        any = true;
    }
    if (!any && g_main_hwnd)
        host_post_message(g_main_hwnd, 0x0111 /* WM_COMMAND */, id, 0);
}
uint32_t host_window_proc(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    return w ? w->wndproc : 0;
}
bool host_window_rect(uint32_t hwnd, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
    Window *win = find_window(hwnd);
    if (!win)
        return false;
    if (x)
        *x = win->x;
    if (y)
        *y = win->y;
    if (w)
        *w = win->w;
    if (h)
        *h = win->h;
    return true;
}
void host_set_client_size(uint32_t hwnd, int32_t w, int32_t h) {
    if (Window *win = find_window(hwnd)) {
        win->w = w;
        win->h = h;
        win->update_pending = true;
    }
}
void host_set_key_state(int vk, bool down) {
    if (vk >= 0 && vk < 256)
        g_key_state[vk] = down ? 0x80 : 0x00;
}
void host_set_cursor_pos(int32_t x, int32_t y) {
    g_cursor_x = x;
    g_cursor_y = y;
}
void host_set_client_cursor_pos(uint32_t hwnd, int32_t x, int32_t y) {
    int32_t origin_x = 0, origin_y = 0;
    client_origin(hwnd, &origin_x, &origin_y);
    host_set_cursor_pos(int32_t(uint32_t(x) + uint32_t(origin_x)),
                        int32_t(uint32_t(y) + uint32_t(origin_y)));
}
void host_set_message_waiter(bool (*fn)()) {
    g_message_waiter = fn;
}
void host_set_window_shown_callback(void (*fn)(uint32_t)) {
    g_window_shown = fn;
}
bool host_messages_pending() {
    return !queue().empty();
}
bool host_window_visible(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    return w && w->visible;
}
bool host_cursor_visible() {
    return g_cursor_show >= 0;
}
bool host_cursor_clip(int32_t *out) {
    if (!g_clipped)
        return false;
    for (int i = 0; i < 4; ++i)
        out[i] = g_clip[i];
    return true;
}

uint32_t host_dispatch_to_wndproc(X86 *c, uint32_t hwnd, uint32_t msg, uint32_t wparam,
                                  uint32_t lparam) {
    Window *w = find_window(hwnd);
    uint32_t proc = w ? w->wndproc : 0;
    if (!proc) {
        LOGV("message %04x for window %08x has no WNDPROC", msg, hwnd);
        return 0;
    }
    return guest_call(c, proc, hwnd, msg, wparam, lparam);
}

// Defined with the rest of the dialog engine below (global scope); the dialog
// entry points inside namespace user32 need it before that.
uint32_t dlg_item(uint32_t parent, uint32_t id);

namespace user32 {

// ---------------------------------------------------------------------------
// Classes and windows
// ---------------------------------------------------------------------------

// WNDCLASSA and WNDCLASSEXA hold the same fields in the same order; the Ex
// form puts cbSize in front of them and hIconSm behind, so one reader with a
// field offset serves both. `shift` is 4 for the Ex form.
void register_class_named(X86 *c, bool wide, uint32_t shift) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    p += shift;
    WndClass wc;
    wc.style = rd32(p + 0);
    wc.wndproc = rd32(p + 4);
    wc.cls_extra = rd32(p + 8);
    wc.wnd_extra = rd32(p + 12);
    wc.hinstance = rd32(p + 16);
    wc.hicon = rd32(p + 20);
    wc.hcursor = rd32(p + 24);
    wc.hbrush = rd32(p + 28);
    wc.menu_id = rd32(p + 32) < 0x10000 ? rd32(p + 32) : 0;
    wc.menu = wc.menu_id ? "" : (wide ? gm_wstr(rd32(p + 32)) : gm_str(rd32(p + 32)));
    wc.unicode = wide;
    wc.extra.resize((wc.cls_extra + 3) / 4);
    std::string name = class_key(rd32(p + 36), wide);
    wc.name = wide ? gm_wstr(rd32(p + 36)) : gm_str(rd32(p + 36));

    // The class is also reachable through the returned ATOM, which is what a
    // caller passes to CreateWindowExA when it keeps the RegisterClass result.
    uint32_t atom = 0xc000 + (uint32_t)classes().size();
    wc.atom = atom;
    classes()[name] = wc;
    char atom_key[32];
    snprintf(atom_key, sizeof atom_key, "#atom%u", atom);
    classes()[atom_key] = wc;
    LOGV("RegisterClassA(\"%s\", wndproc=%08x) -> atom %u", name.c_str(), wc.wndproc, atom);
    set_eax(c, atom);
}

void u_RegisterClassA(X86 *c) {
    register_class_named(c, false, 0);
}

void u_RegisterClassExA(X86 *c) {
    register_class_named(c, false, 4);
}

void u_UnregisterClassA(X86 *c) {
    classes().erase(class_key(arg(c, 0)));
    set_eax(c, 1);
}

// Create a guest window and deliver WM_NCCREATE/WM_CREATE through its window procedure.
// Honor callback rejection and the initial visibility transition before returning the window handle.
namespace {
void create_window_body(X86 *c, bool wide) {
    uint32_t exstyle = arg(c, 0);
    std::string cls = class_key(arg(c, 1), wide);
    std::string title = wide ? gm_wstr(arg(c, 2)) : gm_str(arg(c, 2));
    uint32_t style = arg(c, 3);
    int32_t x = (int32_t)arg(c, 4), y = (int32_t)arg(c, 5);
    int32_t w = (int32_t)arg(c, 6), h = (int32_t)arg(c, 7);
    uint32_t hinst = arg(c, 10), param = arg(c, 11);

    ensure_system_classes();
    auto ci = classes().find(cls);
    if (ci == classes().end()) {
        LOGW("CreateWindowExA: class \"%s\" was never registered", cls.c_str());
        set_last_error(1407); // ERROR_CANNOT_FIND_WND_CLASS
        set_eax(c, 0);
        return;
    }
    // CW_USEDEFAULT
    if (x == (int32_t)0x80000000)
        x = 0;
    if (y == (int32_t)0x80000000)
        y = 0;
    if (w == (int32_t)0x80000000)
        w = 640;
    if (h == (int32_t)0x80000000)
        h = 480;

    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Window win;
    win.hwnd = hwnd;
    win.wndproc = ci->second.wndproc;
    win.style = style;
    win.exstyle = exstyle;
    win.x = x;
    win.y = y;
    win.w = w;
    win.h = h;
    win.cls = class_key(ci->second.atom);
    win.title_utf8 = title;
    win.unicode = wide;
    win.parent = style & 0x40000000u ? arg(c, 8) : 0;
    win.owner = win.parent ? 0 : arg(c, 8);
    win.menu = win.parent ? 0 : arg(c, 9);
    win.id = win.parent ? arg(c, 9) : 0;
    win.enabled = !(style & 0x08000000u);
    win.thread = guest_current_thread_id();
    win.hinstance = hinst;
    win.extra.assign((ci->second.wnd_extra + 3) / 4, 0);
    // The dialog class is framed by the engine (create_dialog_window); a
    // CreateWindowExA of it directly gets the same non-client area, exactly as
    // Windows does for the #32770 class. Every other class stays frameless.
    if (cls == "#32770" || ci->second.name == "Dialog")
        apply_dialog_nc(win);
    windows()[hwnd] = win;
    if (!g_main_hwnd)
        g_main_hwnd = hwnd;
    LOGV("CreateWindowExA(\"%s\", \"%s\", %dx%d at %d,%d) -> %08x", cls.c_str(), title.c_str(), w,
         h, x, y, hwnd);

    // Windows sends WM_CREATE (with a CREATESTRUCT) before returning.
    uint32_t cs = heap_alloc(48, true);
    if (cs) {
        wr32(cs + 0, param);
        wr32(cs + 4, hinst);
        wr32(cs + 8, arg(c, 9));  // hMenu
        wr32(cs + 12, arg(c, 8)); // hwndParent
        wr32(cs + 16, (uint32_t)h);
        wr32(cs + 20, (uint32_t)w);
        wr32(cs + 24, (uint32_t)y);
        wr32(cs + 28, (uint32_t)x);
        wr32(cs + 32, style);
        wr32(cs + 36, arg(c, 2)); // lpszName
        wr32(cs + 40, arg(c, 1)); // lpszClass
        wr32(cs + 44, exstyle);
    }
    // Windows sends WM_NCCREATE first; FALSE from it cancels creation, and
    // -1 from WM_CREATE does the same.
    // A procedure that does not handle WM_NCCREATE passes it to DefWindowProc,
    // which answers TRUE, so only an explicit FALSE cancels creation.
    uint32_t nc = host_dispatch_to_wndproc(c, hwnd, 0x0081 /* WM_NCCREATE */, 0, cs);
    bool cancelled = (win.wndproc != 0 && nc == 0);
    if (!cancelled) {
        uint32_t cr = host_dispatch_to_wndproc(c, hwnd, 0x0001 /* WM_CREATE */, 0, cs);
        cancelled = (cr == 0xffffffffu);
    }
    if (cs)
        heap_free(cs);
    if (cancelled) {
        LOGW("CreateWindowExA(\"%s\"): the window procedure cancelled creation", cls.c_str());
        windows().erase(hwnd);
        if (g_main_hwnd == hwnd)
            g_main_hwnd = windows().empty() ? 0 : windows().begin()->first;
        set_eax(c, 0);
        return;
    }
    // WS_VISIBLE in the style shows the window as part of creation, without a
    // separate ShowWindow. That is the same hidden-to-visible transition, so
    // it invalidates the window and tells the host, and a host does not have
    // to know that CreateWindowExA can be a show as well.
    if (Window *nw = find_window(hwnd)) {
        post_geometry(hwnd, nw, true, true);
        if ((nw->style & WS_VISIBLE) && !nw->visible) {
            nw->visible = true;
            nw->shown = true;
            nw->update_pending = true;
            if (g_window_shown)
                g_window_shown(hwnd);
        }
    }
    window_created(hwnd);
    set_eax(c, hwnd);
}
} // namespace
// The creation messages, and the imports their handlers make, run without a
// thread switch; see sched_atomic_enter. A guest exception that unwinds past
// the call ends the stretch through the unwinder.
void create_window_named(X86 *c, bool wide) {
    sched_atomic_enter(c->r[R_ESP]);
    create_window_body(c, wide);
    sched_atomic_leave();
}

void u_CreateWindowExA(X86 *c) {
    create_window_named(c, false);
}

void u_DestroyWindow(X86 *c) {
    set_eax(c, destroy_window(c, arg(c, 0)));
}

// A hidden window becoming visible: it goes to the top unless the show keeps
// the stacking, its whole client area needs painting, and the host is told it
// has appeared. ShowWindow and SetWindowPos's SWP_SHOWWINDOW both make it.
static void became_visible(Window *w, bool to_top) {
    if (to_top)
        reorder_window(w->hwnd, 0);
    w->update_pending = true;
    if (!w->shown) {
        w->shown = true;
        post_geometry(w->hwnd, w, false, true);
    }
    if (g_window_shown)
        g_window_shown(w->hwnd);
}

// SW_HIDE is the only command that hides; every other one shows the window in
// some form. Showing a window that was hidden invalidates its whole client
// area, which is what makes the following UpdateWindow paint something, and it
// is the transition a host watches to know a window has appeared.
void u_ShowWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    bool was = w && w->visible;
    if (!w) {
        set_eax(c, 0);
        return;
    }
    uint32_t cmd = arg(c, 1);
    w->show_cmd = cmd;
    w->visible = cmd != 0; // SW_HIDE == 0
    if (w->visible)
        w->style |= WS_VISIBLE;
    else
        w->style &= ~WS_VISIBLE;
    if (cmd == 2 || cmd == 6 || cmd == 7 || cmd == 11) {
        w->style |= 0x20000000u;
        w->style &= ~0x01000000u;
    } else if (cmd == 3) {
        w->style |= 0x01000000u;
        w->style &= ~0x20000000u;
    } else if (cmd == 1 || cmd == 9)
        w->style &= ~0x21000000u;
    set_eax(c, was ? 1 : 0);
    if (!was && w->visible)
        became_visible(w, cmd != 4 && cmd != 7 && cmd != 8); // SW_*NOACTIVATE keeps stacking
    else if (was && !w->visible)
        repaint_exposed_windows(); // hiding exposes what the window covered
}

// UpdateWindow sends WM_PAINT directly to the window procedure, synchronously,
// and only if the update region is not empty. It does not post: a posted
// WM_PAINT would arrive whenever the guest next pumped, and a guest that never
// pumps would never paint.
void u_UpdateWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_eax(c, 0);
        return;
    }
    if (w->update_pending) {
        // The region is NOT validated here. Windows validates it in BeginPaint
        // (or ValidateRect), so a WNDPROC that ignores WM_PAINT leaves the
        // window dirty and gets asked again. Clearing it up front would lose
        // the paint entirely for such a window.
        host_dispatch_to_wndproc(c, w->hwnd, 0x000f /* WM_PAINT */, 0, 0);
    }
    set_eax(c, 1);
}

// SetWindowPos's body, shared with the runtime's own callers: DXGI sizes a
// fullscreen swap chain's output window the way Windows does, through here.
void set_window_pos(X86 *c, Window *w, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy,
                    uint32_t flags, bool refresh_size = false) {
    if (!(flags & 4)) // SWP_NOZORDER
        reorder_window(w->hwnd, after);
    // A WM_SIZE handler may set the same size while arranging children.
    // Only actual changes notify it again, or paint is starved forever.
    bool moved = !(flags & 0x0002) && (w->x != x || w->y != y);
    bool sized = !(flags & 0x0001) && (refresh_size || w->w != cx || w->h != cy);
    if (moved) {
        w->x = x;
        w->y = y;
    } // SWP_NOMOVE
    if (sized) {
        w->w = cx;
        w->h = cy;
        if (!(flags & 0x0008)) // SWP_NOREDRAW
            w->update_pending = true;
    } // SWP_NOSIZE
    LOGV("SetWindowPos(%08x): %dx%d at %d,%d, flags=%08x changed=%d/%d", w->hwnd, w->w, w->h, w->x,
         w->y, flags, moved, sized);
    // SWP_SHOWWINDOW and SWP_HIDEWINDOW are ShowWindow's transitions, made
    // before WM_WINDOWPOSCHANGED as Windows makes them. The VCL shows every
    // child control this way, so ignoring them left each one hidden.
    if ((flags & 0x0040) && !w->visible) {
        w->visible = true;
        w->style |= WS_VISIBLE;
        became_visible(w, false); // stacking is SWP_NOZORDER's business
    } else if ((flags & 0x0080) && w->visible) {
        w->visible = false;
        w->style &= ~WS_VISIBLE;
    }
    if (moved || sized) {
        // SetWindowPos sends this before returning. VCL updates its cached
        // bounds here before setting another dimension. DefWindowProc is
        // responsible for the derived WM_MOVE/WM_SIZE notifications.
        uint32_t pos = heap_alloc(28, true);
        if (pos) {
            wr32(pos, w->hwnd);
            wr32(pos + 4, after);
            wr32(pos + 8, w->x);
            wr32(pos + 12, w->y);
            wr32(pos + 16, w->w);
            wr32(pos + 20, w->h);
            wr32(pos + 24, flags | (moved ? 0 : 2) | (sized ? 0 : 1));
            host_dispatch_to_wndproc(c, w->hwnd, 0x47 /* WM_WINDOWPOSCHANGED */, 0, pos);
            heap_free(pos);
        }
    }
}
void u_SetWindowPos(X86 *c) {
    if (Window *w = find_window(arg(c, 0)))
        set_window_pos(c, w, arg(c, 1), int32_t(arg(c, 2)), int32_t(arg(c, 3)), int32_t(arg(c, 4)),
                       int32_t(arg(c, 5)), arg(c, 6));
    set_eax(c, 1);
}
// A framed dialog's non-client hit test, DefWindowProc's answer to
// WM_NCHITTEST. `x`/`y` are CLIENT coordinates: a point in the caption is
// negative y, a point in the left border negative x, and one past the client
// size is the right/bottom border. The close box is the same square the chrome
// painter draws (TD-061), so what a click cancels is what the user sees.
int32_t dialog_hit_test(const Window *w, int32_t x, int32_t y) {
    if (!w)
        return 0; // HTNOWHERE
    if (x >= 0 && y >= 0 && x < w->w && y < w->h)
        return 1; // HTCLIENT
    const int32_t right = w->w + w->nc_right, bottom = w->h + w->nc_bottom;
    if (x < -w->nc_left || y < -w->nc_top || x >= right || y >= bottom)
        return 0; // HTNOWHERE: outside the window entirely
    if (w->nc_caption && (w->style & 0x00080000u /*WS_SYSMENU*/)) {
        const int32_t box = w->nc_top - 4;
        const int32_t x1 = w->w + w->nc_right - 3;
        const int32_t y0 = -w->nc_top + 2;
        if (x >= x1 - box && x < x1 && y >= y0 && y < y0 + box)
            return 20; // HTCLOSE
    }
    if (w->nc_caption && y < 0)
        return 2; // HTCAPTION
    return 18;    // HTBORDER
}
// A caption drag, Windows' HTCAPTION move loop. The runtime models it as state
// rather than a nested message loop: the pointer capture is the pump's, and a
// move walks the window while the button is held. That keeps the modal pump
// (which owns the queue and the paint synthesis) untouched.
namespace {
uint32_t g_move_hwnd = 0;
int32_t g_move_grab_x = 0, g_move_grab_y = 0;
} // namespace
bool dialog_begin_move(X86 *c, uint32_t hwnd, int32_t screen_x, int32_t screen_y) {
    (void)c;
    Window *w = find_window(hwnd);
    if (!w || !w->nc_caption)
        return false;
    g_move_hwnd = hwnd;
    // The offset from the window origin the caption was grabbed at, so the
    // window does not jump to put its corner under the pointer.
    g_move_grab_x = screen_x - w->x;
    g_move_grab_y = screen_y - w->y;
    return true;
}
void dialog_move_to(X86 *c, int32_t screen_x, int32_t screen_y) {
    Window *w = find_window(g_move_hwnd);
    if (!w) {
        g_move_hwnd = 0;
        return;
    }
    const int32_t nx = screen_x - g_move_grab_x, ny = screen_y - g_move_grab_y;
    if (nx == w->x && ny == w->y)
        return;
    const uint32_t hwnd = g_move_hwnd;
    const int32_t cw = w->w, ch = w->h;
    // Move only: SWP_NOSIZE | SWP_NOZORDER. This dispatches WM_WINDOWPOSCHANGED
    // (and so WM_MOVE) exactly as a real move does, then repaints what the
    // dialog uncovered.
    set_window_pos(c, w, 0, nx, ny, cw, ch, 0x0001u /*SWP_NOSIZE*/ | 0x0004u /*SWP_NOZORDER*/);
    if (Window *after = find_window(hwnd)) {
        after->update_pending = true;
        repaint_exposed_windows();
    }
}
void dialog_end_move() {
    g_move_hwnd = 0;
}
bool dialog_move_active() {
    return g_move_hwnd != 0;
}
// The bounds a window had before a display took it over, by handle.
std::map<uint32_t, std::array<int32_t, 4>> &covered_bounds() {
    static std::map<uint32_t, std::array<int32_t, 4>> m;
    return m;
}

void u_GetWindowRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t r = arg(c, 1);
    if (!w || !r) {
        set_eax(c, 0);
        return;
    }
    int32_t x, y;
    client_origin(w->hwnd, &x, &y);
    // The window rectangle is the frame around the client: the client origin
    // is inside the non-client insets (zero on a frameless window).
    wr32(r, x - w->nc_left);
    wr32(r + 4, y - w->nc_top);
    wr32(r + 8, x + w->w + w->nc_right);
    wr32(r + 12, y + w->h + w->nc_bottom);
    set_eax(c, 1);
}

void u_GetClientRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t r = arg(c, 1);
    if (!r) {
        set_eax(c, 0);
        return;
    }
    wr32(r + 0, 0);
    wr32(r + 4, 0);
    wr32(r + 8, (uint32_t)(w ? w->w : 640));
    wr32(r + 12, (uint32_t)(w ? w->h : 480));
    set_eax(c, 1);
}

void u_SystemParametersInfoA(X86 *c) {
    const uint32_t SPI_GETWORKAREA = 48;
    uint32_t action = arg(c, 0), param = arg(c, 2);
    if (action == SPI_GETWORKAREA && param) {
        // SPI_GETWORKAREA is the primary display's work area, not the calling
        // program's own window. This is routinely asked BEFORE any window
        // exists - a program reads the desktop to size its first frame - so it
        // cannot be derived from a host window at all: that read the first
        // window's client area, and before one existed it fell back to the
        // hard-coded 640x480. Catz 5 calls it in CShlGlobals::LoadGlobals
        // (00478d41), before its first CreateWindowExA, stores the rectangle in
        // ShlGlobals+0x29c and sizes its whole play window to it, so the answer
        // pinned the game to 640x480.
        //
        // The port has no taskbar, so the work area is the whole virtual
        // desktop: the selected DirectDraw/DXGI mode, or the fallback that
        // GetSystemMetrics already reports (SMOKE_DRAWABLE, else 1024x768).
        uint32_t width = 0, height = 0, bpp = 0;
        win32_display_mode(&width, &height, &bpp);
        wr32(param + 0, 0);
        wr32(param + 4, 0);
        wr32(param + 8, width);
        wr32(param + 12, height);
        set_eax(c, 1);
        return;
    }
    log_once(("spi:" + std::to_string(action)).c_str(), "SystemParametersInfoA(%u) unsupported",
             action);
    set_eax(c, 0);
}

void u_AdjustWindowRectEx(X86 *c) {
    // The host window has no non-client area, so the client rect is the window
    // rect. Leaving the rectangle untouched keeps the guest's requested client
    // size intact.
    log_once("AdjustWindowRectEx", "AdjustWindowRectEx: no non-client area is modelled");
    set_eax(c, 1);
}

// ScreenToClient is ClientToScreen's inverse: the window's own position taken
// off a point the game polled with GetCursorPos.
void u_ScreenToClient(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (p && w) {
        LOGV("ScreenToClient(%08x): (%d,%d) through a window at %d,%d", arg(c, 0), (int32_t)rd32(p),
             (int32_t)rd32(p + 4), w->x, w->y);
        int32_t x, y;
        client_origin(w->hwnd, &x, &y);
        wr32(p, rd32(p) - uint32_t(x));
        wr32(p + 4, rd32(p + 4) - uint32_t(y));
    }
    set_eax(c, w ? 1 : 0);
}

void u_OpenIcon(X86 *c) {
    set_eax(c, 1);
}

void u_FindWindowA(X86 *c) {
    set_eax(c, 0);
}

void u_ClientToScreen(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t p = arg(c, 1);
    if (p && w) {
        int32_t x, y;
        client_origin(w->hwnd, &x, &y);
        wr32(p, rd32(p) + uint32_t(x));
        wr32(p + 4, rd32(p + 4) + uint32_t(y));
    }
    set_eax(c, 1);
}

void u_SetRect(X86 *c) {
    uint32_t r = arg(c, 0);
    if (r) {
        wr32(r + 0, arg(c, 1));
        wr32(r + 4, arg(c, 2));
        wr32(r + 8, arg(c, 3));
        wr32(r + 12, arg(c, 4));
    }
    set_eax(c, 1);
}

// A null hwnd invalidates every window, which is what Windows does.
void u_InvalidateRect(X86 *c) {
    uint32_t hwnd = arg(c, 0);
    if (!hwnd) {
        for (auto &kv : windows())
            kv.second.update_pending = true;
    } else if (Window *w = find_window(hwnd)) {
        w->update_pending = true;
    }
    set_eax(c, 1);
}

// A layered window's transparency. Windows requires WS_EX_LAYERED, and the
// VCL sets it before calling; a window without it is refused, as there it is.
void u_SetLayeredWindowAttributes(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w || !(w->exstyle & 0x00080000u)) { // WS_EX_LAYERED
        set_last_error(87);                  // ERROR_INVALID_PARAMETER
        set_eax(c, 0);
        return;
    }
    w->layered_key = arg(c, 1) & 0xffffffu;
    w->layered_alpha = arg(c, 2) & 255u;
    w->layered_flags = arg(c, 3) & 3u;
    w->surface.dirty = true;
    LOGV("SetLayeredWindowAttributes(%08x, key=%06x, alpha=%u, flags=%u)", w->hwnd, w->layered_key,
         w->layered_alpha, w->layered_flags);
    set_eax(c, 1);
}

void u_GetLayeredWindowAttributes(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w || !(w->exstyle & 0x00080000u) || !w->layered_flags) {
        set_last_error(87); // ERROR_INVALID_PARAMETER
        set_eax(c, 0);
        return;
    }
    if (uint32_t p = arg(c, 1))
        wr32(p, w->layered_key);
    if (uint32_t p = arg(c, 2))
        wr8(p, uint8_t(w->layered_alpha));
    if (uint32_t p = arg(c, 3))
        wr32(p, w->layered_flags);
    set_eax(c, 1);
}

void u_SetWindowLongA(X86 *c) {
    Window *w = find_window(arg(c, 0));
    int32_t idx = (int32_t)arg(c, 1);
    uint32_t v = arg(c, 2), old = 0;
    if (!w) {
        set_eax(c, 0);
        return;
    }
    switch (idx) {
    case -4:
        old = w->wndproc;
        w->wndproc = v;
        break; // GWL_WNDPROC
    case -6:
        old = w->hinstance;
        w->hinstance = v;
        break;
    case -8:
        old = w->parent;
        w->parent = v;
        break;
    case -12:
        old = w->id;
        w->id = v;
        break;
    case -16:
        old = w->style;
        w->style = v;
        break;
    case -20:
        old = w->exstyle;
        w->exstyle = v;
        break;
    case -21:
        old = w->userdata;
        w->userdata = v;
        break;
    default:
        if (idx >= 0 && (size_t)(idx / 4) < w->extra.size()) {
            old = w->extra[idx / 4];
            w->extra[idx / 4] = v;
        }
        break;
    }
    set_eax(c, old);
}

void u_GetWindowLongA(X86 *c) {
    Window *w = find_window(arg(c, 0));
    int32_t idx = (int32_t)arg(c, 1);
    if (!w) {
        set_eax(c, 0);
        return;
    }
    switch (idx) {
    case -4:
        set_eax(c, w->wndproc);
        return;
    case -6:
        set_eax(c, w->hinstance);
        return;
    case -8:
        set_eax(c, w->parent);
        return;
    case -12:
        set_eax(c, w->id);
        return;
    case -16:
        set_eax(c, w->style);
        return;
    case -20:
        set_eax(c, w->exstyle);
        return;
    case -21:
        set_eax(c, w->userdata);
        return;
    default:
        if (idx >= 0 && (size_t)(idx / 4) < w->extra.size()) {
            set_eax(c, w->extra[idx / 4]);
            return;
        }
        set_eax(c, 0);
        return;
    }
}

void u_SetWindowTextA(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w) {
        w->title_utf8 = gm_str(arg(c, 1));
        // SetWindowText sends WM_SETTEXT; a caption or control text change
        // repaints the window, exactly as DefWindowProc's WM_SETTEXT does.
        w->update_pending = true;
    }
    set_eax(c, 1);
}

void u_SetDlgItemTextA(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (!child) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, deliver_message(c, child, 0x000c /* WM_SETTEXT */, 0, arg(c, 2), false));
}

// ---------------------------------------------------------------------------
// Messages
// ---------------------------------------------------------------------------
// A message passes the filter when the window matches (0 means any) and the
// message id is in [min, max]; min == max == 0 means no id filter at all.
bool msg_matches(const Msg &m, uint32_t filter_hwnd, uint32_t min_msg, uint32_t max_msg) {
    if (m.message == 0x0012)
        return true; // WM_QUIT is never filtered out
    if (filter_hwnd && m.hwnd != filter_hwnd)
        return false;
    if (min_msg == 0 && max_msg == 0)
        return true;
    if (max_msg == 0)
        return m.message >= min_msg; // Win32 treats 0 as no upper bound
    return m.message >= min_msg && m.message <= max_msg;
}

// An update region is a persistent source of low-priority messages, not a
// queued item consumed by PM_REMOVE. Only validation (normally BeginPaint)
// makes it disappear. Hidden child hierarchies never receive synthesized paint.
// The first window Windows would synthesise a WM_PAINT for, or null.
//
// A window's pending ANCESTORS come first. A control paints into its top-level
// window's surface, so painting a control before its parent lets the parent's
// background fill land on top of it; Windows has no such ordering because a
// control owns its pixels. Returning the highest pending ancestor keeps the
// parent's chrome and erase ahead of everything it contains.
Window *first_paint_window() {
    for (auto &kv : windows()) {
        Window *w = &kv.second;
        if (!w->update_pending || !w->visible || w->w <= 0 || w->h <= 0)
            continue;
        const Window *ancestor = w;
        size_t hops = 0;
        while (ancestor && ancestor->visible && (ancestor->style & 0x40000000u) &&
               ancestor->parent && hops++ < windows().size())
            ancestor = find_window(ancestor->parent);
        if (!ancestor || !ancestor->visible || hops > windows().size())
            continue;
        // Walk back up while an ancestor is itself waiting to be painted. The
        // ancestor need not be a WS_CHILD window: a control's parent is the
        // dialog, which is not one.
        Window *highest = w;
        for (Window *up = find_window(w->parent); up && up->visible; up = find_window(up->parent)) {
            if (up->update_pending && up->w > 0 && up->h > 0)
                highest = up;
        }
        return highest;
    }
    return nullptr;
}
static bool pending_paint(uint32_t p, uint32_t hwnd, uint32_t min_msg, uint32_t max_msg) {
    for (const auto &kv : windows()) {
        const Window *w = &kv.second;
        if (!w->update_pending || !w->visible || w->w <= 0 || w->h <= 0)
            continue;
        const Window *ancestor = w;
        size_t hops = 0;
        while (ancestor && ancestor->visible && (ancestor->style & 0x40000000u) &&
               ancestor->parent && hops++ < windows().size())
            ancestor = find_window(ancestor->parent);
        if (!ancestor || !ancestor->visible || hops > windows().size())
            continue;
        Msg m{w->hwnd, 0xf, 0, 0, host_millis(), uint32_t(g_cursor_x), uint32_t(g_cursor_y)};
        if (!msg_matches(m, hwnd, min_msg, max_msg))
            continue;
        if (!p)
            return true; // only asked whether there is one
        last_message = m;
        store_msg(p, m);
        return true;
    }
    return false;
}

Msg last_message{};

bool paint_pending() {
    return pending_paint(0, 0, 0, 0);
}

void peek_message(X86 *c) {
    // The game's message loop is PeekMessageA and nothing else: it never calls
    // GetMessageA, so anything hung off that one never runs. Windows services
    // timers while a message is being retrieved, which is what makes this the
    // right place rather than a convenient one, and it is where the audio
    // shims get their tick - a streamed sound is refilled by calling the guest
    // back, and only a thread holding the scheduler baton may do that.
    host_pump_timers(c);
    pump_window_timers();
    pump_mouse_input(c);
    gdi_present_windows();
    uint32_t p = arg(c, 0), filter_hwnd = arg(c, 1);
    uint32_t min_msg = arg(c, 2), max_msg = arg(c, 3), flags = arg(c, 4);
    for (auto it = queue().begin(); it != queue().end(); ++it) {
        if (!msg_matches(*it, filter_hwnd, min_msg, max_msg))
            continue;
        last_message = *it;
        store_msg(p, *it);
        if (flags & 1)
            queue().erase(it); // PM_REMOVE
        set_eax(c, 1);
        return;
    }
    set_eax(c, pending_paint(p, filter_hwnd, min_msg, max_msg));
}

// GetMessage blocks until a message arrives. The runtime cannot block on its
// own, so it drives the multimedia timers and then asks the host to wait for
// input. With no host attached and nothing queued there is no message to
// deliver and no way to wait for one, so it reports the documented error
// return (-1) rather than inventing a message the system never sent.
// GetMessageA blocks until a message arrives. It does not return until it has
// one, which is the whole point of the call: a caller that wanted "whatever is
// there right now" would use PeekMessage. It returns 0 for WM_QUIT, and -1
// only for a genuine error, which here means a filter naming a window that
// does not exist.
void u_GetMessageA(X86 *c) {
    uint32_t p = arg(c, 0), filter_hwnd = arg(c, 1);
    uint32_t min_msg = arg(c, 2), max_msg = arg(c, 3);

    if (filter_hwnd && !find_window(filter_hwnd)) {
        set_last_error(1400); // ERROR_INVALID_WINDOW_HANDLE
        set_eax(c, 0xffffffffu);
        return;
    }

    for (;;) {
        pump_window_timers();
        pump_mouse_input(c);
        gdi_present_windows();
        for (auto it = queue().begin(); it != queue().end(); ++it) {
            if (!msg_matches(*it, filter_hwnd, min_msg, max_msg))
                continue;
            Msg m = *it;
            queue().erase(it);
            last_message = m;
            store_msg(p, m);
            set_eax(c, m.message == 0x0012 ? 0 : 1); // WM_QUIT ends the loop
            return;
        }
        if (pending_paint(p, filter_hwnd, min_msg, max_msg)) {
            set_eax(c, 1);
            return;
        }
        host_pump_timers(c);
        pump_window_timers();
        // Without a host there is nothing that could ever post a message, so
        // blocking would be a hang with no way out. That is the one case where
        // the documented error is the honest answer.
        if (!g_message_waiter) {
            log_once("GetMessageA-nohost",
                     "GetMessageA has nothing to deliver and no host to wait on: "
                     "returning -1 rather than blocking forever");
            set_eax(c, 0xffffffffu);
            return;
        }
        // Service the host's event loop. Its answer says whether anything is
        // queued, which is NOT the same as whether anything matches this
        // filter: a queued message the filter rejects would otherwise keep the
        // waiter answering true forever and this loop would spin, starving the
        // very thread that would post the message being waited for.
        g_message_waiter();
        // Nothing matched on this pass, whatever the waiter said. Let the
        // other guest threads run - one of them may be the one that posts it -
        // and if none can, sleep so real time passes for the timers and the
        // host instead of burning the slice.
        if (!host_guest_yield())
            guest_sleep_ms(1);
    }
}

void u_TranslateMessage(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    uint32_t msg = rd32(p + 4), vk = rd32(p + 8), lparam = rd32(p + 12);
    if (msg == 0x0100 || msg == 0x0104) { // WM_KEYDOWN / WM_SYSKEYDOWN
        // The host is the OS here and posts this key's WM_CHAR itself (next to
        // the WM_KEYDOWN), because the modal DialogBoxParamA pump never calls
        // TranslateMessage. A guest message loop DOES call us, so synthesising
        // a second character message typed every key twice - the Editor's
        // "Playscene name" edit. The host marks the scan code it translated;
        // consume the mark and let its own WM_CHAR be dispatched.
        const uint32_t scan = (lparam >> 16) & 0xffu;
        if (scan && g_host_char_pending[scan]) {
            g_host_char_pending[scan] = 0;
            set_eax(c, 0);
            return;
        }
        uint32_t ch = 0;
        if (vk >= 0x30 && vk <= 0x5a)
            ch = vk; // digits and letters
        else if (vk == 0x20)
            ch = ' ';
        else if (vk == 0x0d)
            ch = '\r';
        else if (vk == 0x08)
            ch = '\b';
        else if (vk == 0x1b)
            ch = 0x1b;
        if (ch) {
            bool shift = (g_key_state[0x10] & 0x80) != 0;
            if (!shift && ch >= 'A' && ch <= 'Z')
                ch += 32;
            host_post_message(rd32(p), msg == 0x0100 ? 0x0102 : 0x0106, ch, lparam);
            set_eax(c, 1);
            return;
        }
    }
    set_eax(c, 0);
}

void dispatch_message(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p) {
        set_eax(c, 0);
        return;
    }
    uint32_t hwnd = rd32(p + 0), msg = rd32(p + 4);
    uint32_t wp = rd32(p + 8), lp = rd32(p + 12);
    if (msg == 0x113 && lp)
        set_eax(c, guest_call(c, lp, hwnd, msg, wp, rd32(p + 16)));
    else
        set_eax(c, host_dispatch_to_wndproc(c, hwnd, msg, wp, lp));
}

void u_PostMessageA(X86 *c) {
    // System messages with text pointers cannot be posted asynchronously. A
    // caller must SendMessage so the buffer remains alive through conversion.
    if (arg(c, 1) == 0x000c || arg(c, 1) == 0x000d) {
        set_last_error(1159); // ERROR_MESSAGE_SYNC_ONLY
        set_eax(c, 0);
        return;
    }
    if (arg(c, 0) && !find_window(arg(c, 0))) {
        set_last_error(1400);
        set_eax(c, 0);
        return;
    }
    host_post_message(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
    set_eax(c, 1);
}

void u_PeekMessageA(X86 *c) {
    peek_message(c);
}
void u_DispatchMessageA(X86 *c) {
    dispatch_message(c);
}
void u_SendMessageA(X86 *c) {
    send_message(c, false);
}

void def_window_proc(X86 *c, bool wide) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000c: // WM_SETTEXT
        if (w) {
            w->title_utf8 = wide ? gm_wstr(arg(c, 3)) : gm_str(arg(c, 3));
            // Windows repaints a window whose text changed (the caption, or a
            // static/button/edit's own client), which is what a program that
            // fills a dialog with SetDlgItemTextA then shows expects.
            w->update_pending = true;
            // An edit's caret collapses to the end of the new text, so a stale
            // index into the old (longer) string cannot outlive it.
            if (w->cls == "edit")
                w->caret = w->sel_anchor = int32_t(w->title_utf8.size());
        }
        set_eax(c, w ? 1 : 0);
        return;
    case 0x000d: // WM_GETTEXT
        set_eax(c, put_text(arg(c, 3), arg(c, 2), w ? w->title_utf8 : "", wide));
        return;
    case 0x000e: // WM_GETTEXTLENGTH
        set_eax(c, w ? (wide ? wide_units(w->title_utf8) : uint32_t(w->title_utf8.size())) : 0);
        return;
    // WM_SETFONT/WM_GETFONT belong to every window class, not only STATIC: the
    // dialog engine selects the template font into its controls and a program
    // that asks a control which font it draws with must get that font back.
    case 0x0030: // WM_SETFONT
        if (w) {
            w->font = arg(c, 2);
            if (arg(c, 3)) // redraw requested
                w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0031: // WM_GETFONT
        set_eax(c, w ? w->font : 0);
        return;
    case 0x000f: // DefWindowProc validates an otherwise unhandled paint.
        if (w)
            w->update_pending = false;
        set_eax(c, 0);
        return;
    case 0x0021: // WM_MOUSEACTIVATE: children ask their parent first.
        set_eax(c, w && w->parent
                       ? host_dispatch_to_wndproc(c, w->parent, msg, arg(c, 2), arg(c, 3))
                       : 1);
        return;
    case 0x0084: { // WM_NCHITTEST: DefWindowProc answers from the frame geometry.
        if (!w) {
            set_eax(c, 0);
            return;
        }
        const int32_t sx = int16_t(arg(c, 3) & 0xffff);
        const int32_t sy = int16_t((arg(c, 3) >> 16) & 0xffff);
        int32_t cx, cy;
        client_origin(hwnd, &cx, &cy);
        set_eax(c, uint32_t(dialog_hit_test(w, sx - cx, sy - cy)));
        return;
    }
    case 0x0047: { // WM_WINDOWPOSCHANGED
        uint32_t pos = arg(c, 3);
        if (w && pos) {
            uint32_t flags = rd32(pos + 24);
            uint32_t move = (uint32_t(uint16_t(w->y)) << 16) | uint16_t(w->x);
            uint32_t size = (uint32_t(uint16_t(w->h)) << 16) | uint16_t(w->w);
            if (!(flags & 2))
                host_dispatch_to_wndproc(c, hwnd, 3 /* WM_MOVE */, 0, move);
            if (!(flags & 1) && find_window(hwnd))
                host_dispatch_to_wndproc(c, hwnd, 5 /* WM_SIZE */, 0, size);
        }
        set_eax(c, 0);
        return;
    }
    case 0x0081: // WM_NCCREATE: TRUE, or creation is cancelled
    case 0x0014: // WM_ERASEBKGND: the background counts as erased
        set_eax(c, 1);
        return;
    case 0x0010: // WM_CLOSE -> DestroyWindow
        destroy_window(c, hwnd);
        host_post_message(0, 0x0012 /* WM_QUIT */, 0, 0);
        break;
    default:
        break;
    }
    set_eax(c, 0);
}

void u_DefWindowProcA(X86 *c) {
    def_window_proc(c, false);
}

void u_PostQuitMessage(X86 *c) {
    host_post_message(0, 0x0012, arg(c, 0), 0);
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Paint / DC / cursor / input
// ---------------------------------------------------------------------------
void u_GetDC(X86 *c) {
    uint32_t hwnd = arg(c, 0);
    uint32_t hdc = gdi_window_dc(hwnd);
    // A dialog control's DC carries the background colour its parent chose for
    // it (WM_CTLCOLORSTATIC), so a STATIC that draws its caption directly into
    // a GetDC (Petz 5's warning procedure does) fills with the dialog face
    // instead of the default white. A dialog procedure that leaves the message
    // unhandled gets DefDlgProc's face brush, as paint_control's fallback does.
    if (hdc) {
        if (Window *w = find_window(hwnd); w && w->parent && w->cls == "static") {
            uint32_t brush =
                host_dispatch_to_wndproc(c, w->parent, 0x0138 /* WM_CTLCOLORSTATIC */, hdc, hwnd);
            uint32_t pixel;
            bool have = brush && gdi::brush_color(brush, &pixel);
            if (!have) {
                if (Window *pw = find_window(w->parent); pw && pw->cls == "#32770") {
                    pixel = gdi::argb(0x00c8d0d4); // COLOR_BTNFACE, RGB(0xD4,0xD0,0xC8)
                    have = true;
                }
            }
            if (have)
                if (auto *dc = gdi::dc_of(hdc))
                    dc->bk_color = pixel;
        }
    }
    set_eax(c, hdc);
}
void u_ReleaseDC(X86 *c) {
    set_eax(c, gdi_release_window_dc(arg(c, 0), arg(c, 1)));
}

void u_BeginPaint(X86 *c) {
    uint32_t hwnd = arg(c, 0), ps = arg(c, 1);
    uint32_t hdc = gdi_window_dc(hwnd);
    Window *w = find_window(hwnd);
    if (w)
        w->update_pending = false; // BeginPaint validates the region
    if (recomp_env("TRACE_GDI"))
        LOGW("gdi: BeginPaint hwnd=%08x -> hdc=%08x%s", hwnd, hdc, w ? "" : " (no such window)");
    if (ps) {
        memset(g_mem + ps, 0, 64);
        wr32(ps + 0, hdc);                         // hdc
        wr32(ps + 4, 0);                           // fErase
        wr32(ps + 8, 0);                           // rcPaint.left
        wr32(ps + 12, 0);                          // rcPaint.top
        wr32(ps + 16, (uint32_t)(w ? w->w : 640)); // rcPaint.right
        wr32(ps + 20, (uint32_t)(w ? w->h : 480)); // rcPaint.bottom
    }
    set_eax(c, hdc);
}
void u_EndPaint(X86 *c) {
    uint32_t ps = arg(c, 1);
    set_eax(c, ps && gm_valid(ps, 64) && gdi_release_window_dc(arg(c, 0), rd32(ps)));
}

// Use the desktop fallback until DirectDraw selects a mode, then report that
// mode so a window procedure can size its fullscreen blit rectangle correctly.
void u_GetSystemMetrics(X86 *c) {
    uint32_t width = 0, height = 0, bpp = 0;
    win32_display_mode(&width, &height, &bpp);
    uint32_t v = 0;
    switch (arg(c, 0)) {
    case 0:  // SM_CXSCREEN
    case 16: // SM_CXFULLSCREEN
        v = width;
        LOGV("GetSystemMetrics(%u) -> %u (display mode %ux%ux%u)", arg(c, 0), v, width, height,
             bpp);
        break;
    case 1: // SM_CYSCREEN
        v = height;
        LOGV("GetSystemMetrics(%u) -> %u (display mode %ux%ux%u)", arg(c, 0), v, width, height,
             bpp);
        break;
    case 17: // SM_CYFULLSCREEN
        v = height - 19;
        break;
    case 2:
    case 3:
    case 9:
    case 10:
    case 20:
    case 21:
        v = 17;
        break;
    case 15:
        v = 19;
        break;
    case 19:
    case 22:
    case 23:
    case 41:
    case 42:
    case 44:
    case 63:
    case 74:
        v = 1;
        break;
    case 28:
        v = 112;
        break;
    case 29:
        v = 27;
        break;
    case 30:
    case 31:
    case 52:
    case 53:
        v = 18;
        break;
    case 34:
        v = 112;
        break;
    case 35:
        v = 27;
        break;
    case 36:
    case 37:
    case 49:
    case 50:
        v = 4;
        break;
    case 38:
    case 39:
        v = 75;
        break;
    case 45:
    case 46:
        v = 2;
        break;
    case 47:
        v = 160;
        break;
    case 48:
        v = 24;
        break;
    case 51:
    case 55:
        v = 19;
        break;
    case 54:
    case 59:
    case 61:
    case 78:
        v = width;
        break;
    case 56:
    case 60:
    case 62:
    case 79:
        v = height;
        break;
    case 57:
    case 58:
    case 68:
    case 69:
        v = 16;
        break;
    case 4: // SM_CYCAPTION
        v = 19;
        break;
    case 5: // SM_CXBORDER
    case 6: // SM_CYBORDER
        v = 1;
        break;
    case 7: // SM_CXDLGFRAME
    case 8: // SM_CYDLGFRAME
        v = 3;
        break;
    case 11: // SM_CXICON
    case 12: // SM_CYICON
    case 13: // SM_CXCURSOR
    case 14: // SM_CYCURSOR
        v = 32;
        break;
    case 32: // SM_CXFRAME
    case 33: // SM_CYFRAME
        v = 4;
        break;
    case 43: // SM_CMOUSEBUTTONS
        v = 2;
        break;
    case 80: // SM_CMONITORS
        v = 1;
        break;
    default:
        break;
    }
    set_eax(c, v);
}
// One handle per stock cursor id; the host draws the pointer, so the handle
// only has to be distinct and non-zero.
void u_LoadCursorA(X86 *c) {
    set_eax(c, 0x0002a000u + (arg(c, 1) & 0xfffu));
}
// The creation API determines the encoding delivered to the window procedure.
void u_IsWindowUnicode(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && w->unicode);
}
// An icon or cursor assembled from bitmaps: a distinct handle, never drawn by
// the host, which paints its own pointer.
static uint32_t g_next_icon = 0x0002b000u;
// A cursor loaded from a file is the same kind of handle: distinct, and never
// drawn by the host. The file is not read - the host paints its own pointer -
// but a readable path gets a handle so the guest's cursor bookkeeping works.
// The update region is a flag here, so validating any rectangle validates
// the window, and the update rectangle is the whole client while it is set.
void u_ValidateRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w)
        w->update_pending = false;
    set_eax(c, w != nullptr);
}
void u_GetUpdateRect(X86 *c) {
    Window *w = find_window(arg(c, 0));
    uint32_t rect = arg(c, 1);
    bool pending = w && w->update_pending;
    if (rect && gm_valid(rect, 16)) {
        wr32(rect, 0);
        wr32(rect + 4, 0);
        wr32(rect + 8, pending ? (uint32_t)w->w : 0);
        wr32(rect + 12, pending ? (uint32_t)w->h : 0);
    }
    if (pending && arg(c, 2))
        w->update_pending = false; // bErase: the caller will paint it now
    set_eax(c, pending);
}
void u_LoadCursorFromFileW(X86 *c) {
    set_eax(c, arg(c, 0) && gm_valid(arg(c, 0), 2) ? g_next_icon++ : 0);
}
void u_CreateIconIndirect(X86 *c) {
    uint32_t info = arg(c, 0);
    GdiImage image, mask;
    if (info && gm_valid(info, 20) && gdi_read_bitmap(rd32(info + 16), &image)) {
        if (rd32(info + 12) && gdi_read_bitmap(rd32(info + 12), &mask) &&
            mask.width >= image.width && mask.height >= image.height) {
            for (int32_t y = 0; y < image.height; ++y)
                for (int32_t x = 0; x < image.width; ++x)
                    if (mask.pixels[size_t(y) * mask.width + x] & 0xffffff)
                        image.pixels[size_t(y) * image.width + x] = 0;
        }
        set_eax(c, gdi_create_icon(image));
        return;
    }
    set_eax(c, arg(c, 0) ? g_next_icon++ : 0);
}
void u_DestroyIcon(X86 *c) {
    gdi_delete_icon(arg(c, 0));
    set_eax(c, arg(c, 0) ? 1 : 0);
}
void u_LoadIconA(X86 *c) {
    set_eax(c, 0x00029001);
}
void u_SetCursor(X86 *c) {
    uint32_t prev = g_cursor;
    g_cursor = arg(c, 0);
    set_eax(c, prev);
}
void u_SetCursorPos(X86 *c) {
    g_cursor_x = (int32_t)arg(c, 0);
    g_cursor_y = (int32_t)arg(c, 1);
    set_eax(c, 1);
}
void u_GetCursorPos(X86 *c) {
    uint32_t p = arg(c, 0);
    if (p) {
        wr32(p, (uint32_t)g_cursor_x);
        wr32(p + 4, (uint32_t)g_cursor_y);
    }
    set_eax(c, 1);
}
void u_GetMessagePos(X86 *c) {
    set_eax(c, (last_message.pty << 16) | (last_message.ptx & 0xffff));
}
void u_GetMessageTime(X86 *c) {
    set_eax(c, last_message.time);
}
void u_GetAsyncKeyState(X86 *c) {
    uint32_t vk = arg(c, 0);
    set_eax(c, vk < 256 && g_key_state[vk] ? 0x8000 : 0);
}
void u_GetKeyState(X86 *c) {
    uint32_t vk = arg(c, 0);
    set_eax(c, vk < 256 && g_key_state[vk] ? 0xff80 : 0);
}
void u_ShowCursor(X86 *c) {
    g_cursor_show += arg(c, 0) ? 1 : -1;
    set_eax(c, (uint32_t)g_cursor_show);
}

// The cursor is confined by bookkeeping only; the host layer reads the clip
// rectangle back when it decides where a real cursor may go.
void u_ClipCursor(X86 *c) {
    uint32_t r = arg(c, 0);
    if (!r) {
        g_clipped = false;
        set_eax(c, 1);
        return;
    }
    for (int i = 0; i < 4; ++i)
        g_clip[i] = (int32_t)rd32(r + 4 * (uint32_t)i);
    g_clipped = true;
    set_eax(c, 1);
}

void u_GetClipCursor(X86 *c) {
    uint32_t r = arg(c, 0);
    if (!r) {
        set_eax(c, 0);
        return;
    }
    Window *w = find_window(g_main_hwnd);
    int32_t def[4] = {0, 0, w ? w->w : 640, w ? w->h : 480};
    for (int i = 0; i < 4; ++i)
        wr32(r + 4 * (uint32_t)i, (uint32_t)(g_clipped ? g_clip[i] : def[i]));
    set_eax(c, 1);
}

void u_GetDoubleClickTime(X86 *c) {
    set_eax(c, 500);
}
void u_GetKeyboardType(X86 *c) {
    switch (arg(c, 0)) {
    case 0:
        set_eax(c, 4);
        break; // enhanced 101/102 key
    case 1:
        set_eax(c, 0);
        break;
    default:
        set_eax(c, 12);
        break; // function keys
    }
}
void u_GetKeyboardLayout(X86 *c) {
    set_eax(c, 0x04090409);
} // en-US

// ---------------------------------------------------------------------------
// Clipboard: nothing is shared with the host clipboard.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Message boxes and formatting
// ---------------------------------------------------------------------------
// A message box is shown by the platform's own alert (the macOS host's
// NSAlert) through os_message_box(); the runtime reduces the MB_* type to the
// button set, the default button and an icon, and maps the chosen index back to
// a Win32 id. With no presenter - a headless host, the unit tests - the
// platform answers deterministically, so a box is never answered with a button
// the caller did not offer.
//
// The default button is what pressing Return gives, and MB_DEFBUTTON1/2/3
// selects it; the platform returns it when nothing overrides the answer.
uint32_t message_box_icon(uint32_t type) {
    switch (type & 0x000000f0u) { // MB_ICONMASK
    case 0x10:                    // MB_ICONHAND / MB_ICONERROR
        return 3;
    case 0x20: // MB_ICONQUESTION
        return 4;
    case 0x30: // MB_ICONEXCLAMATION / MB_ICONWARNING
        return 2;
    case 0x40: // MB_ICONASTERISK / MB_ICONINFORMATION
        return 1;
    default:
        return 0;
    }
}

uint32_t message_box_answer(const std::string &text, const std::string &title, uint32_t type) {
    // MB_OK, MB_OKCANCEL, MB_ABORTRETRYIGNORE, MB_YESNOCANCEL, MB_YESNO,
    // MB_RETRYCANCEL, MB_CANCELTRYCONTINUE, indexed by type & MB_TYPEMASK.
    struct Button {
        uint32_t id;
        const char *label;
    };
    static const Button sets[7][3] = {
        {{1, "OK"}, {0, nullptr}, {0, nullptr}},              // IDOK
        {{1, "OK"}, {2, "Cancel"}, {0, nullptr}},             // IDOK, IDCANCEL
        {{3, "Abort"}, {4, "Retry"}, {5, "Ignore"}},          // IDABORT, IDRETRY, IDIGNORE
        {{6, "Yes"}, {7, "No"}, {2, "Cancel"}},               // IDYES, IDNO, IDCANCEL
        {{6, "Yes"}, {7, "No"}, {0, nullptr}},                // IDYES, IDNO
        {{4, "Retry"}, {2, "Cancel"}, {0, nullptr}},          // IDRETRY, IDCANCEL
        {{2, "Cancel"}, {10, "Try Again"}, {11, "Continue"}}, // IDCANCEL, IDTRYAGAIN, IDCONTINUE
    };
    uint32_t set = type & 0x0000000fu;
    if (set > 6)
        set = 0;
    uint32_t want = (type & 0x00000f00u) >> 8; // MB_DEFBUTTON1/2/3
    if (want > 2 || sets[set][want].id == 0)
        want = 0;

    const char *labels[3] = {nullptr, nullptr, nullptr};
    uint32_t ids[3] = {0, 0, 0};
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        if (sets[set][i].id == 0)
            break;
        labels[count] = sets[set][i].label;
        ids[count] = sets[set][i].id;
        ++count;
    }
    OsMessageBox m{};
    m.title = title.empty() ? nullptr : title.c_str();
    m.text = text.c_str();
    m.labels = labels;
    m.ids = ids;
    m.count = count;
    m.default_index = (int)want;
    m.icon = (int)message_box_icon(type);
    int index = os_message_box(&m);
    if (index < 0 || index >= count)
        index = (int)want;
    return ids[index];
}

void u_MessageBoxA(X86 *c) {
    std::string text = gm_str(arg(c, 1), 4096);
    std::string title = gm_str(arg(c, 2), 512);
    uint32_t answer = message_box_answer(text, title, arg(c, 3));
    LOGW("MessageBoxA: [%s] %s -> id %u", title.c_str(), text.c_str(), answer);
    set_eax(c, answer);
}

void u_MessageBoxW(X86 *c) {
    std::string text = gm_wstr(arg(c, 1), 4096);
    std::string title = gm_wstr(arg(c, 2), 512);
    uint32_t answer = message_box_answer(text, title, arg(c, 3));
    LOGW("MessageBoxW: [%s] %s -> id %u", title.c_str(), text.c_str(), answer);
    set_eax(c, answer);
}

// wvsprintfA: the Win32 subset (%s %c %d %i %u %x %X %% with width/precision
// and the l/h size prefixes). `va` points at the guest argument array.
void u_wvsprintfA(X86 *c) {
    uint32_t out = arg(c, 0);
    std::string fmt = gm_str(arg(c, 1), 4096);
    uint32_t va = arg(c, 2);
    std::string res;
    size_t i = 0;
    while (i < fmt.size()) {
        char ch = fmt[i++];
        if (ch != '%') {
            res.push_back(ch);
            continue;
        }
        if (i < fmt.size() && fmt[i] == '%') {
            res.push_back('%');
            ++i;
            continue;
        }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ #0", fmt[i]))
            spec.push_back(fmt[i++]);
        while (i < fmt.size() && isdigit((unsigned char)fmt[i]))
            spec.push_back(fmt[i++]);
        if (i < fmt.size() && fmt[i] == '.') {
            spec.push_back(fmt[i++]);
            while (i < fmt.size() && isdigit((unsigned char)fmt[i]))
                spec.push_back(fmt[i++]);
        }
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h'))
            ++i;
        if (i >= fmt.size())
            break;
        char conv = fmt[i++];
        uint32_t v = rd32(va);
        va += 4;
        char buf[512];
        switch (conv) {
        case 's': {
            std::string s = gm_str(v, 1024);
            spec.push_back('s');
            snprintf(buf, sizeof buf, spec.c_str(), s.c_str());
            break;
        }
        case 'c':
            spec.push_back('c');
            snprintf(buf, sizeof buf, spec.c_str(), (int)(v & 0xff));
            break;
        case 'd':
        case 'i':
            spec.push_back('d');
            snprintf(buf, sizeof buf, spec.c_str(), (int)v);
            break;
        case 'u':
            spec.push_back('u');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        case 'x':
            spec.push_back('x');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        case 'X':
            spec.push_back('X');
            snprintf(buf, sizeof buf, spec.c_str(), (unsigned)v);
            break;
        default:
            snprintf(buf, sizeof buf, "%%%c", conv);
            va -= 4;
            break;
        }
        res += buf;
    }
    if (out)
        memcpy(g_mem + out, res.c_str(), res.size() + 1);
    set_eax(c, (uint32_t)res.size());
}

} // namespace user32

void win32_refresh_display_window(X86 *c, uint32_t hwnd, uint32_t w, uint32_t h, uint32_t bpp) {
    auto *win = user32::find_window(hwnd);
    if (!win || hwnd == user32::desktop_handle)
        return;
    // Exclusive DirectDraw transitions notify the window even when a movie
    // restores the same resolution. Its WM_SIZE handler may have discarded
    // the old presentation rectangle. Ordinary SetWindowPos still suppresses
    // unchanged sizes, so layout calls made by that handler cannot recurse.
    user32::set_window_pos(c, win, 0, 0, 0, int32_t(w), int32_t(h),
                           0x54 /* NOZORDER|NOACTIVATE|SHOWWINDOW */, true);
    host_dispatch_to_wndproc(c, hwnd, 0x7e /* WM_DISPLAYCHANGE */, bpp, (h << 16) | (w & 0xffff));
}

// A display taken over by a fullscreen swap chain has one window on it.
// Windows sizes that window to the mode, and gives its bounds back when the
// chain leaves fullscreen; a window procedure learns both through
// WM_WINDOWPOSCHANGED, as from any SetWindowPos.
void win32_cover_display(X86 *c, uint32_t hwnd, uint32_t w, uint32_t h) {
    auto *win = user32::find_window(hwnd);
    if (!win || hwnd == user32::desktop_handle)
        return;
    auto &saved = user32::covered_bounds();
    if (!saved.count(hwnd))
        saved[hwnd] = {win->x, win->y, win->w, win->h};
    user32::set_window_pos(c, win, 0, 0, 0, int32_t(w), int32_t(h), 0x14 /* NOZORDER|NOACTIVATE */);
}
bool win32_top_level(uint32_t hwnd) {
    auto *win = user32::find_window(hwnd);
    return win && hwnd != user32::desktop_handle && !win->parent;
}
void win32_uncover_display(X86 *c, uint32_t hwnd) {
    auto &saved = user32::covered_bounds();
    auto it = saved.find(hwnd);
    if (it == saved.end())
        return;
    auto b = it->second;
    saved.erase(it);
    if (auto *win = user32::find_window(hwnd))
        user32::set_window_pos(c, win, 0, b[0], b[1], b[2], b[3], 0x14);
}

void u_AdjustWindowRect(X86 *c) {
    // Same as the Ex form: no non-client area, so the client rectangle the
    // guest passed in is already the window rectangle.
    log_once("AdjustWindowRect", "AdjustWindowRect: no non-client area is modelled");
    set_eax(c, 1);
}

// One window is ever active and focused: the game's main window.
void u_GetActiveWindow(X86 *c) {
    set_eax(c, g_main_hwnd);
}
void u_GetForegroundWindow(X86 *c) {
    set_eax(c, g_main_hwnd);
}
void u_SetActiveWindow(X86 *c) {
    set_eax(c, host_main_window());
}
void u_SetForegroundWindow(X86 *c) {
    set_eax(c, 1);
}
void u_SetFocus(X86 *c) {
    set_eax(c, find_window(arg(c, 0)) ? g_main_hwnd : 0);
}
void u_GetMenu(X86 *c) {
    set_eax(c, 0);
}
void u_IsIconic(X86 *c) {
    Window *w = find_window(arg(c, 0));
    set_eax(c, w && (w->style & 0x20000000u) ? 1 : 0);
}
// CloseWindow minimizes the guest window; DestroyWindow owns its lifetime.
void u_CloseWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_last_error(1400);
        set_eax(c, 0);
        return;
    }
    w->show_cmd = 2;
    w->style = (w->style | 0x20000000u) & ~0x01000000u;
    set_eax(c, 1);
}
void u_WaitMessage(X86 *c) {
    // A blocking wait in the original; here a scheduling checkpoint so the
    // service threads run, then return as if a message arrived.
    sched_checkpoint();
    set_eax(c, 1);
}

// Clipboard: nothing is shared with the host clipboard.
void u_OpenClipboard(X86 *c) {
    set_eax(c, 1);
}
void u_CloseClipboard(X86 *c) {
    set_eax(c, 1);
}
void u_IsClipboardFormatAvailable(X86 *c) {
    set_eax(c, 0);
}
void u_GetClipboardData(X86 *c) {
    set_eax(c, 0);
}

// Mouse capture: only the handle is remembered. The host already delivers
// pointer input to the one window whether the button is over it or not.
uint32_t g_capture_hwnd = 0;
void u_SetCapture(X86 *c) {
    uint32_t prev = g_capture_hwnd;
    g_capture_hwnd = arg(c, 0);
    set_eax(c, prev);
}
void u_ReleaseCapture(X86 *c) {
    g_capture_hwnd = 0;
    set_eax(c, 1);
}

// Virtual-key codes against set-1 scan codes on a US layout.
struct VkScan {
    uint8_t vk, scan;
};
const VkScan kVkScan[] = {
    {0x1B, 0x01}, {'1', 0x02},  {'2', 0x03},  {'3', 0x04},  {'4', 0x05},  {'5', 0x06},
    {'6', 0x07},  {'7', 0x08},  {'8', 0x09},  {'9', 0x0A},  {'0', 0x0B},  {0xBD, 0x0C},
    {0xBB, 0x0D}, {0x08, 0x0E}, {0x09, 0x0F}, {'Q', 0x10},  {'W', 0x11},  {'E', 0x12},
    {'R', 0x13},  {'T', 0x14},  {'Y', 0x15},  {'U', 0x16},  {'I', 0x17},  {'O', 0x18},
    {'P', 0x19},  {0xDB, 0x1A}, {0xDD, 0x1B}, {0x0D, 0x1C}, {0x11, 0x1D}, {'A', 0x1E},
    {'S', 0x1F},  {'D', 0x20},  {'F', 0x21},  {'G', 0x22},  {'H', 0x23},  {'J', 0x24},
    {'K', 0x25},  {'L', 0x26},  {0xBA, 0x27}, {0xDE, 0x28}, {0xC0, 0x29}, {0x10, 0x2A},
    {0xDC, 0x2B}, {'Z', 0x2C},  {'X', 0x2D},  {'C', 0x2E},  {'V', 0x2F},  {'B', 0x30},
    {'N', 0x31},  {'M', 0x32},  {0xBC, 0x33}, {0xBE, 0x34}, {0xBF, 0x35}, {0xA1, 0x36},
    {0x6A, 0x37}, {0x12, 0x38}, {0x20, 0x39}, {0x14, 0x3A}, {0x70, 0x3B}, {0x71, 0x3C},
    {0x72, 0x3D}, {0x73, 0x3E}, {0x74, 0x3F}, {0x75, 0x40}, {0x76, 0x41}, {0x77, 0x42},
    {0x78, 0x43}, {0x79, 0x44}, {0x90, 0x45}, {0x91, 0x46}, {0x24, 0x47}, {0x26, 0x48},
    {0x21, 0x49}, {0x6D, 0x4A}, {0x25, 0x4B}, {0x0C, 0x4C}, {0x27, 0x4D}, {0x6B, 0x4E},
    {0x23, 0x4F}, {0x28, 0x50}, {0x22, 0x51}, {0x2D, 0x52}, {0x2E, 0x53}, {0x7A, 0x57},
    {0x7B, 0x58}, {0xA0, 0x2A}, {0xA2, 0x1D}, {0xA3, 0x1D}, {0xA4, 0x38}, {0xA5, 0x38},
};
// (uCode, uMapType[, dwhkl]): 0 VK to scan, 1 scan to VK, 2 VK to character,
// 3 scan to VK distinguishing left and right.
uint32_t map_virtual_key(uint32_t code, uint32_t type) {
    switch (type) {
    case 0:
        for (const VkScan &e : kVkScan)
            if (e.vk == code)
                return e.scan;
        return 0;
    case 1:
    case 3:
        for (const VkScan &e : kVkScan)
            if (e.scan == code) {
                if (type == 1 && e.vk == 0xA1)
                    return 0x10;
                return type == 3 && e.vk == 0x10 ? 0xA0 : e.vk;
            }
        return 0;
    case 2:
        if ((code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || code == ' ' ||
            code == 0x0D || code == 0x08 || code == 0x09 || code == 0x1B)
            return code;
        switch (code) {
        case 0xBA:
            return ';';
        case 0xBB:
            return '=';
        case 0xBC:
            return ',';
        case 0xBD:
            return '-';
        case 0xBE:
            return '.';
        case 0xBF:
            return '/';
        case 0xC0:
            return '`';
        case 0xDB:
            return '[';
        case 0xDC:
            return '\\';
        case 0xDD:
            return ']';
        case 0xDE:
            return '\'';
        }
        if (code >= 0x60 && code <= 0x69)
            return '0' + (code - 0x60);
        return 0;
    default:
        return 0;
    }
}
void u_MapVirtualKeyA(X86 *c) {
    set_eax(c, map_virtual_key(arg(c, 0), arg(c, 1)));
}

// ---------------------------------------------------------------------------
// Rectangles: the three pure-integer helpers the game drives its layout with.
// Point arguments (PtInRect's POINT) arrive as two dwords, as stdcall passes
// an 8-byte aggregate.
// ---------------------------------------------------------------------------
bool as_rect(uint32_t p, int32_t r[4]) {
    if (!p || !gm_valid(p, 16))
        return false;
    for (int i = 0; i < 4; ++i)
        r[i] = int32_t(rd32(p + 4 * (uint32_t)i));
    return true;
}
void u_InflateRect(X86 *c) {
    uint32_t p = arg(c, 0);
    int32_t r[4];
    if (!as_rect(p, r)) {
        set_eax(c, 0);
        return;
    }
    int32_t dx = int32_t(arg(c, 1)), dy = int32_t(arg(c, 2));
    wr32(p + 0, uint32_t(r[0] - dx));
    wr32(p + 4, uint32_t(r[1] - dy));
    wr32(p + 8, uint32_t(r[2] + dx));
    wr32(p + 12, uint32_t(r[3] + dy));
    set_eax(c, 1);
}
void u_OffsetRect(X86 *c) {
    uint32_t p = arg(c, 0);
    int32_t r[4];
    if (!as_rect(p, r)) {
        set_eax(c, 0);
        return;
    }
    int32_t dx = int32_t(arg(c, 1)), dy = int32_t(arg(c, 2));
    wr32(p + 0, uint32_t(r[0] + dx));
    wr32(p + 4, uint32_t(r[1] + dy));
    wr32(p + 8, uint32_t(r[2] + dx));
    wr32(p + 12, uint32_t(r[3] + dy));
    set_eax(c, 1);
}
void u_PtInRect(X86 *c) {
    int32_t r[4];
    if (!as_rect(arg(c, 0), r)) {
        set_eax(c, 0);
        return;
    }
    int32_t x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    set_eax(c, x >= r[0] && x < r[2] && y >= r[1] && y < r[3]);
}
// InvertRect XORs every pixel of the rectangle's RGB. Only the intersection
// with the DC's drawable size is touched; out-of-range rectangles are clipped.
void u_InvertRect(X86 *c) {
    uint32_t dc = arg(c, 0), rp = arg(c, 1);
    int32_t r[4];
    int dw = 0, dh = 0;
    if (!gdi::dc_size(dc, &dw, &dh) || !as_rect(rp, r)) {
        set_eax(c, 0);
        return;
    }
    int32_t l = std::max<int32_t>(0, r[0]), t = std::max<int32_t>(0, r[1]);
    int32_t rr = std::min<int32_t>(dw, r[2]), b = std::min<int32_t>(dh, r[3]);
    for (int32_t y = t; y < b; ++y)
        for (int32_t x = l; x < rr; ++x) {
            uint32_t p;
            if (gdi::read_pixel(dc, x, y, &p))
                gdi::write_pixel(dc, x, y, p ^ 0x00ffffffu);
        }
    set_eax(c, l < rr && t < b);
}

// ---------------------------------------------------------------------------
// Windows: the handful of entry points the game uses to place its frame.
// ---------------------------------------------------------------------------
// MoveWindow keeps its stacking (SWP_NOZORDER) and repaints the whole client
// when asked, which for this runtime is the update flag BeginPaint answers.
void u_MoveWindow(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_last_error(1400); // ERROR_INVALID_WINDOW_HANDLE
        set_eax(c, 0);
        return;
    }
    bool repaint = arg(c, 5) != 0;
    set_window_pos(c, w, 0, int32_t(arg(c, 1)), int32_t(arg(c, 2)), int32_t(arg(c, 3)),
                   int32_t(arg(c, 4)), 0x0004 /* SWP_NOZORDER */);
    if (repaint)
        if (Window *after = find_window(arg(c, 0)))
            after->update_pending = true;
    set_eax(c, 1);
}
// BringWindowToTop raises the window within its own popup group, as Windows
// does; it does not activate it.
void u_BringWindowToTop(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (w)
        reorder_window(w->hwnd, 0);
    set_eax(c, w != nullptr);
}
void u_GetWindowTextLengthA(X86 *c) {
    set_eax(c, deliver_message(c, arg(c, 0), 0x000e /* WM_GETTEXTLENGTH */, 0, 0, false));
}
// ScrollWindowEx scrolls a client rectangle. The runtime keeps no stored
// pixels to blit, so the window is invalidated and repainted whole: the caller
// sees a SIMPLEREGION and the paint handler draws the new offset.
void u_ScrollWindowEx(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_eax(c, 0);
        return;
    }
    w->update_pending = true;
    // The whole client is repainted, so the reported update rectangle is it.
    if (uint32_t update = arg(c, 6))
        if (gm_valid(update, 16)) {
            wr32(update + 0, 0);
            wr32(update + 4, 0);
            wr32(update + 8, uint32_t(w->w));
            wr32(update + 12, uint32_t(w->h));
        }
    set_eax(c, 2); // SIMPLEREGION
}
void u_CallWindowProcA(X86 *c) {
    set_eax(c, guest_call(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4)));
}
// DrawState renders a control-like glyph through a callback. Petz 5 uses the
// text form for its disabled menu marks: the current area's Go To item is
// disabled (so it cannot reload the scene the player is already in) and its
// label is drawn with DrawStateA(DST_TEXT, DSS_DISABLED) instead of DrawTextA.
// Windows paints that with an embossed "disabled" look — the glyph in
// COLOR_3DSHADOW with a COLOR_3DHILIGHT highlight one pixel down and right.
// Drawing nothing left the item showing its icon but no caption (the blank Go
// To row between Asia and Snow Scene). Only the text states are drawn; an
// icon/bitmap DST_ still succeeds without painting.
void u_DrawStateA(X86 *c) {
    uint32_t hdc = arg(c, 0), data = arg(c, 3), flags = arg(c, 9);
    auto *dc = gdi::dc_of(hdc);
    uint32_t type = flags & 0xfu;
    if (!dc || !data || (type != 1u && type != 2u)) { // DST_TEXT, DST_PREFIXTEXT
        set_eax(c, 1);
        return;
    }
    const std::string text = gm_str(data);
    const uint32_t units = wide_units(text);
    const int32_t x = int32_t(arg(c, 5)), y = int32_t(arg(c, 6));
    const int32_t cx = int32_t(arg(c, 7)), cy = int32_t(arg(c, 8));
    if (!units || cx <= 0 || cy <= 0) {
        set_eax(c, 1);
        return;
    }
    uint32_t block = heap_alloc(16 + 2 * (units + 1), true);
    if (!block) {
        set_eax(c, 1);
        return;
    }
    gm_put_wstr(block + 16, text, units + 1);
    auto draw_at = [&](int32_t ox, int32_t oy) {
        wr32(block + 0, uint32_t(x + ox));
        wr32(block + 4, uint32_t(y + oy));
        wr32(block + 8, uint32_t(x + ox + cx));
        wr32(block + 12, uint32_t(y + oy + cy));
        dc->bk_mode = 1; // TRANSPARENT: keep the already-painted item cell
        gdi::draw_text(hdc, block + 16, units, block, 0x20 /*DT_SINGLELINE*/);
    };
    if (flags & 0x20u) {             // DSS_DISABLED
        dc->text_color = 0x00ffffff; // COLOR_3DHILIGHT
        draw_at(1, 1);
        dc->text_color = 0x00808080; // COLOR_3DSHADOW
        draw_at(0, 0);
    } else {
        draw_at(0, 0);
    }
    heap_free(block);
    set_eax(c, 1);
}
// WinHelp opens the old WinHelp engine, which does not exist here. The
// platform seam logs it (and opens a URL when the caller gave one instead of a
// .hlp file); reporting success lets a caller that bails out on failure carry
// on. Never fatal, never blocking on a missing engine.
bool help_target_is_url(const std::string &target) {
    return target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0 ||
           target.rfind("file:", 0) == 0 || target.rfind("mailto:", 0) == 0;
}
void u_WinHelpA(X86 *c) {
    uint32_t p = arg(c, 1);
    std::string file = p ? gm_str(p, 1024) : std::string();
    os_help("WinHelpA", file.c_str(), help_target_is_url(file) ? file.c_str() : nullptr);
    set_eax(c, 1);
}

// ---------------------------------------------------------------------------
// Dialogs and their controls.
// ---------------------------------------------------------------------------
// The runtime models a dialog as an ordinary window whose WNDPROC is the
// caller's DLGPROC, with one child window per control in the template. There
// is no non-client frame and no z-order between controls beyond the template
// order. Standard control messages are answered by control_proc; anything the
// game subclasses must handle itself.
uint32_t dlg_item(uint32_t parent, uint32_t id) {
    if (!find_window(parent))
        return 0;
    for (const auto &kv : windows())
        if (kv.second.parent == parent && kv.second.id == id && !kv.second.owner)
            return kv.first;
    return 0;
}
void u_GetDlgItem(X86 *c) {
    set_eax(c, dlg_item(arg(c, 0), arg(c, 1)));
}
void u_SendDlgItemMessageA(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (!child) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, deliver_message(c, child, arg(c, 2), arg(c, 3), arg(c, 4), false));
}
void u_GetDlgItemTextA(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (!child) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, deliver_message(c, child, 0x000d /* WM_GETTEXT */, arg(c, 3), arg(c, 2), false));
}
void u_SetDlgItemInt(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (!child) {
        set_eax(c, 0);
        return;
    }
    char buf[32];
    if (arg(c, 3))
        snprintf(buf, sizeof buf, "%d", int32_t(arg(c, 2)));
    else
        snprintf(buf, sizeof buf, "%u", arg(c, 2));
    uint32_t tmp = heap_alloc(32, true);
    if (!tmp) {
        set_eax(c, 0);
        return;
    }
    gm_put_str(tmp, buf, 32);
    uint32_t result = deliver_message(c, child, 0x000c /* WM_SETTEXT */, 0, tmp, false);
    heap_free(tmp);
    set_eax(c, result);
}
void u_GetDlgItemInt(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1)), translated = arg(c, 2);
    if (!child) {
        if (translated && gm_valid(translated, 4))
            wr32(translated, 0);
        set_eax(c, 0);
        return;
    }
    uint32_t buf = heap_alloc(64, true);
    if (!buf) {
        if (translated && gm_valid(translated, 4))
            wr32(translated, 0);
        set_eax(c, 0);
        return;
    }
    deliver_message(c, child, 0x000d, 64, buf, false);
    std::string s = gm_str(buf);
    heap_free(buf);
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i]))
        ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
        neg = s[i] == '-';
        ++i;
    }
    uint64_t value = 0;
    bool any = false, ok = true;
    for (; i < s.size() && isdigit((unsigned char)s[i]); ++i) {
        any = true;
        value = value * 10 + uint64_t(s[i] - '0');
        if (value > 0xffffffffull) {
            ok = false;
            break;
        }
    }
    ok = ok && any;
    if (translated && gm_valid(translated, 4))
        wr32(translated, ok ? 1 : 0);
    if (!ok) {
        set_eax(c, 0);
        return;
    }
    if (arg(c, 3))
        set_eax(c, uint32_t(int32_t(neg ? -int64_t(value) : int64_t(value))));
    else
        set_eax(c, uint32_t(value));
}
void u_CheckDlgButton(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (child)
        deliver_message(c, child, 0x00f1 /* BM_SETCHECK */, arg(c, 2), 0, false);
    set_eax(c, child != 0);
}
void u_IsDlgButtonChecked(X86 *c) {
    uint32_t child = dlg_item(arg(c, 0), arg(c, 1));
    if (!child) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, deliver_message(c, child, 0x00f0 /* BM_GETCHECK */, 0, 0, false));
}
// Maps dialog units to pixels with the dialog's own base units: a quarter of
// the DS_SETFONT face's average character width across, an eighth of its cell
// height down. A window that is not a dialog, or whose template had no font,
// keeps the 8x16 bitmap cell the runtime measures everything else with.
void u_MapDialogRect(X86 *c) {
    uint32_t hwnd = arg(c, 0), p = arg(c, 1);
    int32_t r[4];
    Window *w = find_window(hwnd);
    if (!w || !as_rect(p, r)) {
        set_eax(c, 0);
        return;
    }
    int32_t bx = w->dlg_base_x > 0 ? w->dlg_base_x : 8;
    int32_t by = w->dlg_base_y > 0 ? w->dlg_base_y : 16;
    wr32(p + 0, uint32_t(dialog_map_unit(r[0], bx, 4)));
    wr32(p + 4, uint32_t(dialog_map_unit(r[1], by, 8)));
    wr32(p + 8, uint32_t(dialog_map_unit(r[2], bx, 4)));
    wr32(p + 12, uint32_t(dialog_map_unit(r[3], by, 8)));
    set_eax(c, 1);
}

// A modal dialog is identified by the window that is running it; EndDialog
// records its result and clears the running flag the loop below watches. Both
// maps are keyed by the dialog's handle.
std::map<uint32_t, bool> dialog_running;
std::map<uint32_t, int32_t> dialog_results;
void u_EndDialog(X86 *c) {
    uint32_t hwnd = arg(c, 0);
    if (!find_window(hwnd)) {
        set_eax(c, 0);
        return;
    }
    dialog_results[hwnd] = int32_t(arg(c, 1));
    if (dialog_running.count(hwnd))
        dialog_running[hwnd] = false; // the modal loop tears the window down
    else
        destroy_window(c, hwnd); // a modeless caller destroys its own
    set_eax(c, 1);
}

// The close box's rule, which Windows puts in DefDlgProc: the click becomes
// WM_COMMAND IDCANCEL, and a dialog procedure that does not end the dialog
// (Petz 5's warning procedure answers IDCANCEL with 0, so EndDialog is never
// reached) gets a WM_CLOSE next, which is what ends it. Only a #32770 dialog
// has a close box in this runtime, so both steps are a dialog's.
bool close_box_request(X86 *c, uint32_t hwnd) {
    if (!find_window(hwnd))
        return false;
    host_dispatch_to_wndproc(c, hwnd, 0x0111 /* WM_COMMAND */, 2 /* IDCANCEL */, hwnd);
    if (!find_window(hwnd))
        return true;
    auto it = dialog_running.find(hwnd);
    if (it != dialog_running.end() && !it->second)
        return true; // the procedure ended the dialog on its own
    host_dispatch_to_wndproc(c, hwnd, 0x0010 /* WM_CLOSE */, 0, 0);
    return true;
}

// One row height, asked of the parent with WM_MEASUREITEM. An owner-draw box
// owns its row height, so the same value must drive drawing, hit-testing, the
// item rects and the scroll — a click at the wrong divisor lands on the wrong
// row (the farther down, the worse). LBS_OWNERDRAWFIXED (0x10) has one height;
// LBS_OWNERDRAWVARIABLE (0x20) gives a different one per row index. A box that
// is neither, or whose parent leaves itemHeight zero, uses the 8x16 system
// font's line height, which is what MapDialogRect measures a dialog unit with.
int32_t listbox_measure_row(X86 *c, Window *w, int32_t item) {
    if (!w->parent || !(w->style & (0x10 | 0x20)))
        return 16;
    uint32_t ms = heap_alloc(0x18, true);
    if (!ms)
        return 16;
    for (int k = 0; k < 0x18; k += 4)
        wr32(ms + k, 0);
    wr32(ms + 0x00, 2);                                       // CtlType = ODT_LISTBOX
    wr32(ms + 0x04, w->id);                                   // CtlID
    wr32(ms + 0x08, (w->style & 0x20) ? uint32_t(item) : 0u); // itemID
    host_dispatch_to_wndproc(c, w->parent, 0x002c /* WM_MEASUREITEM */, w->id, ms);
    int32_t measured = (int32_t)rd32(ms + 0x10); // itemHeight
    heap_free(ms);
    return measured > 0 ? measured : 16;
}

// The height of row `item`, at one source for every consumer of it. A fixed
// box measures once and caches; a variable box caches per row.
int32_t listbox_item_height(X86 *c, Window *w, int32_t item) {
    if (!(w->style & 0x20)) { // LBS_OWNERDRAWFIXED, LBS_HASSTRINGS or neither
        if (w->list_item_height <= 0)
            w->list_item_height = listbox_measure_row(c, w, 0);
        return w->list_item_height;
    }
    if (item >= 0 && item < (int32_t)w->list_item_heights.size() && w->list_item_heights[item] > 0)
        return w->list_item_heights[item];
    int32_t h = listbox_measure_row(c, w, item);
    if (item >= 0) {
        if ((int32_t)w->list_item_heights.size() <= item)
            w->list_item_heights.resize(item + 1, 0);
        w->list_item_heights[item] = h;
    }
    return h;
}

// Client y of row `item`'s top edge: the heights of the rows scrolled above it,
// from the same source as the drawing.
int32_t listbox_item_top(X86 *c, Window *w, int32_t item) {
    int32_t y = 0;
    for (int32_t i = w->list_top; i < item && i < (int32_t)w->list_items.size(); ++i)
        y += listbox_item_height(c, w, i);
    return y;
}

// The row under client y, or -1 past the last one. The same heights the paint
// and LB_GETITEMRECT use, so a click selects the row the user sees.
int32_t listbox_item_from_y(X86 *c, Window *w, int32_t y) {
    if (y < 0)
        return -1;
    int32_t top = 0;
    for (int32_t i = w->list_top; i < (int32_t)w->list_items.size(); ++i) {
        int32_t h = listbox_item_height(c, w, i);
        if (h <= 0)
            h = 1;
        if (y < top + h)
            return i;
        top += h;
    }
    return -1;
}

// The largest first-visible index that still fills the client. Scrolling down
// stops when the last row reaches the bottom, exactly like a listbox with a
// scrollbar, so the box never scrolls past its content.
int32_t listbox_max_top(X86 *c, Window *w) {
    int64_t spanned = 0;
    int32_t t = int32_t(w->list_items.size());
    while (t > 0) {
        int32_t rh = listbox_item_height(c, w, t - 1);
        if (rh <= 0)
            rh = 1;
        if (spanned + rh > w->h)
            break;
        spanned += rh;
        --t;
    }
    return t;
}
// Keep the selected row inside the client. The view is an index, so it moves a
// whole row at a time; this is the keyboard selection's scroll.
void listbox_scroll_into_view(X86 *c, Window *w) {
    if (w->list_sel < 0)
        return;
    if (w->list_sel < w->list_top) {
        w->list_top = w->list_sel;
        return;
    }
    int64_t bottom = 0;
    for (int32_t i = w->list_top; i <= w->list_sel && i < int32_t(w->list_items.size()); ++i) {
        int32_t rh = listbox_item_height(c, w, i);
        bottom += rh > 0 ? rh : 1;
    }
    while (bottom > w->h && w->list_top < w->list_sel) {
        int32_t rh = listbox_item_height(c, w, w->list_top);
        bottom -= rh > 0 ? rh : 1;
        ++w->list_top;
    }
}
// A listbox scrolls its view by whole rows: the offset is the first visible
// index, so SB_THUMBTRACK has no exact equivalent and is treated as a line.
void listbox_scroll_to(X86 *c, Window *w, int32_t top) {
    int32_t max_top = listbox_max_top(c, w);
    w->list_top = top < 0 ? 0 : (top > max_top ? max_top : top);
    w->update_pending = true;
}

// A listbox with WS_VSCROLL (0x00200000) draws its own 16 px scrollbar on the
// right: two arrows, a track and a proportional thumb. A click on an arrow
// lines, on the track pages, and a drag moves the thumb. The wheel and the
// arrow keys already scroll (TD-062).
constexpr int32_t listbox_sb_width = 16;
bool listbox_has_scrollbar(const Window *w) {
    return (w->style & 0x00200000u) != 0;
}
// A listbox's selection is a SET (`list_selected`); `list_sel` is the CARET
// (LB_GETCURSEL), which Windows paints as a focus rectangle, NOT as a selected
// row. A single-select box keeps the two in step; a multi/extended one does not
// (LBS_MULTIPLESEL 0x8, LBS_EXTENDEDSEL 0x800). The Editor's Contact Sound list
// is LBS_EXTENDEDSEL and its own subclass toggles rows with LB_GETSEL +
// LB_SETSEL: LB_SETSEL(FALSE, row) clears the row from the selection but leaves
// the caret on it, so a paint that also treated the caret as selected made the
// last-clicked row impossible to deselect (TD-107).
bool listbox_multi_select(const Window *w) {
    return (w->style & (0x0008u | 0x0800u)) != 0;
}
// The total height of every row, which the thumb's size is proportional to.
int32_t listbox_content_height(X86 *c, Window *w) {
    int32_t total = 0;
    for (int32_t i = 0; i < int32_t(w->list_items.size()); ++i)
        total += std::max(1, listbox_item_height(c, w, i));
    return total;
}
// The thumb's top (client y, measured from the top arrow) and height for the
// current view. The track is the client less the two 16 px arrow boxes.
int32_t listbox_thumb(X86 *c, Window *w, int32_t *thumb_h) {
    const int32_t total = listbox_content_height(c, w);
    const int32_t track = w->h - 2 * listbox_sb_width;
    const int32_t view = w->h;
    int32_t th = track;
    if (total > view && total > 0)
        th = int32_t(std::max<int64_t>(16, int64_t(track) * view / total));
    if (th > track)
        th = track;
    int32_t above = 0;
    for (int32_t i = 0; i < w->list_top && i < int32_t(w->list_items.size()); ++i)
        above += std::max(1, listbox_item_height(c, w, i));
    int32_t top = listbox_sb_width;
    if (total > view && track > th)
        top += int32_t(int64_t(above) * (track - th) / (total - view));
    if (thumb_h)
        *thumb_h = th;
    return top;
}
// The zone a pointer at client y is over on the listbox's scrollbar: 1 = up
// arrow, 2 = track above the thumb, 3 = the thumb, 4 = track below, 5 = down.
int32_t listbox_sb_zone(X86 *c, Window *w, int32_t y) {
    if (y < listbox_sb_width)
        return 1; // the up arrow
    if (y >= w->h - listbox_sb_width)
        return 5; // the down arrow
    int32_t th = 0;
    const int32_t top = listbox_thumb(c, w, &th);
    if (y >= top && y < top + th)
        return 3;           // the thumb
    return y < top ? 2 : 4; // the track above / below the thumb
}
// An owner-draw listbox (LBS_OWNERDRAWFIXED / LBS_OWNERDRAWVARIABLE) does not
// draw its own rows: Windows asks the parent for the row height with
// WM_MEASUREITEM, then asks it to draw each visible row with WM_DRAWITEM. The
// struct is built in guest memory with the exact Windows layout, so a guest's
// own handler reads it unchanged. Returns false when the parent does not handle
// WM_DRAWITEM (return 0), so the plain-string fallback can run.
bool owner_draw_listbox(X86 *c, Window *w, uint32_t hwnd, uint32_t hdc, int32_t width,
                        int32_t height) {
    if (!w->parent || !(w->style & (0x10 | 0x20)))
        return false;
    int32_t y = 0;
    for (int32_t i = w->list_top; i < int32_t(w->list_items.size()) && y < height; ++i) {
        int32_t ih = listbox_item_height(c, w, i);
        uint32_t ds = heap_alloc(0x30, true);
        if (!ds)
            return false;
        for (int k = 0; k < 0x30; k += 4)
            wr32(ds + k, 0);
        wr32(ds + 0x00, 2);           // CtlType = ODT_LISTBOX
        wr32(ds + 0x04, w->id);       // CtlID
        wr32(ds + 0x08, uint32_t(i)); // itemID = the row index
        wr32(ds + 0x0c, 1);           // itemAction = ODA_DRAWENTIRE
        // ODS_SELECTED is the selection SET; the caret is ODS_FOCUS (0x10) and
        // is not a selection (see listbox_multi_select).
        uint32_t item_state = w->list_selected.count(i) ? 1u : 0u; // ODS_SELECTED
        if (i == w->list_sel)
            item_state |= 0x10u; // ODS_FOCUS
        wr32(ds + 0x10, item_state);
        wr32(ds + 0x14, hwnd);             // hwndItem
        wr32(ds + 0x18, hdc);              // hDC
        wr32(ds + 0x1c, 0);                // rcItem.left
        wr32(ds + 0x20, uint32_t(y));      // rcItem.top
        wr32(ds + 0x24, uint32_t(width));  // rcItem.right
        wr32(ds + 0x28, uint32_t(y + ih)); // rcItem.bottom
        wr32(ds + 0x2c, 0);                // itemData
        uint32_t r = host_dispatch_to_wndproc(c, w->parent, 0x002b /* WM_DRAWITEM */, w->id, ds);
        heap_free(ds);
        if (!r)
            return false;
        y += ih;
    }
    return true;
}
// Classic, non-themed chrome for a framed dialog: a raised 3D frame, a solid
// active-caption title bar with white text and (with WS_SYSMENU) a close box,
// and a COLOR_BTNFACE client fill unless the dialog's own WM_CTLCOLORDLG
// returns a brush. Windows erases the client and paints the frame before the
// client WM_PAINT; this runs in that place, for the dialog the pump is about
// to paint. The frame only ever goes to the window's own surface (the client DC
// maps client (0,0) onto the surface at the non-client inset).
void paint_dialog_chrome(X86 *c, uint32_t hwnd) {
    Window *w = find_window(hwnd);
    if (!w || !w->visible)
        return;
    const int32_t l = w->nc_left, t = w->nc_top, r = w->nc_right, b = w->nc_bottom;
    if (!l && !t && !r && !b)
        return;
    const bool caption = w->nc_caption;
    const int32_t cw = w->w, ch = w->h;
    const uint32_t style = w->style;
    const std::string title = w->title_utf8;
    uint32_t hdc = gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (hdc)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const int32_t right = cw + r, bottom = ch + b;
    // gdi::argb takes a COLORREF (00BBGGRR) and returns the surface's
    // ARGB. These literals are the COLORREF byte order, so a face named by
    // its RGB triple (D4 D0 C8) is written 0x00c8d0d4; writing the RGB value
    // straight in (0x00d4d0c8) would swap red and blue.
    const uint32_t face = gdi::argb(0x00c8d0d4); // COLOR_BTNFACE, RGB(0xD4,0xD0,0xC8)
    const uint32_t hi = gdi::argb(0x00ffffff);   // COLOR_3DHILIGHT
    const uint32_t lo = gdi::argb(0x00808080);   // COLOR_3DSHADOW
    const uint32_t dk = gdi::argb(0x00000000);   // COLOR_3DDKSHADOW
    // The whole window starts as frame face, then the caption bar on top.
    gdi::fill(hdc, {-l, -t, right, bottom}, face);
    if (caption)
        // COLOR_ACTIVECAPTION, the classic navy RGB(0x0A,0x24,0x6A).
        gdi::fill(hdc, {-l, -t, right, 0}, gdi::argb(0x006a240a));
    // The client background: the brush the dialog proc returns for
    // WM_CTLCOLORDLG, else COLOR_BTNFACE. Sent before the client is filled; the
    // window is re-found afterwards because a guest handler may destroy it.
    uint32_t bg = face;
    if (uint32_t brush = host_dispatch_to_wndproc(c, hwnd, 0x0131 /* WM_CTLCOLORDLG */, hdc, hwnd))
        if (!gdi::brush_color(brush, &bg))
            bg = face;
    w = find_window(hwnd);
    if (!w) {
        gdi_release_window_dc(hwnd, hdc);
        return;
    }
    gdi::fill(hdc, {0, 0, cw, ch}, bg);
    // Raised outer edge: highlight on the top/left, dark shadow on the bottom
    // and right, with a shadow just inside the dark edge.
    gdi::fill(hdc, {-l, -t, right, -t + 1}, hi);
    gdi::fill(hdc, {-l, -t, -l + 1, bottom}, hi);
    gdi::fill(hdc, {-l, bottom - 1, right, bottom}, dk);
    gdi::fill(hdc, {right - 1, -t, right, bottom}, dk);
    if (caption) {
        gdi::fill(hdc, {-l + 1, bottom - 2, right - 1, bottom - 1}, lo);
        gdi::fill(hdc, {right - 2, -t + 1, right - 1, bottom - 1}, lo);
        gdi::fill(hdc, {-l, -1, right, 0}, dk); // rule under the caption
    }
    if (!caption)
        goto skip_caption;
    {
        const bool sysmenu = (style & 0x00080000u) != 0; // WS_SYSMENU
        int32_t box = 0;
        if (sysmenu) {
            box = t - 4;
            const int32_t cx1 = right - 3, cy0 = -t + 2, cx0 = cx1 - box, cy1 = cy0 + box;
            gdi::fill(hdc, {cx0, cy0, cx1, cy1}, face);
            gdi::fill(hdc, {cx0, cy0, cx1, cy0 + 1}, hi);
            gdi::fill(hdc, {cx0, cy0, cx0 + 1, cy1}, hi);
            gdi::fill(hdc, {cx0, cy1 - 1, cx1, cy1}, dk);
            gdi::fill(hdc, {cx1 - 1, cy0, cx1, cy1}, dk);
            gdi::line(hdc, cx0 + 3, cy0 + 3, cx1 - 4, cy1 - 4);
            gdi::line(hdc, cx1 - 4, cy0 + 3, cx0 + 3, cy1 - 4);
        }
        uint32_t units = title.empty() ? 0 : wide_units(title);
        const int32_t text_right = sysmenu ? right - 3 - box - 4 : right - 4;
        if (units && units < (UINT32_MAX - 32) / 2 && text_right > 4) {
            if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
                if (auto *title_dc = gdi::dc_of(hdc)) {
                    title_dc->text_color = 0x00ffffff;
                    title_dc->bk_mode = 1; // TRANSPARENT
                    if (w->font)
                        title_dc->font = w->font; // the dialog's DS_SETFONT face
                }
                wr32(block + 0, 4);
                wr32(block + 4, uint32_t(-t));
                wr32(block + 8, uint32_t(text_right));
                wr32(block + 12, 0);
                gm_put_wstr(block + 16, title, units + 1);
                gdi::draw_text(hdc, block + 16, units, block,
                               0x20 /*DT_SINGLELINE*/ | 0x4 /*DT_VCENTER*/ | 0x800 /*DT_NOPREFIX*/);
                heap_free(block);
            }
        }
    }
skip_caption:
    if (Window *after = find_window(hwnd))
        after->update_pending = false;
    gdi_release_window_dc(hwnd, hdc);
}

// One window the way the modal pump paints it: chrome and erase first, its own
// WM_PAINT, then every visible child in z-order (recursively). A control has no
// surface of its own - its DC writes into the top-level window's surface - so
// without the children being repainted here, a parent's background fill would
// be the last thing written over a control that happened to be painted first.
// Windows does not have the ordering at all (each control owns its pixels);
// repainting the subtree makes the frame the same whatever order the
// invalidations arrived in, and one call now clears the whole subtree's update
// regions instead of one window per pump pass.
void paint_window_tree(X86 *c, uint32_t hwnd) {
    paint_dialog_chrome(c, hwnd);
    if (!find_window(hwnd))
        return;
    host_dispatch_to_wndproc(c, hwnd, 0x000f /* WM_PAINT */, 0, 0);
    if (!find_window(hwnd))
        return;
    for (uint32_t child : window_z_order(hwnd)) {
        Window *cw = find_window(child);
        if (!cw || !cw->visible || !(cw->style & 0x40000000u) || cw->w <= 0 || cw->h <= 0)
            continue;
        paint_window_tree(c, child);
        if (!find_window(hwnd))
            return;
    }
}

// A filled disc by horizontal spans (an integer midpoint form, no libm): the
// radio glyph needs a ring and a dot and the runtime has no ellipse helper a
// control paint can reach without an import.
void fill_disc(uint32_t hdc, int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    for (int32_t dy = -r; dy <= r; ++dy) {
        int32_t x = r;
        while (x > 0 && int64_t(x) * x + int64_t(dy) * dy > int64_t(r) * r)
            --x;
        gdi::fill(hdc, {cx - x, cy + dy, cx + x + 1, cy + dy + 1}, color);
    }
}
// A filled triangle pointing up or down, by horizontal spans (the runtime has
// no polygon fill a control paint can reach). Shared by the scrollbar arrows,
// the combo box's drop-down arrow and the up-down.
void fill_triangle(uint32_t hdc, int32_t cx, int32_t cy, int32_t half, uint32_t color, bool down) {
    for (int32_t i = 0; i <= half; ++i) {
        const int32_t w = half - i;
        const int32_t y = down ? cy - half + i : cy + half - i;
        gdi::fill(hdc, {cx - w, y, cx + w + 1, y + 1}, color);
    }
}
// Button styles whose check state is the window's own `check_state` rather than
// the pressed flag: checkbox, auto-checkbox, radio, 3-state, auto-3-state,
// auto-radio (BS_TYPEMASK values 2,3,4,5,6,9).
bool is_check_button(uint32_t style) {
    const uint32_t bs = style & 0x0fu;
    return bs == 2 || bs == 3 || bs == 4 || bs == 5 || bs == 6 || bs == 9;
}
// BS_GROUPBOX (7): a frame with a caption, not a button. Windows gives it no
// pressed state and reports no BN_CLICKED, so a click on it is inert.
bool is_group_box(uint32_t style) {
    return (style & 0x0fu) == 7u;
}
// A checkbox or radio button: a 13 px glyph (a box, or a circle) at the left of
// the caption (the right, with BS_LEFTTEXT), filled when checked, and the
// caption drawn in the control's font. A checkbox gets the parent's
// WM_CTLCOLORSTATIC colour protocol, as on Windows.
void paint_checkbox(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const bool radio = ((w->style & 0x0fu) == 0x4u || (w->style & 0x0fu) == 0x9u);
    const uint32_t checked = (w->check_state & 3u) != 0;
    const bool left_text = (w->style & 0x20u) != 0; // BS_LEFTTEXT
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk_mode = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    // Background: the parent's WM_CTLCOLORSTATIC brush, else the dialog face.
    uint32_t pixel = 0;
    bool have = false;
    if (w->parent) {
        uint32_t brush =
            host_dispatch_to_wndproc(c, w->parent, 0x0138 /* WM_CTLCOLORSTATIC */, hdc, hwnd);
        have = brush && gdi::brush_color(brush, &pixel);
        if (!have) {
            if (Window *pw = find_window(w->parent); pw && pw->cls == "#32770") {
                uint32_t b = host_dispatch_to_wndproc(c, w->parent, 0x0131 /* WM_CTLCOLORDLG */,
                                                      hdc, w->parent);
                have = b && gdi::brush_color(b, &pixel);
                if (!have)
                    pixel = gdi::argb(0x00c8d0d4), have = true;
            }
        }
    }
    if (have)
        gdi::fill(hdc, {0, 0, width, height}, pixel);
    if (auto *paint_dc = gdi::dc_of(hdc)) {
        paint_dc->bk_mode = 1; // TRANSPARENT over the background
        paint_dc->text_color = 0x00000000;
    }
    const int32_t g = 13, gy = (height - 13) / 2, gx = left_text ? width - 13 : 0;
    const uint32_t white = 0x00ffffff, dk = gdi::argb(0x00000000), lo = gdi::argb(0x00808080);
    if (radio) {
        fill_disc(hdc, gx + g / 2, gy + g / 2, g / 2, lo);
        fill_disc(hdc, gx + g / 2, gy + g / 2, g / 2 - 1, white);
        if (checked)
            fill_disc(hdc, gx + g / 2, gy + g / 2, g / 2 - 3, dk);
    } else {
        gdi::fill(hdc, {gx, gy, gx + g, gy + g}, white);
        gdi::fill(hdc, {gx, gy, gx + g, gy + 1}, lo); // sunken top/left
        gdi::fill(hdc, {gx, gy, gx + 1, gy + g}, lo);
        gdi::fill(hdc, {gx, gy + g - 1, gx + g, gy + g}, white); // light bottom/right
        gdi::fill(hdc, {gx + g - 1, gy, gx + g, gy + g}, white);
        if (checked) {
            gdi::line(hdc, gx + 3, gy + 6, gx + 5, gy + 9);
            gdi::line(hdc, gx + 5, gy + 9, gx + 10, gy + 3);
        }
    }
    const int32_t tx = left_text ? 0 : gx + g + 4;
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (units && units < (UINT32_MAX - 32) / 2) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            wr32(block + 0, uint32_t(tx));
            wr32(block + 4, 0);
            wr32(block + 8, uint32_t(width));
            wr32(block + 12, uint32_t(height));
            gm_put_wstr(block + 16, text, units + 1);
            gdi::draw_text(hdc, block + 16, units, block,
                           0x0 /*DT_LEFT*/ | 0x4 /*DT_VCENTER*/ | 0x20 /*DT_SINGLELINE*/ |
                               0x800 /*DT_NOPREFIX*/);
            heap_free(block);
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk_mode;
    }
}
// A group box (BS_GROUPBOX): a 1 px etched frame whose top edge is broken for
// the caption, the caption drawn at the top-left in the control's own font.
// Windows draws this in the BUTTON class, and a program relies on the frame to
// read the dialog's structure; the caption is not a normal centred label, so
// the STATIC caption path cannot draw it (its text_flags returns 0 for a button
// style, leaving the box invisible).
void paint_groupbox(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk_mode = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    // The parent chooses the caption's colours through the static protocol; a
    // null brush leaves the dialog face already painted underneath (the dialog
    // engine fills its client), as for a STATIC caption.
    uint32_t pixel = 0;
    bool have = false;
    if (w->parent) {
        uint32_t brush =
            host_dispatch_to_wndproc(c, w->parent, 0x0138 /* WM_CTLCOLORSTATIC */, hdc, hwnd);
        have = brush && gdi::brush_color(brush, &pixel);
        if (!have) {
            if (Window *pw = find_window(w->parent); pw && pw->cls == "#32770") {
                uint32_t b = host_dispatch_to_wndproc(c, w->parent, 0x0131 /* WM_CTLCOLORDLG */,
                                                      hdc, w->parent);
                have = b && gdi::brush_color(b, &pixel);
            }
        }
    }
    if (have)
        gdi::fill(hdc, {0, 0, width, height}, pixel);
    const std::string text = w->title_utf8;
    const int32_t th = gdi::text_height(hdc);
    const int32_t top = th > 0 ? th / 2 : 6; // the frame's top edge
    const int32_t gap = 8;                   // the caption's left inset
    const int32_t tw = text.empty() ? 0 : gdi::text_width(hdc, text);
    const uint32_t shadow = gdi::argb(0x00808080), light = gdi::argb(0x00ffffff);
    // Etched frame: the top/left edge in the shadow, the bottom/right in the
    // highlight, with the top edge interrupted under the caption.
    gdi::fill(hdc, {0, top, gap, top + 1}, shadow);
    if (tw)
        gdi::fill(hdc, {gap + tw + 2, top, width, top + 1}, shadow);
    gdi::fill(hdc, {0, top, 1, height}, shadow);
    gdi::fill(hdc, {width - 1, top, width, height}, shadow);
    gdi::fill(hdc, {0, height - 1, width, height}, light);
    // The caption sits on the frame's top-left, its text transparent over the
    // dialog face.
    if (!text.empty()) {
        if (auto *paint_dc = gdi::dc_of(hdc)) {
            paint_dc->bk_mode = 1; // TRANSPARENT
            paint_dc->text_color = 0x00000000;
        }
        uint32_t units = wide_units(text);
        if (units && units < (UINT32_MAX - 32) / 2) {
            if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
                wr32(block + 0, uint32_t(gap));
                wr32(block + 4, 0);
                wr32(block + 8, uint32_t(width));
                wr32(block + 12, uint32_t(height));
                gm_put_wstr(block + 16, text, units + 1);
                gdi::draw_text(hdc, block + 16, units, block,
                               0x0 /*DT_LEFT*/ | 0x20 /*DT_SINGLELINE*/ | 0x800 /*DT_NOPREFIX*/);
                heap_free(block);
            }
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk_mode;
    }
}
// A push button in a dialog: a COLOR_BTNFACE face with a raised 3D bevel, a
// sunken one while the press is held, and the caption centred. BS_DEFPUSHBUTTON
// (style 1) also gets the dark default-button border. Checkboxes and radios get
// the glyph path above; a group box gets its own frame and top-left caption;
// other button styles fall back to the STATIC caption path.
void paint_button(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (is_check_button(w->style)) {
        paint_checkbox(c, hwnd, into);
        return;
    }
    if ((w->style & 0x0fu) == 7u) { // BS_GROUPBOX
        paint_groupbox(c, hwnd, into);
        return;
    }
    if ((w->style & 0x0fu) > 1u) {
        paint_control(c, hwnd, into);
        return;
    }
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const bool pressed = w->pressed;
    const bool is_default = (w->style & 0x0fu) == 1u;
    const bool disabled = (w->style & 0x08000000u) != 0; // WS_DISABLED
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk_mode = dc->bk_mode;
    if (w->font)
        dc->font = w->font; // the dialog's WM_SETFONT face, or the control's own
    // A dialog may choose the button's face with WM_CTLCOLORBTN; the classic
    // COLOR_BTNFACE is the fallback.
    uint32_t face = gdi::argb(0x00c8d0d4);
    if (w->parent) {
        uint32_t brush =
            host_dispatch_to_wndproc(c, w->parent, 0x0135 /* WM_CTLCOLORBTN */, hdc, hwnd);
        uint32_t pixel;
        if (brush && gdi::brush_color(brush, &pixel))
            face = pixel;
    }
    const uint32_t hi = gdi::argb(0x00ffffff), lo = gdi::argb(0x00808080),
                   dk = gdi::argb(0x00000000);
    gdi::fill(hdc, {0, 0, width, height}, face);
    // The bevel swaps ends when the button is held down.
    const uint32_t top_left = pressed ? lo : hi, bottom_right = pressed ? hi : dk;
    gdi::fill(hdc, {0, 0, width, 1}, top_left);
    gdi::fill(hdc, {0, 0, 1, height}, top_left);
    gdi::fill(hdc, {0, height - 1, width, height}, bottom_right);
    gdi::fill(hdc, {width - 1, 0, width, height}, bottom_right);
    if (is_default) { // a dark 1 px border around a default push button
        gdi::fill(hdc, {0, 0, width, 1}, dk);
        gdi::fill(hdc, {0, 0, 1, height}, dk);
        gdi::fill(hdc, {0, height - 1, width, height}, dk);
        gdi::fill(hdc, {width - 1, 0, width, height}, dk);
    }
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (units && units < (UINT32_MAX - 32) / 2) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            if (auto *paint_dc = gdi::dc_of(hdc)) {
                if (disabled)
                    paint_dc->text_color = 0x00808080;
                paint_dc->bk_mode = 1; // TRANSPARENT over the bevel
            }
            int32_t off = pressed ? 1 : 0;
            wr32(block + 0, off);
            wr32(block + 4, off);
            wr32(block + 8, uint32_t(width));
            wr32(block + 12, uint32_t(height));
            gm_put_wstr(block + 16, text, units + 1);
            gdi::draw_text(hdc, block + 16, units, block,
                           0x1 /*DT_CENTER*/ | 0x4 /*DT_VCENTER*/ | 0x20 /*DT_SINGLELINE*/ |
                               0x800 /*DT_NOPREFIX*/);
            heap_free(block);
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) { // the caller's DC keeps its own state
        caller->font = caller_font;
        caller->bk_mode = caller_bk_mode;
    }
}

// The listbox's own WS_VSCROLL bar, drawn in the right 16 px: two arrow boxes,
// a track and a proportional thumb. The rows are drawn narrower to make room.
void paint_listbox_scrollbar(X86 *c, Window *w, uint32_t hdc) {
    const int32_t x0 = w->w - listbox_sb_width, height = w->h;
    const uint32_t face = gdi::argb(0x00c8d0d4), light = gdi::argb(0x00ffffff),
                   dark = gdi::argb(0x00000000), shadow = gdi::argb(0x00808080);
    gdi::fill(hdc, {x0, 0, w->w, height}, face);
    auto bevel = [&](int32_t t, int32_t b) {
        gdi::fill(hdc, {x0, t, w->w, t + 1}, light);
        gdi::fill(hdc, {x0, t, x0 + 1, b}, light);
        gdi::fill(hdc, {x0, b - 1, w->w, b}, dark);
        gdi::fill(hdc, {w->w - 1, t, w->w, b}, dark);
    };
    bevel(0, listbox_sb_width);
    bevel(height - listbox_sb_width, height);
    fill_triangle(hdc, x0 + listbox_sb_width / 2, listbox_sb_width / 2, 4, dark, false);
    fill_triangle(hdc, x0 + listbox_sb_width / 2, height - listbox_sb_width / 2, 4, dark, true);
    int32_t th = 0;
    const int32_t top = listbox_thumb(c, w, &th);
    bevel(top, top + th);
    gdi::fill(hdc, {x0 + 4, top + 3, w->w - 4, top + 4}, shadow);
}
// A listbox control with strings (LBS_HASSTRINGS): paint the rows top to
// bottom and highlight the selected one, each at the row height above. An
// owner-draw listbox instead hands each row to the parent's WM_DRAWITEM.
void paint_listbox(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc)
        return;
    const uint32_t caller_font = dc->font;
    const int caller_bk_mode = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    gdi::fill(hdc, {0, 0, width, height}, 0x00ffffff);
    const bool sb = listbox_has_scrollbar(w);
    const int32_t row_width = width - (sb ? listbox_sb_width : 0);
    bool drawn = owner_draw_listbox(c, w, hwnd, hdc, row_width, height);
    if (drawn) {
        if (sb)
            paint_listbox_scrollbar(c, w, hdc);
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        else if (auto *caller = gdi::dc_of(hdc)) {
            caller->font = caller_font;
            caller->bk_mode = caller_bk_mode;
        }
        return;
    }
    if (auto *paint_dc = gdi::dc_of(hdc))
        paint_dc->bk_mode = 1; // TRANSPARENT: the row background is what was just filled
    int32_t y = 0;
    for (int32_t i = w->list_top; i < int32_t(w->list_items.size()) && y < height; ++i) {
        int32_t ih = listbox_item_height(c, w, i);
        const bool selected = w->list_selected.count(i) != 0;
        const bool caret = i == w->list_sel && focus_window() == hwnd;
        if (auto *paint_dc = gdi::dc_of(hdc))
            paint_dc->text_color = selected ? 0x00ffffff : 0x00000000;
        if (selected)
            gdi::fill(hdc, {0, y, row_width, y + ih}, 0x00c00000);
        if (caret) {
            // The caret row carries a focus rectangle, not a selection
            // highlight: a one-pixel black outline inset from the row (Windows
            // dithers it; a solid edge reads the same at this size).
            const uint32_t ink = 0x00000000;
            gdi::fill(hdc, {1, y + 1, row_width - 1, y + 2}, ink);
            gdi::fill(hdc, {1, y + ih - 2, row_width - 1, y + ih - 1}, ink);
            gdi::fill(hdc, {1, y + 1, 2, y + ih - 1}, ink);
            gdi::fill(hdc, {row_width - 2, y + 1, row_width - 1, y + ih - 1}, ink);
        }
        uint32_t units = wide_units(w->list_items[i]);
        if (!units) {
            y += ih;
            continue;
        }
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            wr32(block + 0, 2);
            wr32(block + 4, y);
            wr32(block + 8, uint32_t(row_width - 2));
            wr32(block + 12, y + ih);
            gm_put_wstr(block + 16, w->list_items[i], units + 1);
            gdi::draw_text(hdc, block + 16, units, block, 0x24 /* DT_SINGLELINE|DT_VCENTER */);
            heap_free(block);
        }
        y += ih;
    }
    if (sb)
        paint_listbox_scrollbar(c, w, hdc);
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk_mode;
    }
}
// An EDIT control. The dialog engine's generic control path knows only a STATIC
// caption, so an edit's own text (set with SetDlgItemTextA/WM_SETTEXT - Petz 5's
// profile fills both of its multi-line edits with the pet's information) was
// never painted, and neither was the field itself. Windows draws the client in
// the parent's brush (WM_CTLCOLOREDIT, or WM_CTLCOLORSTATIC for a read-only
// edit; COLOR_WINDOW when the dialog procedure answers nothing) inside a sunken
// border, with the text left-aligned, clipped to the field, in the control's
// WM_SETFONT face. Caret and selection are not modelled (TD-049).
// An edit control's text model. The caret and the selection anchor are byte
// indices into title_utf8; every operation clamps them to the current text so a
// WM_SETTEXT that shrinks the string cannot leave a caret past its end.
void edit_clamp(Window *w) {
    const int32_t n = int32_t(w->title_utf8.size());
    if (w->caret < 0)
        w->caret = 0;
    if (w->caret > n)
        w->caret = n;
    if (w->sel_anchor < 0)
        w->sel_anchor = 0;
    if (w->sel_anchor > n)
        w->sel_anchor = n;
}
int32_t edit_sel_begin(const Window *w) {
    return w->sel_anchor < w->caret ? w->sel_anchor : w->caret;
}
int32_t edit_sel_end(const Window *w) {
    return w->sel_anchor < w->caret ? w->caret : w->sel_anchor;
}
void edit_delete_selection(Window *w) {
    edit_clamp(w);
    const int32_t a = edit_sel_begin(w), b = edit_sel_end(w);
    if (a == b)
        return;
    w->title_utf8.erase(size_t(a), size_t(b - a));
    w->caret = w->sel_anchor = a;
}
// Put the caret at `pos`, extending the selection to it when `select` (Shift).
void edit_move_caret(Window *w, int32_t pos, bool select) {
    const int32_t n = int32_t(w->title_utf8.size());
    if (pos < 0)
        pos = 0;
    if (pos > n)
        pos = n;
    w->caret = pos;
    if (!select)
        w->sel_anchor = pos;
    w->update_pending = true;
}
void edit_insert(Window *w, char ch) {
    edit_clamp(w);
    edit_delete_selection(w);
    w->title_utf8.insert(w->title_utf8.begin() + w->caret, ch);
    ++w->caret;
    w->sel_anchor = w->caret;
    w->update_pending = true;
}
void edit_backspace(Window *w) {
    edit_clamp(w);
    if (edit_sel_begin(w) != edit_sel_end(w)) {
        edit_delete_selection(w);
    } else if (w->caret > 0) {
        w->title_utf8.erase(size_t(w->caret - 1), 1);
        --w->caret;
        w->sel_anchor = w->caret;
    }
    w->update_pending = true;
}
void edit_delete(Window *w) {
    edit_clamp(w);
    if (edit_sel_begin(w) != edit_sel_end(w)) {
        edit_delete_selection(w);
    } else if (w->caret < int32_t(w->title_utf8.size())) {
        w->title_utf8.erase(size_t(w->caret), 1);
    }
    w->update_pending = true;
}
// The caret index nearest a client x: walk the string and take the boundary
// whose midpoint the point is on. The same advance the paint lays the text out
// with, so a click lands where the glyph under it is.
int32_t edit_index_from_x(uint32_t hdc, const Window *w, int32_t x) {
    const std::string &s = w->title_utf8;
    int32_t best = 0, best_dist = INT32_MAX;
    for (size_t i = 0; i <= s.size(); ++i) {
        const int32_t cx = gdi::text_width(hdc, s.substr(0, i));
        const int32_t dist = cx > x ? cx - x : x - cx;
        if (dist < best_dist) {
            best_dist = dist;
            best = int32_t(i);
        }
        if (cx > x)
            break;
    }
    return best;
}
void paint_edit(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    edit_clamp(w);
    const int32_t width = w->w, height = w->h;
    const uint32_t parent = w->parent;
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc)
        return;
    const uint32_t caller_font = dc->font;
    const int caller_bk_mode = dc->bk_mode;
    if (w->font)
        dc->font = w->font; // the dialog's WM_SETFONT face, or the control's own
    const bool readonly = (w->style & 0x0800u) != 0; // ES_READONLY
    uint32_t brush = parent ? host_dispatch_to_wndproc(c, parent,
                                                       readonly ? 0x0138u /*WM_CTLCOLORSTATIC*/
                                                                : 0x0133u /*WM_CTLCOLOREDIT*/,
                                                       hdc, hwnd)
                            : 0;
    uint32_t pixel = gdi::argb(0x00ffffff); // COLOR_WINDOW when the dialog answers nothing
    if (brush)
        gdi::brush_color(brush, &pixel);
    int32_t inset = 0;
    gdi::fill(hdc, {0, 0, width, height}, pixel);
    if (w->style & 0x00800000u) {                    // WS_BORDER: a one-pixel sunken frame
        const uint32_t edge = gdi::argb(0x00808080); // COLOR_3DSHADOW
        gdi::fill(hdc, {0, 0, width, 1}, edge);
        gdi::fill(hdc, {0, 0, 1, height}, edge);
        gdi::fill(hdc, {0, height - 1, width, height}, edge);
        gdi::fill(hdc, {width - 1, 0, width, height}, edge);
        inset = 1;
    }
    if (w->exstyle & 0x00000200u) { // WS_EX_CLIENTEDGE: a two-pixel 3D edge
        const uint32_t shadow = gdi::argb(0x00808080), light = gdi::argb(0x00ffffff);
        gdi::fill(hdc, {inset, inset, width - inset, inset + 1}, shadow);
        gdi::fill(hdc, {inset, inset, inset + 1, height - inset}, shadow);
        gdi::fill(hdc, {inset, height - inset - 1, width - inset, height - inset}, light);
        gdi::fill(hdc, {width - inset - 1, inset, width - inset, height - inset}, light);
        inset += 2;
    }
    const bool multiline = (w->style & 0x0004u) != 0; // ES_MULTILINE
    const bool focused = focus_window() == hwnd;
    // A one-line edit's caret/selection geometry. The text is drawn vertically
    // centred (DT_VCENTER), so the caret band sits at the same centre.
    const int32_t tx = inset + 2, line_h = gdi::text_height(hdc);
    const int32_t ty = multiline ? inset + 1 : (height - line_h) / 2;
    const int32_t sel_a = edit_sel_begin(w), sel_b = edit_sel_end(w);
    if (!multiline && sel_a != sel_b && line_h > 0) {
        const int32_t xa = tx + gdi::text_width(hdc, text.substr(0, size_t(sel_a)));
        const int32_t xb = tx + gdi::text_width(hdc, text.substr(0, size_t(sel_b)));
        // A focused edit's selection is the system highlight (navy); an
        // unfocused one is the grey Windows uses for an inactive selection.
        const uint32_t hi = focused ? gdi::argb(0x00a80000) : gdi::argb(0x00c0c0c0);
        gdi::fill(hdc, {xa, ty, xb, ty + line_h}, hi);
    }
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (units && units < (UINT32_MAX - 32) / 2 && width > 2 * inset && height > 2 * inset) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            dc->text_color = 0x00000000;
            dc->bk_mode = 1; // TRANSPARENT over the field just filled
            wr32(block + 0, uint32_t(tx));
            wr32(block + 4, uint32_t(ty));
            wr32(block + 8, uint32_t(width - inset - 1));
            wr32(block + 12, uint32_t(height - inset));
            gm_put_wstr(block + 16, text, units + 1);
            uint32_t flags = 0x800 | 0x40; // DT_NOPREFIX | DT_EXPANDTABS
            if (multiline)
                flags |= 0x10; // DT_WORDBREAK
            else
                flags |= 0x20 | 0x4; // DT_SINGLELINE | DT_VCENTER (DT_LEFT is 0)
            gdi::draw_text(hdc, block + 16, units, block, flags);
            // The selected run again, white, clipped to its own highlight band.
            if (!multiline && sel_a != sel_b && focused && line_h > 0) {
                std::string sel = text.substr(size_t(sel_a), size_t(sel_b - sel_a));
                uint32_t sunits = wide_units(sel);
                if (sunits && sunits < (UINT32_MAX - 32) / 2) {
                    uint32_t sblock = heap_alloc(16 + 2 * (sunits + 1), true);
                    if (sblock) {
                        const int32_t xa = tx + gdi::text_width(hdc, text.substr(0, size_t(sel_a)));
                        const int32_t xb = tx + gdi::text_width(hdc, text.substr(0, size_t(sel_b)));
                        dc->text_color = 0x00ffffff;
                        wr32(sblock + 0, uint32_t(xa));
                        wr32(sblock + 4, uint32_t(ty));
                        wr32(sblock + 8, uint32_t(xb));
                        wr32(sblock + 12, uint32_t(ty + line_h));
                        gm_put_wstr(sblock + 16, sel, sunits + 1);
                        gdi::draw_text(hdc, sblock + 16, sunits, sblock, 0x800 | 0x40 | 0x20);
                        heap_free(sblock);
                    }
                }
            }
            heap_free(block);
        }
    }
    // The caret: a one-pixel bar at the insertion point while the edit has the
    // keyboard focus. It does not blink yet (TD-095).
    if (!multiline && focused && line_h > 0) {
        const int32_t cx = tx + gdi::text_width(hdc, text.substr(0, size_t(w->caret)));
        if (cx >= inset && cx < width)
            gdi::fill(hdc, {cx, ty, cx + 1, ty + line_h}, gdi::argb(0x00000000));
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) { // the caller's DC keeps its own state
        caller->font = caller_font;
        caller->bk_mode = caller_bk_mode;
    }
}
// ---------------------------------------------------------------------------
// The range controls and the common controls (trackbar, combo box, progress,
// up-down, tab, scrollbar). Each is a control with its own class name, painted
// into the parent's surface (a control has no surface of its own) and driven by
// its own message set. They share the list model (`list_items`) and the range
// model (`range_min`/`range_max`/`range_pos`) on Window.
// ---------------------------------------------------------------------------
// The background pixel for a control that uses the static colour protocol: the
// parent's WM_CTLCOLORSTATIC brush, else the dialog's WM_CTLCOLORDLG brush, else
// the classic COLOR_BTNFACE. False when the parent chose no brush at all, in
// which case the caller leaves what is underneath. The window is re-found after
// the dispatch because a guest handler may destroy it.
bool control_background(X86 *c, uint32_t hwnd, uint32_t hdc, uint32_t *pixel) {
    *pixel = gdi::argb(0x00c8d0d4); // COLOR_BTNFACE, RGB(0xD4,0xD0,0xC8)
    Window *w = find_window(hwnd);
    if (!w || !w->parent)
        return false;
    const uint32_t parent = w->parent;
    uint32_t brush = host_dispatch_to_wndproc(c, parent, 0x0138 /*WM_CTLCOLORSTATIC*/, hdc, hwnd);
    if (brush && gdi::brush_color(brush, pixel))
        return true;
    if (Window *pw = find_window(parent); pw && pw->cls == "#32770") {
        uint32_t b = host_dispatch_to_wndproc(c, parent, 0x0131 /*WM_CTLCOLORDLG*/, hdc, parent);
        if (b && gdi::brush_color(b, pixel))
            return true;
        return true; // the dialog face default
    }
    return false;
}
// A control's orientation: TBS_VERT (2) for a trackbar, SBS_VERT (1) for a
// scrollbar, horizontal otherwise.
bool range_vertical(const Window *w) {
    if (w->cls == "msctls_trackbar32")
        return (w->style & 2u) != 0;
    if (w->cls == "scrollbar")
        return (w->style & 1u) != 0;
    return false;
}
int32_t range_thumb_length(const Window *w) {
    return w->cls == "msctls_trackbar32" ? 11 : 16;
}
int32_t range_clamp(const Window *w, int32_t pos) {
    if (pos < w->range_min)
        return w->range_min;
    if (pos > w->range_max)
        return w->range_max;
    return pos;
}
// The coordinate (along the axis) of the thumb's leading edge for the current
// position: 0 at the min end. A vertical control's min is at the bottom, so the
// coordinate is measured from the bottom there.
int32_t range_thumb_start(const Window *w) {
    const bool vert = range_vertical(w);
    const int32_t span = (vert ? w->h : w->w) - range_thumb_length(w);
    const int32_t range = w->range_max - w->range_min;
    if (span <= 0 || range <= 0)
        return 0;
    int32_t t = int32_t((int64_t(w->range_pos - w->range_min) * span + range / 2) / range);
    if (t < 0)
        t = 0;
    if (t > span)
        t = span;
    return vert ? span - t : t;
}
// The coordinate of a value's tick centre, for the tick marks.
int32_t range_value_coord(const Window *w, int32_t value) {
    const bool vert = range_vertical(w);
    const int32_t thumb = range_thumb_length(w);
    const int32_t span = (vert ? w->h : w->w) - thumb;
    const int32_t range = w->range_max - w->range_min;
    if (span <= 0 || range <= 0)
        return thumb / 2;
    int32_t t = int32_t((int64_t(value - w->range_min) * span + range / 2) / range);
    if (t < 0)
        t = 0;
    if (t > span)
        t = span;
    return (vert ? span - t : t) + thumb / 2;
}
// The position a pointer at client (x,y) asks for.
int32_t range_pos_from_point(const Window *w, int32_t x, int32_t y) {
    const bool vert = range_vertical(w);
    const int32_t thumb = range_thumb_length(w);
    const int32_t span = (vert ? w->h : w->w) - thumb;
    const int32_t range = w->range_max - w->range_min;
    if (span <= 0 || range <= 0)
        return w->range_min;
    int32_t along = vert ? (w->h - thumb / 2 - y) : (x - thumb / 2);
    if (along < 0)
        along = 0;
    if (along > span)
        along = span;
    return w->range_min + int32_t((int64_t(along) * range + span / 2) / span);
}
// A trackbar's page step: TBM_SETPAGESIZE's value, else the class default of a
// fifth of the range.
int32_t trackbar_page(const Window *w) {
    if (w->range_page > 0)
        return w->range_page;
    const int32_t range = w->range_max - w->range_min;
    return range > 5 ? range / 5 : 1;
}
// A trackbar/scrollbar move reports WM_HSCROLL or WM_VSCROLL to its parent, with
// the notification code in the low word and (for the thumb codes) the position
// in the high word.
void range_notify(X86 *c, uint32_t hwnd, uint32_t code, int32_t pos) {
    Window *w = find_window(hwnd);
    if (!w || !w->parent)
        return;
    const uint32_t msg = range_vertical(w) ? 0x0115u /*WM_VSCROLL*/ : 0x0114u /*WM_HSCROLL*/;
    const uint32_t hi = (code == 4 || code == 5) ? uint32_t(uint16_t(pos)) : 0u;
    host_dispatch_to_wndproc(c, w->parent, msg, (hi << 16) | (code & 0xffffu), hwnd);
}
void paint_trackbar(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const bool vert = range_vertical(w);
    const uint32_t style = w->style;
    const int32_t lo = w->range_min, hi = w->range_max;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    uint32_t face = gdi::argb(0x00c8d0d4);
    control_background(c, hwnd, hdc, &face);
    gdi::fill(hdc, {0, 0, width, height}, face);
    if (!(w = find_window(hwnd))) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t shadow = gdi::argb(0x00808080), dark = gdi::argb(0x00404040),
                   light = gdi::argb(0x00ffffff);
    const int32_t thumb = range_thumb_length(w);
    // The channel: a 4 px sunken groove down the middle.
    if (vert) {
        const int32_t cx = width / 2;
        gdi::fill(hdc, {cx - 2, thumb / 2, cx + 2, height - thumb / 2}, shadow);
        gdi::fill(hdc, {cx - 2, thumb / 2, cx - 1, height - thumb / 2}, dark);
    } else {
        const int32_t cy = height / 2;
        gdi::fill(hdc, {thumb / 2, cy - 2, width - thumb / 2, cy + 2}, shadow);
        gdi::fill(hdc, {thumb / 2, cy - 2, width - thumb / 2, cy - 1}, dark);
    }
    // Ticks, unless TBS_NOTICKS (0x10). TBS_AUTOTICKS (1) puts one at every
    // step; the runtime draws at most eleven so a wide range stays readable.
    if (!(style & 0x10u)) {
        const int32_t range = hi - lo;
        int32_t step = range > 0 ? (range + 10) / 11 : 1;
        if (step < 1)
            step = 1;
        const bool both = (style & 8u) != 0; // TBS_BOTH
        const bool top = (style & 4u) != 0;  // TBS_TOP (left when vertical)
        for (int32_t v = lo; v <= hi; v += step) {
            const int32_t coord = range_value_coord(w, v);
            if (vert) {
                if (both || top)
                    gdi::fill(hdc, {width / 2 - 6, coord, width / 2 - 2, coord + 1}, shadow);
                if (both || !top)
                    gdi::fill(hdc, {width / 2 + 2, coord, width / 2 + 6, coord + 1}, shadow);
            } else {
                if (both || !top)
                    gdi::fill(hdc, {coord, height / 2 + 2, coord + 1, height / 2 + 6}, shadow);
                if (both || top)
                    gdi::fill(hdc, {coord, height / 2 - 6, coord + 1, height / 2 - 2}, shadow);
            }
        }
        if (range > 0 && (hi - lo) % step != 0) {
            const int32_t coord = range_value_coord(w, hi);
            if (vert)
                gdi::fill(hdc, {width / 2 - 6, coord, width / 2 - 2, coord + 1}, shadow);
            else
                gdi::fill(hdc, {coord, height / 2 + 2, coord + 1, height / 2 + 6}, shadow);
        }
    }
    // The thumb: a raised 11 px box spanning the control's cross axis, with a
    // grip line down its middle.
    const int32_t ts = range_thumb_start(w);
    int32_t tl, tt, tr, tb;
    if (vert) {
        tl = 0;
        tr = width;
        tt = ts;
        tb = ts + thumb;
    } else {
        tl = ts;
        tr = ts + thumb;
        tt = 0;
        tb = height;
    }
    gdi::fill(hdc, {tl, tt, tr, tb}, face);
    gdi::fill(hdc, {tl, tt, tr, tt + 1}, light);
    gdi::fill(hdc, {tl, tt, tl + 1, tb}, light);
    gdi::fill(hdc, {tl, tb - 1, tr, tb}, dark);
    gdi::fill(hdc, {tr - 1, tt, tr, tb}, dark);
    if (vert)
        gdi::fill(hdc, {width / 2 - 1, tt + 3, width / 2, tb - 3}, shadow);
    else
        gdi::fill(hdc, {tl + 3, height / 2 - 1, tr - 3, height / 2}, shadow);
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
// A combo box's row height in the drop-down: CB_SETITEMHEIGHT's value, else the
// 8x16 system line.
int32_t combo_row_height(const Window *w) {
    return w->list_item_height > 0 ? w->list_item_height : 16;
}
// The height of an open combo box's drop-down list in pixels: the template's
// dropped height when there is one, else the rows plus a 2 px border.
int32_t combo_drop_height(const Window *w) {
    const int32_t rh = combo_row_height(w);
    int32_t h = int32_t(w->list_items.size()) * rh + 2;
    if (w->combo_drop_h > w->h)
        h = w->combo_drop_h - w->h;
    return h < rh + 2 ? rh + 2 : h;
}
// The row under client y in the popup list (y = 1 + row*row_height), or -1.
int32_t combo_popup_row(const Window *w, int32_t y) {
    const int32_t rh = combo_row_height(w);
    const int32_t row = y < 1 ? -1 : (y - 1) / (rh > 0 ? rh : 1);
    if (row < 0 || row >= int32_t(w->list_items.size()))
        return -1;
    return row;
}
// The popup list's paint: a white field with a sunken border, the rows, and the
// selected/hot row highlighted. It is its own window, so the list never writes
// into the dialog's surface.
void paint_combo_popup(X86 *c, uint32_t hwnd, uint32_t into) {
    (void)c;
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const int32_t rh = combo_row_height(w);
    const std::vector<std::string> items = w->list_items;
    const int32_t sel = w->list_sel, hot = w->combo_hot;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    const uint32_t white = gdi::argb(0x00ffffff), shadow = gdi::argb(0x00808080),
                   dark = gdi::argb(0x00404040), navy = gdi::argb(0x00a80000);
    gdi::fill(hdc, {0, 0, width, height}, white);
    gdi::fill(hdc, {0, 0, width, 1}, shadow);
    gdi::fill(hdc, {0, 0, 1, height}, shadow);
    gdi::fill(hdc, {0, height - 1, width, height}, dark);
    gdi::fill(hdc, {width - 1, 0, width, height}, dark);
    dc->bk_mode = 1; // TRANSPARENT
    for (size_t i = 0; i < items.size(); ++i) {
        const int32_t y = 1 + int32_t(i) * rh;
        if (y + rh > height - 1)
            break;
        const bool selected = int32_t(i) == sel;
        const bool hotrow = int32_t(i) == hot;
        if (selected || hotrow) {
            gdi::fill(hdc, {1, y, width - 1, y + rh}, navy);
            dc->text_color = 0x00ffffff;
        } else {
            dc->text_color = 0x00000000;
        }
        uint32_t units = wide_units(items[i]);
        if (units && units < (UINT32_MAX - 32) / 2) {
            if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
                wr32(block + 0, 2);
                wr32(block + 4, uint32_t(y));
                wr32(block + 8, uint32_t(width - 2));
                wr32(block + 12, uint32_t(y + rh));
                gm_put_wstr(block + 16, items[i], units + 1);
                gdi::draw_text(hdc, block + 16, units, block,
                               0x24 /*DT_SINGLELINE|DT_VCENTER*/ | 0x800 /*DT_NOPREFIX*/);
                heap_free(block);
            }
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
// Close an open drop-down: destroy the popup (which repaints whatever it
// covered) and clear the combo's state. `c` is needed for the WM_DESTROY.
void combo_close_popup(X86 *c, uint32_t combo_hwnd) {
    Window *w = find_window(combo_hwnd);
    if (!w)
        return;
    const uint32_t pop = w->combo_popup;
    w->combo_open = false;
    w->combo_hot = -1;
    w->combo_popup = 0;
    w->update_pending = true;
    if (pop) {
        if (capture_window() == pop)
            set_capture_window(0);
        destroy_window(c, pop);
    }
}
// The drop-down popup's window procedure: it paints itself and, on a press,
// selects the row under the pointer (or dismisses when the point is off the
// list) and reports CBN_SELCHANGE to the combo's parent.
void combo_drop_proc(X86 *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000f: // WM_PAINT
    case 0x0318: // WM_PRINTCLIENT
        paint_combo_popup(c, hwnd, arg(c, 2));
        set_eax(c, 0);
        return;
    case 0x0200: { // WM_MOUSEMOVE: the hot row follows the pointer
        if (w) {
            const int32_t row = combo_popup_row(w, int16_t((arg(c, 3) >> 16) & 0xffff));
            if (row != w->combo_hot) {
                w->combo_hot = row;
                w->update_pending = true;
            }
        }
        set_eax(c, 0);
        return;
    }
    case 0x0201: { // WM_LBUTTONDOWN: select a row, or dismiss
        const uint32_t owner = w ? w->owner : 0;
        const int32_t row = w ? combo_popup_row(w, int16_t((arg(c, 3) >> 16) & 0xffff)) : -1;
        uint32_t parent = 0, cid = 0;
        if (owner) {
            Window *combo = find_window(owner);
            if (combo) {
                parent = combo->parent;
                cid = combo->id;
                if (row >= 0) {
                    combo->list_sel = row;
                    if ((combo->style & 3u) == 3u /*CBS_DROPDOWNLIST*/)
                        combo->title_utf8 = combo->list_items[row];
                    combo->update_pending = true;
                }
            }
            combo_close_popup(c, owner);
            if (row >= 0 && parent)
                host_dispatch_to_wndproc(c, parent, 0x0111 /*WM_COMMAND*/,
                                         (1u << 16) | (cid & 0xffffu), owner); // CBN_SELCHANGE
        }
        set_eax(c, 0);
        return;
    }
    case 0x0202: // WM_LBUTTONUP
        set_eax(c, 0);
        return;
    default:
        def_window_proc(c, false);
    }
}
uint32_t combo_drop_proc_addr() {
    static uint32_t addr = 0;
    if (!addr)
        addr =
            imports_alloc_trampoline("USER32.dll", "combo drop-down procedure", combo_drop_proc, 4);
    return addr;
}
// Open or close a combo box's drop-down. The list is a top-level popup window
// with its own surface placed just below the closed field, owned by the combo
// (so destroying the combo destroys the list) and holding the pointer capture so
// a click anywhere selects a row or dismisses the list.
void combo_show_dropdown(X86 *c, uint32_t hwnd, bool open) {
    Window *w = find_window(hwnd);
    if (!w || w->combo_open == open)
        return;
    const uint32_t parent = w->parent, id = w->id;
    if (open) {
        int32_t sx = 0, sy = 0;
        client_origin(hwnd, &sx, &sy);
        const int32_t drop = combo_drop_height(w);
        uint32_t pop = g_next_hwnd;
        g_next_hwnd += 4;
        Window p;
        p.hwnd = pop;
        p.wndproc = combo_drop_proc_addr();
        p.style = 0x80000000u /*WS_POPUP*/ | WS_VISIBLE;
        p.cls = "comboboxdrop";
        p.x = sx;
        p.y = sy + w->h;
        p.w = w->w;
        p.h = drop;
        p.owner = hwnd;
        p.id = id;
        p.thread = guest_current_thread_id();
        p.visible = true;
        p.shown = true;
        p.update_pending = true;
        p.list_items = w->list_items;
        p.list_sel = w->list_sel;
        p.font = w->font;
        windows()[pop] = p;
        window_created(pop);
        w->combo_popup = pop;
        w->combo_open = true;
        w->combo_hot = -1;
        w->update_pending = true;
        set_capture_window(pop);
    } else {
        combo_close_popup(c, hwnd);
    }
    // CBN_DROPDOWN (7) / CBN_CLOSEUP (8) to the parent.
    if (parent)
        host_dispatch_to_wndproc(c, parent, 0x0111 /*WM_COMMAND*/,
                                 (uint32_t(open ? 7u : 8u) << 16) | (id & 0xffffu), hwnd);
}
void paint_combobox(X86 *c, uint32_t hwnd, uint32_t into) {
    (void)c;
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    const uint32_t white = gdi::argb(0x00ffffff), shadow = gdi::argb(0x00808080),
                   dark = gdi::argb(0x00404040), face = gdi::argb(0x00c8d0d4);
    // The closed field: white inside a 2 px sunken border, the selected item's
    // text left-aligned, and the arrow button on the right.
    const int32_t arrow = 17;
    gdi::fill(hdc, {0, 0, width, height}, white);
    gdi::fill(hdc, {0, 0, width, 1}, shadow);
    gdi::fill(hdc, {0, 0, 1, height}, shadow);
    gdi::fill(hdc, {0, height - 1, width, height}, dark);
    gdi::fill(hdc, {width - 1, 0, width, height}, dark);
    // The arrow button (a raised bevel with a down triangle).
    const int32_t ax0 = width - arrow - 1;
    gdi::fill(hdc, {ax0, 1, width - 1, height - 1}, face);
    gdi::fill(hdc, {ax0, 1, width - 1, 2}, gdi::argb(0x00ffffff));
    gdi::fill(hdc, {ax0, 1, ax0 + 1, height - 1}, gdi::argb(0x00ffffff));
    gdi::fill(hdc, {ax0, height - 2, width - 1, height - 1}, dark);
    gdi::fill(hdc, {width - 2, 1, width - 1, height - 1}, dark);
    fill_triangle(hdc, ax0 + arrow / 2, height / 2, 4, gdi::argb(0x00000000), true);
    if (auto *paint_dc = gdi::dc_of(hdc)) {
        paint_dc->bk_mode = 1; // TRANSPARENT
        paint_dc->text_color = 0x00000000;
    }
    if (!text.empty()) {
        uint32_t units = wide_units(text);
        if (units && units < (UINT32_MAX - 32) / 2) {
            if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
                wr32(block + 0, 3);
                wr32(block + 4, 0);
                wr32(block + 8, uint32_t(ax0 - 1));
                wr32(block + 12, uint32_t(height));
                gm_put_wstr(block + 16, text, units + 1);
                gdi::draw_text(hdc, block + 16, units, block,
                               0x0 /*DT_LEFT*/ | 0x4 /*DT_VCENTER*/ | 0x20 /*DT_SINGLELINE*/ |
                                   0x800 /*DT_NOPREFIX*/);
                heap_free(block);
            }
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
void paint_progress(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const int32_t lo = w->range_min, hi = w->range_max, pos = w->range_pos;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    const uint32_t face = gdi::argb(0x00c8d0d4), shadow = gdi::argb(0x00808080),
                   light = gdi::argb(0x00ffffff), bar = gdi::argb(0x00a80000);
    gdi::fill(hdc, {0, 0, width, height}, face);
    // The sunken field, then the bar filling from the left.
    const int32_t in = 2;
    gdi::fill(hdc, {0, 0, width, height}, face);
    gdi::fill(hdc, {in, in, width - in, height - in}, gdi::argb(0x00ffffff));
    gdi::fill(hdc, {0, 0, width, 1}, shadow);
    gdi::fill(hdc, {0, 0, 1, height}, shadow);
    gdi::fill(hdc, {0, height - 1, width, height}, light);
    gdi::fill(hdc, {width - 1, 0, width, height}, light);
    const int32_t range = hi - lo;
    if (range > 0 && width > 2 * in) {
        int64_t frac = int64_t(pos - lo) * (width - 2 * in) / range;
        if (frac < 0)
            frac = 0;
        if (frac > width - 2 * in)
            frac = width - 2 * in;
        gdi::fill(hdc, {in, in, in + int32_t(frac), height - in}, bar);
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
void paint_spin(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    const uint32_t face = gdi::argb(0x00c8d0d4), light = gdi::argb(0x00ffffff),
                   dark = gdi::argb(0x00000000);
    gdi::fill(hdc, {0, 0, width, height}, face);
    // Two arrow halves. The up half is above, the down half below; a 1 px
    // raised edge around the pair.
    const int32_t mid = height / 2;
    gdi::fill(hdc, {0, 0, width, 1}, light);
    gdi::fill(hdc, {0, 0, 1, height}, light);
    gdi::fill(hdc, {0, height - 1, width, height}, dark);
    gdi::fill(hdc, {width - 1, 0, width, height}, dark);
    gdi::fill(hdc, {1, mid, width - 1, mid + 1}, dark);
    fill_triangle(hdc, width / 2, mid / 2, 4, gdi::argb(0x00000000), false);
    fill_triangle(hdc, width / 2, mid + (height - mid) / 2, 4, gdi::argb(0x00000000), true);
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
void paint_tab(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const std::vector<std::string> items = w->list_items;
    const int32_t sel = w->list_sel;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    if (w->font)
        dc->font = w->font;
    const uint32_t face = gdi::argb(0x00c8d0d4), light = gdi::argb(0x00ffffff),
                   dark = gdi::argb(0x00000000), shadow = gdi::argb(0x00808080);
    const int32_t tab_h = 20;
    gdi::fill(hdc, {0, 0, width, height}, face);
    if (auto *paint_dc = gdi::dc_of(hdc)) {
        paint_dc->bk_mode = 1;
        paint_dc->text_color = 0x00000000;
    }
    // The body: a sunken rect below the tab strip.
    gdi::fill(hdc, {0, tab_h, width, height}, light);
    gdi::fill(hdc, {0, tab_h, 1, height}, shadow);
    gdi::fill(hdc, {0, height - 1, width, height}, dark);
    gdi::fill(hdc, {width - 1, tab_h, width, height}, dark);
    gdi::fill(hdc, {0, tab_h, width, tab_h + 1}, shadow);
    // Each tab: a raised box; the selected one is taller and joins the body.
    int32_t x = 2;
    for (size_t i = 0; i < items.size(); ++i) {
        uint32_t units = items[i].empty() ? 0 : wide_units(items[i]);
        const int32_t tw = (units ? gdi::text_width(hdc, items[i]) : 0) + 12;
        const bool selected = int32_t(i) == sel;
        const int32_t ty = selected ? 1 : 3;
        const int32_t tx1 = x + tw;
        gdi::fill(hdc, {x, ty, tx1, tab_h + (selected ? 1 : 0)}, face);
        gdi::fill(hdc, {x, ty, tx1, ty + 1}, light);
        gdi::fill(hdc, {x, ty, x + 1, tab_h}, light);
        gdi::fill(hdc, {tx1 - 1, ty, tx1, tab_h}, dark);
        if (!selected)
            gdi::fill(hdc, {x, tab_h - 1, tx1, tab_h}, dark);
        if (units && units < (UINT32_MAX - 32) / 2) {
            if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
                wr32(block + 0, x + 6);
                wr32(block + 4, ty);
                wr32(block + 8, tx1 - 4);
                wr32(block + 12, tab_h);
                gm_put_wstr(block + 16, items[i], units + 1);
                gdi::draw_text(hdc, block + 16, units, block,
                               0x1 /*DT_CENTER*/ | 0x4 /*DT_VCENTER*/ | 0x20 /*DT_SINGLELINE*/ |
                                   0x800 /*DT_NOPREFIX*/);
                heap_free(block);
            }
        }
        x = tx1 + 2;
        if (x >= width)
            break;
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
// The tab index under client (x,y), or -1. The same widths the paint lays the
// strip out with (the label width plus the padding), so a click selects the tab
// it is over.
int32_t tab_index_from_point(Window *w, int32_t x, int32_t y) {
    if (y < 0 || y >= 20)
        return -1;
    uint32_t hdc = gdi_window_dc(w->hwnd);
    if (!gdi::dc_of(hdc))
        return -1;
    if (w->font)
        if (auto *dc = gdi::dc_of(hdc))
            dc->font = w->font;
    int32_t cx = 2, found = -1;
    for (size_t i = 0; i < w->list_items.size(); ++i) {
        const int32_t tw =
            (w->list_items[i].empty() ? 0 : gdi::text_width(hdc, w->list_items[i])) + 12;
        if (x >= cx && x < cx + tw) {
            found = int32_t(i);
            break;
        }
        cx += tw + 2;
        if (cx >= w->w)
            break;
    }
    gdi_release_window_dc(w->hwnd, hdc);
    return found;
}
void paint_scrollbar(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const bool vert = range_vertical(w);
    const int32_t arrow = 16;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    const uint32_t caller_font = dc->font;
    const int caller_bk = dc->bk_mode;
    const uint32_t face = gdi::argb(0x00c8d0d4), light = gdi::argb(0x00ffffff),
                   dark = gdi::argb(0x00000000), shadow = gdi::argb(0x00808080);
    gdi::fill(hdc, {0, 0, width, height}, face);
    const int32_t span = (vert ? height : width) - 2 * arrow;
    const int32_t range = w->range_max - w->range_min;
    const int32_t thumb_len = 16;
    int32_t tpos = 0;
    if (span > 0 && range > 0) {
        const int32_t track = span - thumb_len;
        if (track > 0) {
            int32_t t = int32_t((int64_t(w->range_pos - w->range_min) * track + range / 2) / range);
            if (t < 0)
                t = 0;
            if (t > track)
                t = track;
            tpos = t;
        }
    }
    // The two arrow boxes and the thumb, each a raised bevel.
    auto bevel = [&](int32_t l, int32_t t, int32_t r, int32_t b) {
        gdi::fill(hdc, {l, t, r, b}, face);
        gdi::fill(hdc, {l, t, r, t + 1}, light);
        gdi::fill(hdc, {l, t, l + 1, b}, light);
        gdi::fill(hdc, {l, b - 1, r, b}, dark);
        gdi::fill(hdc, {r - 1, t, r, b}, dark);
    };
    if (vert) {
        bevel(0, 0, width, arrow);
        bevel(0, height - arrow, width, height);
        const int32_t t0 = arrow + tpos;
        bevel(0, t0, width, t0 + thumb_len);
        fill_triangle(hdc, width / 2, arrow / 2, 4, gdi::argb(0x00000000), false);
        fill_triangle(hdc, width / 2, height - arrow / 2, 4, gdi::argb(0x00000000), true);
        gdi::fill(hdc, {width / 2 - 2, t0 + 3, width / 2 + 2, t0 + 4}, shadow);
        gdi::fill(hdc, {width / 2 - 2, t0 + 6, width / 2 + 2, t0 + 7}, shadow);
    } else {
        bevel(0, 0, arrow, height);
        bevel(width - arrow, 0, width, height);
        const int32_t t0 = arrow + tpos;
        bevel(t0, 0, t0 + thumb_len, height);
        fill_triangle(hdc, arrow / 2, height / 2, 4, gdi::argb(0x00000000), false);
        fill_triangle(hdc, width - arrow / 2, height / 2, 4, gdi::argb(0x00000000), true);
        gdi::fill(hdc, {t0 + 3, height / 2 - 2, t0 + 4, height / 2 + 2}, shadow);
        gdi::fill(hdc, {t0 + 6, height / 2 - 2, t0 + 7, height / 2 + 2}, shadow);
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) {
        caller->font = caller_font;
        caller->bk_mode = caller_bk;
    }
}
// Windows' radio group rule (TD-081): BS_AUTORADIOBUTTON/BS_RADIOBUTTON
// controls form a group that starts at a WS_GROUP (0x20000) control and runs to
// the next one in tab order; checking one clears every other button in the same
// group. The siblings are collected first and only their state is changed (no
// guest dispatch), so no window is touched across a callback.
void radio_group_clear(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    if (!w || !w->parent || w->cls != "button")
        return;
    const uint32_t bs = w->style & 0x0fu;
    if (bs != 4 && bs != 9) // BS_RADIOBUTTON / BS_AUTORADIOBUTTON
        return;
    std::vector<uint32_t> sibs;
    for (const auto &kv : windows())
        if (kv.second.parent == w->parent)
            sibs.push_back(kv.first); // std::map order is handle order = creation order
    size_t self = sibs.size();
    for (size_t i = 0; i < sibs.size(); ++i)
        if (sibs[i] == hwnd)
            self = i;
    if (self == sibs.size())
        return;
    size_t start = 0;
    for (size_t j = self + 1; j-- > 0;) {
        Window *s = find_window(sibs[j]);
        if (s && (s->style & 0x20000u /*WS_GROUP*/)) {
            start = j;
            break;
        }
    }
    for (size_t j = start; j < sibs.size(); ++j) {
        if (j > start) {
            Window *s = find_window(sibs[j]);
            if (s && (s->style & 0x20000u))
                break; // the next group begins
        }
        if (sibs[j] == hwnd)
            continue;
        Window *s = find_window(sibs[j]);
        if (s && s->cls == "button" && ((s->style & 0x0fu) == 4 || (s->style & 0x0fu) == 9)) {
            s->check_state = 0;
            s->update_pending = true;
        }
    }
}
// Control windows answer the button-state and text messages the dialog helper
// imports generate; everything else falls through to DefWindowProc.
void control_proc(X86 *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000f: // WM_PAINT
    case 0x0318: // WM_PRINTCLIENT
        if (w && w->cls == "listbox")
            paint_listbox(c, hwnd, arg(c, 2));
        else if (w && w->cls == "button")
            paint_button(c, hwnd, arg(c, 2));
        else if (w && w->cls == "edit")
            paint_edit(c, hwnd, arg(c, 2));
        else if (w && w->cls == "combobox")
            paint_combobox(c, hwnd, arg(c, 2));
        else if (w && w->cls == "msctls_trackbar32")
            paint_trackbar(c, hwnd, arg(c, 2));
        else if (w && w->cls == "msctls_progress32")
            paint_progress(c, hwnd, arg(c, 2));
        else if (w && w->cls == "msctls_updown32")
            paint_spin(c, hwnd, arg(c, 2));
        else if (w && w->cls == "systabcontrol32")
            paint_tab(c, hwnd, arg(c, 2));
        else if (w && w->cls == "scrollbar")
            paint_scrollbar(c, hwnd, arg(c, 2));
        else
            paint_control(c, hwnd, arg(c, 2));
        set_eax(c, 0);
        return;
    case 0x000c: // WM_SETTEXT: the new caption has to be drawn
        def_window_proc(c, w && w->unicode);
        if (Window *after = find_window(hwnd))
            after->update_pending = true;
        return;
    case 0x0102: { // WM_CHAR: an edit control inserts the typed character
        if (w && w->cls == "edit") {
            uint32_t ch = arg(c, 2);
            if (ch == 8) {
                edit_backspace(w);
            } else if (ch == 1) {
                // Ctrl+A: the control code TranslateMessage makes of it. Select
                // the whole text so the next keystroke replaces it.
                w->sel_anchor = 0;
                w->caret = int32_t(w->title_utf8.size());
                w->update_pending = true;
            } else if (ch >= 0x20 && ch < 0x7f) {
                edit_insert(w, char(ch));
            }
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    }
    case 0x0100: { // WM_KEYDOWN: listbox navigation, edit caret/selection
        uint32_t vk = arg(c, 2);
        if (w && w->cls == "edit") {
            // Shift extends the selection; Control+A selects everything. The
            // control key state is the one the host set for the modifier.
            const bool shift = (g_key_state[0x10] & 0x80) != 0;
            const bool ctrl = (g_key_state[0x11] & 0x80) != 0;
            edit_clamp(w);
            switch (vk) {
            case 0x25: // VK_LEFT
                edit_move_caret(w, w->caret - 1, shift);
                break;
            case 0x27: // VK_RIGHT
                edit_move_caret(w, w->caret + 1, shift);
                break;
            case 0x26: // VK_UP: a one-line edit goes to the start
            case 0x24: // VK_HOME
                edit_move_caret(w, 0, shift);
                break;
            case 0x28: // VK_DOWN: a one-line edit goes to the end
            case 0x23: // VK_END
                edit_move_caret(w, int32_t(w->title_utf8.size()), shift);
                break;
            case 0x2e: // VK_DELETE
                edit_delete(w);
                break;
            case 0x08: // VK_BACK
                edit_backspace(w);
                break;
            case 'A':
                if (ctrl) {
                    w->sel_anchor = 0;
                    w->caret = int32_t(w->title_utf8.size());
                    w->update_pending = true;
                }
                break;
            default:
                break;
            }
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "listbox") {
            int32_t n = int32_t(w->list_items.size());
            int32_t sel = w->list_sel;
            bool moved = true;
            switch (vk) {
            case 0x26: // VK_UP
                sel = sel <= 0 ? 0 : sel - 1;
                break;
            case 0x28: // VK_DOWN
                sel = sel < 0 ? 0 : (sel + 1 < n ? sel + 1 : n - 1);
                break;
            case 0x21: // VK_PRIOR
                sel = sel < 0 ? 0 : sel - 5;
                break;
            case 0x22: // VK_NEXT
                sel = sel < 0 ? 0 : sel + 5;
                break;
            case 0x24: // VK_HOME
                sel = 0;
                break;
            case 0x23: // VK_END
                sel = n - 1;
                break;
            default:
                moved = false;
                break;
            }
            if (moved && n > 0) {
                w->list_sel = sel < 0 ? 0 : (sel >= n ? n - 1 : sel);
                // A plain arrow moves the caret AND the selection (Windows
                // replaces the selection on an unmodified arrow); the caret is
                // not itself a selection, so a multi/extended box would show
                // nothing highlighted without this.
                w->list_selected.clear();
                w->list_selected.insert(w->list_sel);
                listbox_scroll_into_view(c, w);
                w->update_pending = true;
                if (w->parent)
                    host_dispatch_to_wndproc(c, w->parent, 0x0111 /* WM_COMMAND */, w->id & 0xffffu,
                                             hwnd); // LBN_SELCHANGE
            }
            set_eax(c, 0);
            return;
        }
        set_eax(c, 0);
        return;
    }
    case 0x0115: { // WM_VSCROLL
        if (w && w->cls == "listbox") {
            uint32_t code = arg(c, 2) & 0xffff;
            int32_t top = w->list_top;
            if (code == 0) // SB_LINEUP
                --top;
            else if (code == 1) // SB_LINEDOWN
                ++top;
            else if (code == 2 || code == 6) // SB_PAGEUP / SB_TOP
                top = 0;
            else if (code == 3 || code == 7) // SB_PAGEDOWN / SB_BOTTOM
                top = listbox_max_top(c, w);
            listbox_scroll_to(c, w, top);
        }
        set_eax(c, 0);
        return;
    }
    case 0x020a: { // WM_MOUSEWHEEL: one notch is three rows (the system default)
        if (w && w->cls == "listbox") {
            int32_t delta = int16_t((arg(c, 2) >> 16) & 0xffff);
            listbox_scroll_to(c, w, w->list_top + (delta > 0 ? -3 : 3));
        }
        set_eax(c, 0);
        return;
    }
    case 0x0180: // LB_ADDSTRING
        if (w) {
            w->list_items.push_back(gm_str(arg(c, 3)));
            w->update_pending = true;
            set_eax(c, uint32_t(w->list_items.size() - 1));
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    case 0x0181: { // LB_INSERTSTRING
        if (w) {
            int32_t i = int32_t(arg(c, 2));
            if (i < 0 || i > int32_t(w->list_items.size()))
                i = int32_t(w->list_items.size());
            w->list_items.insert(w->list_items.begin() + i, gm_str(arg(c, 3)));
            // Inserting shifts every later row's index; the per-row height cache
            // is keyed by index, so drop it and measure the moved rows again,
            // and shift the selection set the same way.
            w->list_item_heights.clear();
            std::set<int32_t> shifted;
            for (int32_t s : w->list_selected)
                shifted.insert(s >= i ? s + 1 : s);
            w->list_selected.swap(shifted);
            if (w->list_sel >= i)
                ++w->list_sel;
            w->update_pending = true;
            set_eax(c, uint32_t(i));
        } else {
            set_eax(c, 0xffffffffu);
        }
        return;
    }
    case 0x0184: // LB_RESETCONTENT
        if (w) {
            w->list_items.clear();
            w->list_sel = -1;
            w->list_top = 0;
            w->list_selected.clear();
            w->list_item_heights.clear();
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0185: { // LB_SETSEL: select or clear one row (or all with index -1)
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        const bool select = arg(c, 2) != 0;
        const int32_t idx = int32_t(arg(c, 3));
        const int32_t n = int32_t(w->list_items.size());
        if (idx == -1) {
            w->list_selected.clear();
            if (select)
                for (int32_t i = 0; i < n; ++i)
                    w->list_selected.insert(i);
        } else if (idx >= 0 && idx < n) {
            if (select) {
                w->list_selected.insert(idx);
                w->list_sel = idx;
            } else {
                w->list_selected.erase(idx);
            }
        }
        w->update_pending = true;
        set_eax(c, 0);
        return;
    }
    case 0x0186: { // LB_SETCURSEL
        int32_t i = int32_t(arg(c, 2));
        if (w) {
            w->list_sel = (i < 0 || i >= int32_t(w->list_items.size())) ? -1 : i;
            // Windows selects the caret item too, but only when the box is not
            // multiple/extended select; there the caret is independent of the
            // selection (and LB_SETSEL is what changes it).
            if (!listbox_multi_select(w)) {
                w->list_selected.clear();
                if (w->list_sel >= 0)
                    w->list_selected.insert(w->list_sel);
            }
            w->update_pending = true;
        }
        set_eax(c, w ? uint32_t(w->list_sel) : 0xffffffffu);
        return;
    }
    case 0x0187: { // LB_GETSEL: is this row selected?
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        const int32_t idx = int32_t(arg(c, 2));
        if (idx < 0 || idx >= int32_t(w->list_items.size())) {
            set_eax(c, 0xffffffffu);
            return;
        }
        set_eax(c, w->list_selected.count(idx) ? 1u : 0u);
        return;
    }
    case 0x0188: // LB_GETCURSEL
        set_eax(c, w ? uint32_t(w->list_sel) : 0xffffffffu);
        return;
    case 0x0189: { // LB_GETTEXT
        int32_t i = int32_t(arg(c, 2));
        std::string s = (w && i >= 0 && i < int32_t(w->list_items.size())) ? w->list_items[i] : "";
        set_eax(c, gm_put_str(arg(c, 3), s.c_str(), 0x10000));
        return;
    }
    case 0x018a: { // LB_GETTEXTLEN
        int32_t i = int32_t(arg(c, 2));
        set_eax(c, (w && i >= 0 && i < int32_t(w->list_items.size()))
                       ? uint32_t(w->list_items[i].size())
                       : 0xffffffffu);
        return;
    }
    case 0x018b: // LB_GETCOUNT
        set_eax(c, w ? uint32_t(w->list_items.size()) : 0);
        return;
    case 0x018e: // LB_GETTOPINDEX
        set_eax(c, w ? uint32_t(w->list_top) : 0);
        return;
    case 0x018f: { // LB_FINDSTRING: first item after the start with the prefix
        std::string want = gm_str(arg(c, 3));
        std::string lower_want = user32::lower(want);
        int32_t start = int32_t(arg(c, 2)) + 1;
        int32_t found = -1;
        if (w)
            for (int32_t i = start; i < int32_t(w->list_items.size()); ++i)
                if (user32::lower(w->list_items[i]).rfind(lower_want, 0) == 0) {
                    found = i;
                    break;
                }
        set_eax(c, found < 0 ? 0xffffffffu : uint32_t(found));
        return;
    }
    case 0x018c: { // LB_SELECTSTRING: find the prefix, then make it the caret
        // An empty prefix matches nothing, as on Windows; the search starts at
        // the item after `wParam`. (This is 0x018C, not 0x0190: 0x0190 is
        // LB_GETSELCOUNT. The two were swapped here, which made the Editor's
        // Contact Sound list select row 1 on every click - see the note.)
        std::string want = gm_str(arg(c, 3));
        if (want.empty()) {
            set_eax(c, 0xffffffffu);
            return;
        }
        std::string lower_want = user32::lower(want);
        int32_t start = int32_t(arg(c, 2)) + 1;
        int32_t found = -1;
        if (w)
            for (int32_t i = start; i < int32_t(w->list_items.size()); ++i)
                if (user32::lower(w->list_items[i]).rfind(lower_want, 0) == 0) {
                    found = i;
                    break;
                }
        if (w && found >= 0) {
            w->list_sel = found;
            w->list_selected.clear();
            w->list_selected.insert(found);
            w->update_pending = true;
        }
        set_eax(c, found < 0 ? 0xffffffffu : uint32_t(found));
        return;
    }
    case 0x0190: // LB_GETSELCOUNT: how many rows are selected
        set_eax(c, w ? uint32_t(w->list_selected.size()) : 0xffffffffu);
        return;
    case 0x0191: { // LB_GETSELITEMS: fill `wParam` ints with the selected rows
        int32_t max_items = int32_t(arg(c, 2));
        uint32_t buf = arg(c, 3);
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        int32_t written = 0;
        for (int32_t i = 0; i < int32_t(w->list_items.size()) && written < max_items; ++i)
            if (w->list_selected.count(i)) {
                if (gm_valid(buf + 4u * uint32_t(written), 4))
                    wr32(buf + 4u * uint32_t(written), uint32_t(i));
                ++written;
            }
        set_eax(c, uint32_t(written));
        return;
    }
    case 0x0197: // LB_SETTOPINDEX
        if (w) {
            int32_t t = int32_t(arg(c, 2));
            int32_t n = int32_t(w->list_items.size());
            w->list_top = t < 0 ? 0 : (t > n ? n : t);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    // The row rectangle is the same geometry the hit test and the paint use, so
    // a caller asking where row k is gets the band a click there would select.
    case 0x0198: { // LB_GETITEMRECT
        int32_t i = int32_t(arg(c, 2));
        uint32_t rp = arg(c, 3);
        if (!w || i < 0 || i >= int32_t(w->list_items.size()) || !gm_valid(rp, 16)) {
            set_eax(c, 0xffffffffu); // LB_ERR on a bad index
            return;
        }
        int32_t top = listbox_item_top(c, w, i);
        wr32(rp + 0, 0);
        wr32(rp + 4, uint32_t(top));
        wr32(rp + 8, uint32_t(w->w));
        wr32(rp + 12, uint32_t(top + listbox_item_height(c, w, i)));
        set_eax(c, 1);
        return;
    }
    case 0x01a0: { // LB_SETITEMHEIGHT: an explicit height overrides WM_MEASUREITEM
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        int32_t i = int32_t(arg(c, 2)), h = int32_t(arg(c, 3));
        if (w->style & 0x20) { // LBS_OWNERDRAWVARIABLE sets one row
            if (i < 0) {
                set_eax(c, 0xffffffffu);
                return;
            }
            if (int32_t(w->list_item_heights.size()) <= i)
                w->list_item_heights.resize(i + 1, 0);
            w->list_item_heights[i] = h;
        } else {
            w->list_item_height = h; // the box has a single height
        }
        w->update_pending = true;
        set_eax(c, 0);
        return;
    }
    case 0x01a1: { // LB_GETITEMHEIGHT
        int32_t ih = w ? listbox_item_height(c, w, int32_t(arg(c, 2))) : -1;
        LOGW("LB_GETITEMHEIGHT hwnd=%08x item=%d -> %d style=%08x", arg(c, 1), int32_t(arg(c, 2)), ih,
             w ? w->style : 0);
        set_eax(c, w ? uint32_t(ih) : 0xffffffffu);
        return;
    }
    case 0x01a9: { // LB_ITEMFROMPOINT: LOWORD row, HIWORD non-zero when outside
        int32_t x = int16_t(arg(c, 3) & 0xffff);
        int32_t y = int16_t((arg(c, 3) >> 16) & 0xffff);
        if (!w) {
            set_eax(c, 0x10000u); // no rows: 0xffff index, outside the client area
            return;
        }
        bool outside = x < 0 || x >= w->w || y < 0 || y >= w->h;
        int32_t idx = outside ? -1 : listbox_item_from_y(c, w, y);
        uint32_t out = idx < 0 ? 0xffffu : uint32_t(idx);
        set_eax(c, out | (outside ? 0x10000u : 0u));
        return;
    }
    // -----------------------------------------------------------------------
    // msctls_trackbar32 (TBM_*) and msctls_progress32 (PBM_*). The two message
    // sets share WM_USER+1..+16, so a code is told apart by the control's class
    // exactly as the two classes do on Windows. The range is inclusive; TBS_VERT
    // (2) makes a trackbar vertical with the minimum at the bottom.
    // -----------------------------------------------------------------------
    case 0x0400: // TBM_GETPOS
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    case 0x0401: // TBM_GETRANGEMIN / PBM_SETRANGE (lParam = MAKELONG(min,max))
        if (w && w->cls == "msctls_progress32") {
            w->range_min = int16_t(arg(c, 3) & 0xffff);
            w->range_max = int16_t((arg(c, 3) >> 16) & 0xffff);
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
            set_eax(c, 0);
            return;
        }
        set_eax(c, w ? uint32_t(w->range_min) : 0);
        return;
    case 0x0402: // TBM_GETRANGEMAX / PBM_SETPOS
        if (w && w->cls == "msctls_progress32") {
            w->range_pos = range_clamp(w, int32_t(arg(c, 2)));
            w->update_pending = true;
            set_eax(c, uint32_t(w->range_pos));
            return;
        }
        set_eax(c, w ? uint32_t(w->range_max) : 0);
        return;
    case 0x0403: // PBM_DELTAPOS
        if (w) {
            w->range_pos = range_clamp(w, w->range_pos + int32_t(arg(c, 2)));
            w->update_pending = true;
        }
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    case 0x0404: // PBM_SETSTEP
        if (w) {
            w->range_line = int32_t(arg(c, 2));
            if (w->range_line < 1)
                w->range_line = 1;
        }
        set_eax(c, w ? uint32_t(w->range_line) : 0);
        return;
    case 0x0405: // TBM_SETPOS / PBM_STEPIT
        if (w && w->cls == "msctls_progress32") {
            w->range_pos = range_clamp(w, w->range_pos + w->range_line);
            w->update_pending = true;
            set_eax(c, uint32_t(w->range_pos));
            return;
        }
        if (w) {
            w->range_pos = range_clamp(w, int32_t(arg(c, 3)));
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0406: // TBM_SETRANGE (lParam = MAKELONG(min,max)) / PBM_SETRANGE32
        if (w) {
            if (w->cls == "msctls_progress32") {
                w->range_min = int32_t(arg(c, 2));
                w->range_max = int32_t(arg(c, 3));
            } else {
                w->range_min = int16_t(arg(c, 3) & 0xffff);
                w->range_max = int16_t((arg(c, 3) >> 16) & 0xffff);
            }
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0407: // TBM_SETRANGEMIN / PBM_GETRANGE
        if (w && w->cls == "msctls_progress32") {
            uint32_t lo = arg(c, 2), hi = arg(c, 3);
            if (lo && gm_valid(lo, 4))
                wr32(lo, uint32_t(w->range_min));
            if (hi && gm_valid(hi, 4))
                wr32(hi, uint32_t(w->range_max));
            set_eax(c, 1);
            return;
        }
        if (w) {
            w->range_min = int32_t(arg(c, 3));
            if (w->range_max < w->range_min)
                w->range_max = w->range_min;
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0408: // TBM_SETRANGEMAX / PBM_GETPOS
        if (w && w->cls == "msctls_progress32") {
            set_eax(c, uint32_t(w->range_pos));
            return;
        }
        if (w) {
            w->range_max = int32_t(arg(c, 3));
            if (w->range_max < w->range_min)
                w->range_min = w->range_max;
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0410: // TBM_GETNUMTICS / PBM_SETSTATE
        if (w && w->cls == "msctls_progress32") {
            set_eax(c, 0);
            return;
        }
        {
            int32_t n = 0;
            if (w) {
                const int32_t range = w->range_max - w->range_min;
                n = range >= 0 ? range + 1 : 0;
            }
            set_eax(c, uint32_t(n));
        }
        return;
    case 0x0414: // TBM_SETTICFREQ: accepted; ticks are drawn at a computed step
        set_eax(c, 0);
        return;
    case 0x0415: // TBM_SETPAGESIZE
        if (w) {
            w->range_page = int32_t(arg(c, 3));
            if (w->range_page < 0)
                w->range_page = 0;
        }
        set_eax(c, 0);
        return;
    case 0x0416: // TBM_GETPAGESIZE
        set_eax(c, w ? uint32_t(trackbar_page(w)) : 0);
        return;
    case 0x0417: // TBM_SETLINESIZE
        if (w) {
            w->range_line = int32_t(arg(c, 3));
            if (w->range_line < 1)
                w->range_line = 1;
        }
        set_eax(c, 0);
        return;
    case 0x0418: // TBM_GETLINESIZE
        set_eax(c, w ? uint32_t(w->range_line) : 0);
        return;
    // -----------------------------------------------------------------------
    // COMBOBOX (CB_*). The items live in `list_items`, the selection in
    // `list_sel`; a drop-down list draws its closed field and, when open, the
    // list below it.
    // -----------------------------------------------------------------------
    case 0x0143: // CB_ADDSTRING
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        w->list_items.push_back(gm_str(arg(c, 3)));
        w->update_pending = true;
        set_eax(c, uint32_t(w->list_items.size() - 1));
        return;
    case 0x014a: { // CB_INSERTSTRING
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        int32_t i = int32_t(arg(c, 2));
        if (i < 0 || i > int32_t(w->list_items.size()))
            i = int32_t(w->list_items.size());
        w->list_items.insert(w->list_items.begin() + i, gm_str(arg(c, 3)));
        w->update_pending = true;
        set_eax(c, uint32_t(i));
        return;
    }
    case 0x0144: { // CB_DELETESTRING
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        int32_t i = int32_t(arg(c, 2));
        if (i < 0 || i >= int32_t(w->list_items.size())) {
            set_eax(c, 0xffffffffu);
            return;
        }
        w->list_items.erase(w->list_items.begin() + i);
        if (w->list_sel == i)
            w->list_sel = -1;
        else if (w->list_sel > i)
            --w->list_sel;
        w->update_pending = true;
        set_eax(c, uint32_t(w->list_items.size()));
        return;
    }
    case 0x014b: // CB_RESETCONTENT
        if (w) {
            w->list_items.clear();
            w->list_sel = -1;
            w->combo_hot = -1;
            w->title_utf8.clear();
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0146: // CB_GETCOUNT
        set_eax(c, w ? uint32_t(w->list_items.size()) : 0);
        return;
    case 0x0147: // CB_GETCURSEL
        set_eax(c, w ? uint32_t(w->list_sel) : 0xffffffffu);
        return;
    case 0x014e: { // CB_SETCURSEL
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        const int32_t i = int32_t(arg(c, 2));
        if (i < 0 || i >= int32_t(w->list_items.size())) {
            w->list_sel = -1; // Windows: -1 removes the selection
        } else {
            w->list_sel = i;
            // A drop-down list shows the selected item in its closed field.
            if ((w->style & 3u) == 3u /*CBS_DROPDOWNLIST*/)
                w->title_utf8 = w->list_items[i];
        }
        w->update_pending = true;
        set_eax(c, w->list_sel < 0 ? 0xffffffffu : uint32_t(w->list_sel));
        return;
    }
    case 0x0148: { // CB_GETLBTEXT
        const int32_t i = int32_t(arg(c, 2));
        std::string s = (w && i >= 0 && i < int32_t(w->list_items.size())) ? w->list_items[i] : "";
        set_eax(c, gm_put_str(arg(c, 3), s.c_str(), 0x10000));
        return;
    }
    case 0x0149: { // CB_GETLBTEXTLEN
        const int32_t i = int32_t(arg(c, 2));
        set_eax(c, (w && i >= 0 && i < int32_t(w->list_items.size()))
                       ? uint32_t(w->list_items[i].size())
                       : 0xffffffffu);
        return;
    }
    case 0x014c:   // CB_FINDSTRING (prefix)
    case 0x0158: { // CB_FINDSTRINGEXACT
        const std::string want = user32::lower(gm_str(arg(c, 3)));
        const int32_t start = int32_t(arg(c, 2)) + 1;
        int32_t found = -1;
        if (w)
            for (int32_t i = start; i < int32_t(w->list_items.size()); ++i) {
                const std::string li = user32::lower(w->list_items[i]);
                if (msg == 0x0158 ? li == want : li.rfind(want, 0) == 0) {
                    found = i;
                    break;
                }
            }
        set_eax(c, found < 0 ? 0xffffffffu : uint32_t(found));
        return;
    }
    case 0x014d: { // CB_SELECTSTRING
        const std::string want = user32::lower(gm_str(arg(c, 3)));
        const int32_t start = int32_t(arg(c, 2)) + 1;
        int32_t found = -1;
        if (w)
            for (int32_t i = start; i < int32_t(w->list_items.size()); ++i)
                if (user32::lower(w->list_items[i]).rfind(want, 0) == 0) {
                    found = i;
                    break;
                }
        if (w && found >= 0) {
            w->list_sel = found;
            if ((w->style & 3u) == 3u)
                w->title_utf8 = w->list_items[found];
            w->update_pending = true;
        }
        set_eax(c, found < 0 ? 0xffffffffu : uint32_t(found));
        return;
    }
    case 0x014f: // CB_SHOWDROPDOWN
        if (w)
            combo_show_dropdown(c, hwnd, arg(c, 2) != 0);
        set_eax(c, 0);
        return;
    case 0x0153: // CB_SETITEMHEIGHT
        if (w) {
            w->list_item_height = int32_t(arg(c, 3));
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0154: // CB_GETITEMHEIGHT
        set_eax(c, w ? uint32_t(combo_row_height(w)) : 0xffffffffu);
        return;
    case 0x0157: // CB_GETDROPPEDSTATE
        set_eax(c, (w && w->combo_open) ? 1u : 0u);
        return;
    case 0x015b: // CB_GETTOPINDEX
        set_eax(c, w ? uint32_t(w->list_top) : 0);
        return;
    // -----------------------------------------------------------------------
    // msctls_updown32 (UDM_*). The buddy edit is updated with the new value.
    // -----------------------------------------------------------------------
    case 0x0465: // UDM_SETRANGE (lParam = MAKELONG(max,min))
        if (w) {
            w->range_max = int16_t(arg(c, 3) & 0xffff);
            w->range_min = int16_t((arg(c, 3) >> 16) & 0xffff);
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
        }
        set_eax(c, 0);
        return;
    case 0x0466: // UDM_GETRANGE
        set_eax(c, w ? ((uint32_t(uint16_t(w->range_min)) << 16) | uint16_t(w->range_max)) : 0);
        return;
    case 0x0467: // UDM_SETPOS (lParam = signed position)
        if (w) {
            w->range_pos = range_clamp(w, int32_t(int16_t(arg(c, 3) & 0xffff)));
            w->update_pending = true;
        }
        set_eax(c, w ? uint32_t(int16_t(w->range_pos)) : 0);
        return;
    case 0x0468: // UDM_GETPOS (LOWORD the value, HIWORD non-zero on error)
        set_eax(c, w ? uint32_t(uint16_t(w->range_pos)) : 0);
        return;
    case 0x0469: // UDM_SETBUDDY
        if (w) {
            w->spin_buddy = arg(c, 2);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x046a: // UDM_GETBUDDY
        set_eax(c, w ? w->spin_buddy : 0);
        return;
    case 0x046d: // UDM_SETBASE
        if (w)
            w->spin_base = int32_t(arg(c, 2));
        set_eax(c, w ? uint32_t(w->spin_base) : 0);
        return;
    case 0x046e: // UDM_GETBASE
        set_eax(c, w ? uint32_t(w->spin_base) : 0);
        return;
    case 0x046f: // UDM_SETRANGE32 (wParam low, lParam high)
        if (w) {
            w->range_min = int32_t(arg(c, 2));
            w->range_max = int32_t(arg(c, 3));
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
        }
        set_eax(c, 0);
        return;
    case 0x0471: // UDM_SETPOS32
        if (w) {
            w->range_pos = range_clamp(w, int32_t(arg(c, 2)));
            w->update_pending = true;
        }
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    case 0x0472: // UDM_GETPOS32
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    // -----------------------------------------------------------------------
    // SysTabControl32 (TCM_*). TCITEM is 28 bytes; only TCIF_TEXT (1) is read.
    // -----------------------------------------------------------------------
    case 0x1304: // TCM_GETITEMCOUNT
        set_eax(c, w ? uint32_t(w->list_items.size()) : 0);
        return;
    case 0x1307: { // TCM_INSERTITEM (wParam index, lParam TCITEM*)
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        int32_t i = int32_t(arg(c, 2));
        if (i < 0 || i > int32_t(w->list_items.size()))
            i = int32_t(w->list_items.size());
        uint32_t item = arg(c, 3);
        std::string text;
        if (item && gm_valid(item, 28) && (rd32(item) & 1u)) // TCIF_TEXT
            text = gm_str(rd32(item + 12));
        w->list_items.insert(w->list_items.begin() + i, text);
        w->update_pending = true;
        set_eax(c, uint32_t(i));
        return;
    }
    case 0x1308: { // TCM_DELETEITEM
        if (!w) {
            set_eax(c, 0);
            return;
        }
        const int32_t i = int32_t(arg(c, 2));
        if (i >= 0 && i < int32_t(w->list_items.size())) {
            w->list_items.erase(w->list_items.begin() + i);
            if (w->list_sel == i)
                w->list_sel = -1;
            else if (w->list_sel > i)
                --w->list_sel;
            w->update_pending = true;
        }
        set_eax(c, 1);
        return;
    }
    case 0x1309: // TCM_DELETEALLITEMS
        if (w) {
            w->list_items.clear();
            w->list_sel = -1;
            w->update_pending = true;
        }
        set_eax(c, 1);
        return;
    case 0x130b: // TCM_GETCURSEL
        set_eax(c, w ? uint32_t(w->list_sel) : 0xffffffffu);
        return;
    case 0x130c: { // TCM_SETCURSEL
        if (!w) {
            set_eax(c, 0xffffffffu);
            return;
        }
        const int32_t i = int32_t(arg(c, 2));
        w->list_sel = (i < 0 || i >= int32_t(w->list_items.size())) ? -1 : i;
        w->update_pending = true;
        set_eax(c, w->list_sel < 0 ? 0xffffffffu : uint32_t(w->list_sel));
        return;
    }
    case 0x1305: { // TCM_GETITEM (wParam index, lParam TCITEM*)
        if (!w) {
            set_eax(c, 0);
            return;
        }
        const int32_t i = int32_t(arg(c, 2));
        uint32_t item = arg(c, 3);
        if (i < 0 || i >= int32_t(w->list_items.size()) || !item || !gm_valid(item, 28)) {
            set_eax(c, 0);
            return;
        }
        if (rd32(item) & 1u) { // TCIF_TEXT
            uint32_t buf = rd32(item + 12);
            uint32_t cap = rd32(item + 16);
            if (buf && cap)
                gm_put_str(buf, w->list_items[i].c_str(), cap);
        }
        set_eax(c, 1);
        return;
    }
    // -----------------------------------------------------------------------
    // The SCROLLBAR control (SBM_*). The range is inclusive; SBS_VERT (1) makes
    // it vertical.
    // -----------------------------------------------------------------------
    case 0x00e0: // SBM_SETPOS (wParam pos, lParam redraw)
        if (w) {
            w->range_pos = range_clamp(w, int32_t(arg(c, 2)));
            w->update_pending = true;
        }
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    case 0x00e1: // SBM_GETPOS
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    case 0x00e2: // SBM_SETRANGE (wParam min, lParam max)
        if (w) {
            w->range_min = int32_t(arg(c, 2));
            w->range_max = int32_t(arg(c, 3));
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x00e3: { // SBM_GETRANGE (wParam LPINT min, lParam LPINT max)
        if (w) {
            uint32_t lo = arg(c, 2), hi = arg(c, 3);
            if (lo && gm_valid(lo, 4))
                wr32(lo, uint32_t(w->range_min));
            if (hi && gm_valid(hi, 4))
                wr32(hi, uint32_t(w->range_max));
        }
        set_eax(c, w ? uint32_t(w->range_pos) : 0);
        return;
    }
    case 0x00e6: // SBM_SETRANGEREDRAW
        if (w) {
            w->range_min = int32_t(arg(c, 2));
            w->range_max = int32_t(arg(c, 3));
            if (w->range_max < w->range_min)
                std::swap(w->range_min, w->range_max);
            w->range_pos = range_clamp(w, w->range_pos);
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x00f1: // BM_SETCHECK
        if (w) {
            const uint32_t bs = w->style & 0x0fu;
            w->check_state = arg(c, 2);
            // Checking a radio button clears its group (TD-081).
            if ((bs == 4 || bs == 9) && w->check_state)
                radio_group_clear(hwnd);
        }
        set_eax(c, 0);
        return;
    case 0x00f5: { // BM_CLICK: press and release, then notify the parent
        uint32_t parent = w ? w->parent : 0, id = w ? w->id : 0;
        if (w && w->cls == "button") {
            if (is_check_button(w->style)) {
                const uint32_t bs = w->style & 0x0fu;
                if (bs == 3)
                    w->check_state = w->check_state ? 0u : 1u;
                else if (bs == 6)
                    w->check_state = (w->check_state + 1) % 3;
                else if (bs == 9 || bs == 4) {
                    w->check_state = 1u;
                    radio_group_clear(hwnd);
                }
                w->update_pending = true;
            } else if (!is_group_box(w->style)) {
                w->pressed = true;
                w->pressed = false;
            }
        }
        if (w && is_group_box(w->style)) {
            set_eax(c, 0);
            return;
        }
        if (parent)
            host_dispatch_to_wndproc(c, parent, 0x0111 /*WM_COMMAND*/, id & 0xffffu, hwnd);
        set_eax(c, 0);
        return;
    }
    case 0x00f0: // BM_GETCHECK
        set_eax(c, w ? w->check_state : 0);
        return;
    case 0x00f2: { // BM_GETSTATE
        // BST_PUSHED while the button is held, BST_FOCUS while it owns the
        // keyboard. Windows sets the focus bit when a button is clicked, and
        // Petz 5's profile gates its Pick A Pet and Family Tree handlers on a
        // non-zero BM_GETSTATE - they did nothing while this always answered 0.
        uint32_t state = 0;
        if (w && w->cls == "button" && w->pressed)
            state |= 4u; // BST_PUSHED
        if (focus_window() == hwnd)
            state |= 8u; // BST_FOCUS
        set_eax(c, state);
        return;
    }
    case 0x0200: { // WM_MOUSEMOVE: a thumb drag, a scrollbar drag, a combo hot row
        if (!w) {
            set_eax(c, 0);
            return;
        }
        const int32_t mx = int16_t(arg(c, 3) & 0xffff);
        const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
        if (w->cls == "msctls_trackbar32" && w->thumb_drag) {
            const int32_t np = range_clamp(w, range_pos_from_point(w, mx, my));
            if (np != w->range_pos) {
                w->range_pos = np;
                w->update_pending = true;
                range_notify(c, hwnd, 5 /*TB_THUMBTRACK*/, np);
            }
        } else if (w->cls == "scrollbar" && w->thumb_drag) {
            const bool vert = range_vertical(w);
            const int32_t arrow = 16, thumb = 16;
            const int32_t span = (vert ? w->h : w->w) - 2 * arrow;
            const int32_t track = span - thumb;
            const int32_t range = w->range_max - w->range_min;
            if (track > 0 && range > 0) {
                int32_t along = vert ? my - arrow - thumb / 2 : mx - arrow - thumb / 2;
                if (along < 0)
                    along = 0;
                if (along > track)
                    along = track;
                const int32_t np =
                    w->range_min + int32_t((int64_t(along) * range + track / 2) / track);
                if (np != w->range_pos) {
                    w->range_pos = np;
                    w->update_pending = true;
                    range_notify(c, hwnd, 5 /*SB_THUMBTRACK*/, np);
                }
            }
        } else if (w->cls == "listbox" && w->thumb_drag && listbox_has_scrollbar(w)) {
            // Drag the scrollbar thumb: map the pointer's position on the track
            // to the first-visible row.
            const int32_t max_top = listbox_max_top(c, w);
            const int32_t band = w->h - 2 * listbox_sb_width;
            int32_t frac = band > 0 ? (my - listbox_sb_width) * 1000 / band : 0;
            if (frac < 0)
                frac = 0;
            if (frac > 1000)
                frac = 1000;
            listbox_scroll_to(c, w, int32_t(int64_t(frac) * max_top / 1000));
        }
        // An open combo box's drop-down is its own popup window and takes the
        // pointer capture, so a move over the list never reaches the combo.
        set_eax(c, 0);
        return;
    }
    case 0x0201: // WM_LBUTTONDOWN: remember the press so the click is a pair
        LOGW("control: WM_LBUTTONDOWN hwnd=%08x id=%u", hwnd, w ? w->id : 0);
        if (w && w->cls == "button")
            set_focus_window(hwnd); // a clicked button keeps the focus (BST_FOCUS)
        if (w && w->cls == "edit") {
            // A click gives the edit the keyboard and places the caret where
            // the pointer is; Shift extends the existing selection instead.
            set_focus_window(hwnd);
            uint32_t hdc = gdi_window_dc(hwnd);
            if (auto *dc = gdi::dc_of(hdc))
                if (w->font)
                    dc->font = w->font; // measure in the control's own face
            const int32_t x = int16_t(arg(c, 3) & 0xffff);
            edit_move_caret(w, edit_index_from_x(hdc, w, x), (g_key_state[0x10] & 0x80) != 0);
            gdi_release_window_dc(hwnd, hdc);
            w->update_pending = true;
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "listbox") {
            // Windows gives the keyboard to a listbox that is clicked, so the
            // arrow keys that follow reach it. The dialog manager otherwise
            // leaves focus on the first tab stop (its OK button).
            set_focus_window(hwnd);
            const int32_t mx = int16_t(arg(c, 3) & 0xffff);
            const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
            // A WS_VSCROLL box owns its right 16 px: the arrows line, the track
            // pages and the thumb starts a drag, none of which selects a row.
            if (listbox_has_scrollbar(w) && mx >= w->w - listbox_sb_width) {
                switch (listbox_sb_zone(c, w, my)) {
                case 1:
                    listbox_scroll_to(c, w, w->list_top - 1);
                    break;
                case 5:
                    listbox_scroll_to(c, w, w->list_top + 1);
                    break;
                case 3:
                    w->thumb_drag = true;
                    set_capture_window(hwnd); // keep the drag even off the bar
                    break;
                case 2:
                    listbox_scroll_to(c, w, w->list_top - 3);
                    break;
                default:
                    listbox_scroll_to(c, w, w->list_top + 3);
                    break;
                }
                set_eax(c, 0);
                return;
            }
            // The click row comes from the same heights the paint and
            // LB_GETITEMRECT use; a hard 16 px divisor here selected the wrong
            // row once WM_MEASUREITEM gave the box a different height.
            int32_t row = listbox_item_from_y(c, w, my);
            if (row >= 0 && row != w->list_sel) {
                w->list_sel = row;
                // A plain click replaces the selection, even in an extended-
                // select box (the modifiers are what extend it, and the game's
                // own subclass drives its multi-selection with LB_SETSEL).
                w->list_selected.clear();
                w->list_selected.insert(row);
                w->update_pending = true;
                if (w->parent)
                    host_dispatch_to_wndproc(c, w->parent, 0x0111 /* WM_COMMAND */, w->id & 0xffffu,
                                             hwnd); // LBN_SELCHANGE
            }
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "combobox") {
            set_focus_window(hwnd);
            // A press on the closed field opens a drop-down list; a press while
            // it is open (only reachable without the popup's capture) closes it.
            if (w->combo_open)
                combo_show_dropdown(c, hwnd, false);
            else if ((w->style & 3u) == 3u /*CBS_DROPDOWNLIST*/)
                combo_show_dropdown(c, hwnd, true);
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "msctls_trackbar32") {
            set_focus_window(hwnd);
            w->thumb_drag = true;
            const int32_t mx = int16_t(arg(c, 3) & 0xffff);
            const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
            const int32_t np = range_clamp(w, range_pos_from_point(w, mx, my));
            w->range_pos = np;
            w->update_pending = true;
            range_notify(c, hwnd, 5 /*TB_THUMBTRACK*/, np);
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "scrollbar") {
            set_focus_window(hwnd);
            const bool vert = range_vertical(w);
            const int32_t mx = int16_t(arg(c, 3) & 0xffff);
            const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
            const int32_t arrow = 16, thumb = 16;
            const int32_t axis = vert ? my : mx;
            const int32_t span = (vert ? w->h : w->w) - 2 * arrow;
            const int32_t track = span - thumb;
            const int32_t range = w->range_max - w->range_min;
            int32_t tpos = 0;
            if (track > 0 && range > 0) {
                int32_t t =
                    int32_t((int64_t(w->range_pos - w->range_min) * track + range / 2) / range);
                if (t < 0)
                    t = 0;
                if (t > track)
                    t = track;
                tpos = t;
            }
            const int32_t thumb0 = arrow + tpos;
            const int32_t far = vert ? w->h : w->w;
            if (axis < arrow) {
                w->range_pos = range_clamp(w, w->range_pos - w->range_line);
                w->update_pending = true;
                range_notify(c, hwnd, 0 /*SB_LINEUP*/, w->range_pos);
            } else if (axis >= far - arrow) {
                w->range_pos = range_clamp(w, w->range_pos + w->range_line);
                w->update_pending = true;
                range_notify(c, hwnd, 1 /*SB_LINEDOWN*/, w->range_pos);
            } else if (axis >= thumb0 && axis < thumb0 + thumb) {
                w->thumb_drag = true; // grab the thumb
            } else if (axis < thumb0) {
                w->range_pos = range_clamp(w, w->range_pos - trackbar_page(w));
                w->update_pending = true;
                range_notify(c, hwnd, 2 /*SB_PAGEUP*/, w->range_pos);
            } else {
                w->range_pos = range_clamp(w, w->range_pos + trackbar_page(w));
                w->update_pending = true;
                range_notify(c, hwnd, 3 /*SB_PAGEDOWN*/, w->range_pos);
            }
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "msctls_updown32") {
            const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
            const int32_t delta = my < w->h / 2 ? 1 : -1;
            w->range_pos = range_clamp(w, w->range_pos + delta);
            w->update_pending = true;
            // The up-down pushes the value into its buddy edit, if it has one.
            if (w->spin_buddy && find_window(w->spin_buddy)) {
                char buf[32];
                snprintf(buf, sizeof buf, "%d", w->range_pos);
                uint32_t tmp = heap_alloc(32, true);
                if (tmp) {
                    gm_put_str(tmp, buf, 32);
                    deliver_message(c, w->spin_buddy, 0x000c /*WM_SETTEXT*/, 0, tmp, false);
                    heap_free(tmp);
                }
            }
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "systabcontrol32") {
            const int32_t mx = int16_t(arg(c, 3) & 0xffff);
            const int32_t my = int16_t((arg(c, 3) >> 16) & 0xffff);
            const int32_t tab = tab_index_from_point(w, mx, my);
            if (tab >= 0 && tab != w->list_sel) {
                const uint32_t parent = w->parent, id = w->id;
                w->list_sel = tab;
                w->update_pending = true;
                // TCN_SELCHANGE (TCN_FIRST-1) through an NMHDR via WM_NOTIFY.
                if (parent) {
                    uint32_t nm = heap_alloc(24, true);
                    if (nm) {
                        wr32(nm + 0, hwnd);        // hwndFrom
                        wr32(nm + 4, id);          // idFrom
                        wr32(nm + 8, 0xfffffdd9u); // TCN_SELCHANGE
                        host_dispatch_to_wndproc(c, parent, 0x004e /*WM_NOTIFY*/, id, nm);
                        heap_free(nm);
                    }
                }
            }
            set_eax(c, 0);
            return;
        }
        if (w) {
            // A checkbox/radio keeps its check state in `check_state`; a press
            // does not disturb it (only the push buttons use the pressed bit).
            // Never GWL_USERDATA: a program's own storage must survive a click.
            if (w->cls != "button" || (!is_check_button(w->style) && !is_group_box(w->style)))
                w->pressed = true;
            w->update_pending = true; // the pressed bevel has to be drawn
        }
        set_eax(c, 0);
        return;
    case 0x0202: { // WM_LBUTTONUP: a completed click reports WM_COMMAND
        uint32_t parent = w ? w->parent : 0, id = w ? w->id : 0;
        LOGW("control: WM_LBUTTONUP hwnd=%08x id=%u parent=%08x", hwnd, id, parent);
        if (w && w->cls == "listbox") {
            w->thumb_drag = false; // end a scrollbar-thumb drag
            if (capture_window() == hwnd)
                set_capture_window(0);
        }
        // The range controls and the combo box end their own interaction and
        // never report BN_CLICKED.
        if (w && (w->cls == "msctls_trackbar32" || w->cls == "scrollbar")) {
            if (w->thumb_drag) {
                w->thumb_drag = false;
                if (w->cls == "msctls_trackbar32") {
                    range_notify(c, hwnd, 4 /*TB_THUMBPOSITION*/, w->range_pos);
                    range_notify(c, hwnd, 8 /*TB_ENDTRACK*/, w->range_pos);
                } else {
                    range_notify(c, hwnd, 4 /*SB_THUMBPOSITION*/, w->range_pos);
                    range_notify(c, hwnd, 8 /*SB_ENDSCROLL*/, w->range_pos);
                }
            }
            w->update_pending = true;
            set_eax(c, 0);
            return;
        }
        if (w && (w->cls == "combobox" || w->cls == "msctls_updown32" ||
                  w->cls == "systabcontrol32" || w->cls == "msctls_progress32")) {
            set_eax(c, 0);
            return;
        }
        if (w) {
            if (w->cls == "button" && is_check_button(w->style)) {
                // An auto-checkbox/auto-3-state toggles itself before notifying
                // (BS_AUTO*), like the button class; the others only repaint.
                const uint32_t bs = w->style & 0x0fu;
                if (bs == 3) // BS_AUTOCHECKBOX
                    w->check_state = w->check_state ? 0u : 1u;
                else if (bs == 6) // BS_AUTO3STATE
                    w->check_state = (w->check_state + 1) % 3;
                else if (bs == 9) { // BS_AUTORADIOBUTTON
                    w->check_state = 1u;
                    radio_group_clear(hwnd); // TD-081: the group is exclusive
                }
            } else {
                w->pressed = false;
            }
            if (w->cls == "button")
                w->update_pending = true; // the bevel returns to raised
        }
        // A group box is not a button: no BN_CLICKED to the parent.
        if (w && is_group_box(w->style)) {
            set_eax(c, 0);
            return;
        }
        if (parent)
            host_dispatch_to_wndproc(c, parent, 0x0111 /* WM_COMMAND */,
                                     id & 0xffffu /* BN_CLICKED is 0 */, hwnd);
        set_eax(c, 0);
        return;
    }
    default:
        def_window_proc(c, w && w->unicode);
    }
}
uint32_t control_proc_addr() {
    static uint32_t addr = 0;
    if (!addr)
        addr = imports_alloc_trampoline("USER32.dll", "DIALOG control procedure", control_proc, 4);
    return addr;
}
// Register the dialog class and the controls a template names. The kit only
// ships STATIC; the rest are inert windows that still route messages.
void ensure_dialog_classes() {
    ensure_system_classes();
    for (const char *name :
         {"button", "edit", "combobox", "listbox", "scrollbar", "msctls_trackbar32",
          "msctls_progress32", "msctls_updown32", "systabcontrol32"}) {
        if (classes().count(name))
            continue;
        WndClass wc;
        wc.style = 0x4008; // CS_PARENTDC | CS_DBLCLKS
        wc.wndproc = control_proc_addr();
        wc.name = name;
        wc.atom = 0xc000 + uint32_t(classes().size());
        classes()[name] = wc;
        classes()["#atom" + std::to_string(wc.atom)] = wc;
    }
    if (!classes().count("#32770")) {
        WndClass wc;
        wc.style = 0x0008; // CS_DBLCLKS
        wc.name = "Dialog";
        wc.atom = 0xc000 + uint32_t(classes().size());
        classes()["#32770"] = wc;
        classes()["#atom" + std::to_string(wc.atom)] = wc;
    }
}

// ---------------------------------------------------------------------------
// DLGTEMPLATE decoding. Both the classic and the extended (DLGTEMPLATEEX)
// layouts are read; the menu field is ignored because an owned dialog menu is
// not modelled. Every length-prefixed string is bounded by the resource size.
// ---------------------------------------------------------------------------
struct DlgItemTemplate {
    uint32_t style = 0, exstyle = 0, id = 0;
    int16_t x = 0, y = 0, cx = 0, cy = 0;
    std::string cls, text;
    bool cls_ordinal = false;
    uint32_t cls_id = 0;
};
struct DlgTemplate {
    uint32_t style = 0, exstyle = 0;
    int16_t x = 0, y = 0, cx = 0, cy = 0;
    std::string menu, cls, title, font;
    uint16_t pointsize = 0, weight = 0;
    uint8_t italic = 0, charset = 0;
    std::vector<DlgItemTemplate> items;
};
// A template string: 0x0000 none, 0xffff ordinal, otherwise UTF-16 NUL-ended.
uint32_t tmpl_string(uint32_t p, uint32_t end, std::string *out, bool *ordinal, uint32_t *ord) {
    if (p + 2 > end)
        return 0;
    uint16_t first = rd16(p);
    if (first == 0)
        return p + 2;
    if (first == 0xffff) {
        if (p + 4 > end)
            return 0;
        if (ordinal)
            *ordinal = true;
        if (ord)
            *ord = rd16(p + 2);
        return p + 4;
    }
    if (out)
        *out = gm_wstr(p, (end - p) / 2);
    uint32_t q = p;
    while (q + 2 <= end && rd16(q))
        q += 2;
    return q + 2 <= end ? q + 2 : end;
}
bool parse_dlg_item(uint32_t p, uint32_t end, bool extended, DlgItemTemplate *it, uint32_t *next) {
    if (p + (extended ? 24u : 18u) > end)
        return false;
    if (extended) {
        it->exstyle = rd32(p + 4);
        it->style = rd32(p + 8);
        it->x = int16_t(rd16(p + 12));
        it->y = int16_t(rd16(p + 14));
        it->cx = int16_t(rd16(p + 16));
        it->cy = int16_t(rd16(p + 18));
        it->id = rd32(p + 20);
        p += 24;
    } else {
        it->style = rd32(p);
        it->exstyle = rd32(p + 4);
        it->x = int16_t(rd16(p + 8));
        it->y = int16_t(rd16(p + 10));
        it->cx = int16_t(rd16(p + 12));
        it->cy = int16_t(rd16(p + 14));
        it->id = rd16(p + 16);
        p += 18;
    }
    bool ordinal = false;
    uint32_t ord = 0;
    p = tmpl_string(p, end, &it->cls, &ordinal, &ord);
    it->cls_ordinal = ordinal;
    it->cls_id = ord;
    if (!p)
        return false;
    p = tmpl_string(p, end, &it->text, nullptr, nullptr);
    if (!p || p + 2 > end)
        return false;
    uint32_t extra = rd16(p);
    p += 2;
    if (extra) {
        if (p + extra > end)
            return false;
        p += extra;
    }
    // DLGITEMTEMPLATE structures are DWORD-aligned in the resource. Without
    // this, every item after one whose content length is not a multiple of
    // four is read two bytes early: its id, class and geometry all shift, so a
    // template's buttons get the previous item's id and the wrong rectangle
    // (the "Welcome to Catz 5" dialog parsed `Previous`/`Help` as id 14).
    p = (p + 3) & ~3u;
    *next = p;
    return true;
}
bool parse_dlg_template(uint32_t p, uint32_t size, DlgTemplate *t) {
    uint32_t end = p + size;
    bool extended = size >= 4 && rd16(p) == 1 && rd16(p + 2) == 0xffff;
    uint32_t q;
    uint16_t count = 0;
    if (extended) {
        if (size < 26)
            return false;
        t->exstyle = rd32(p + 8);
        t->style = rd32(p + 12);
        count = rd16(p + 16);
        t->x = int16_t(rd16(p + 18));
        t->y = int16_t(rd16(p + 20));
        t->cx = int16_t(rd16(p + 22));
        t->cy = int16_t(rd16(p + 24));
        q = tmpl_string(p + 26, end, &t->menu, nullptr, nullptr);
    } else {
        if (size < 18)
            return false;
        t->style = rd32(p);
        t->exstyle = rd32(p + 4);
        count = rd16(p + 8);
        t->x = int16_t(rd16(p + 10));
        t->y = int16_t(rd16(p + 12));
        t->cx = int16_t(rd16(p + 14));
        t->cy = int16_t(rd16(p + 16));
        q = tmpl_string(p + 18, end, &t->menu, nullptr, nullptr);
    }
    if (!q)
        return false;
    q = tmpl_string(q, end, &t->cls, nullptr, nullptr);
    if (!q)
        return false;
    q = tmpl_string(q, end, &t->title, nullptr, nullptr);
    if (!q)
        return false;
    if (t->style & 0x40) { // DS_SETFONT / DS_SHELLFONT
        // The classic DLGTEMPLATE carries only the point size then the typeface
        // name; the extended form puts weight, italic and charset in between.
        // Reading the extended layout from a classic template eats the first
        // four bytes of the typeface ("MS Sans Serif" -> "Sans Serif").
        uint32_t skip = extended ? 6u : 2u;
        if (q + skip > end)
            return false;
        t->pointsize = rd16(q);
        if (extended) {
            t->weight = rd16(q + 2);
            t->italic = rd8(q + 4);
            t->charset = rd8(q + 5);
        }
        q = tmpl_string(q + skip, end, &t->font, nullptr, nullptr);
        if (!q)
            return false;
    }
    // The first DLGITEMTEMPLATE is DWORD-aligned in the resource, exactly like
    // every following one (which parse_dlg_item aligns). Without this, a font
    // typeface whose UTF-16 length is not a multiple of four (e.g. "MS Sans
    // Serif", 13 chars + NUL = 28 bytes) leaves the item cursor two bytes early,
    // so the first few controls get the previous field's data as their geometry
    // and class — Petz 5's adoption-pledge dialog (template 164) lost its Adopt
    // button that way.
    q = (q + 3) & ~3u;
    for (uint16_t i = 0; i < count; ++i) {
        DlgItemTemplate item;
        if (!parse_dlg_item(q, end, extended, &item, &q))
            return false;
        t->items.push_back(item);
    }
    return true;
}
std::string dlg_control_class(const DlgItemTemplate &it) {
    if (it.cls_ordinal) {
        switch (it.cls_id) {
        case 0x80:
            return "button";
        case 0x81:
            return "edit";
        case 0x82:
            return "static";
        case 0x83:
            return "listbox";
        case 0x84:
            return "scrollbar";
        case 0x85:
            return "combobox";
        default:
            break;
        }
    }
    return it.cls.empty() ? "static" : user32::lower(it.cls);
}
uint32_t dlg_create_child(uint32_t parent, const DlgItemTemplate &it, const DlgBaseUnit &base,
                          uint32_t font) {
    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Window w;
    w.hwnd = hwnd;
    // The control's class owns its window procedure, exactly as on Windows:
    // STATIC paints its caption, the others answer the button/text messages.
    // The template names the class, so the engine looks it up rather than
    // forcing one procedure on every control.
    w.cls = dlg_control_class(it);
    auto cls = classes().find(w.cls);
    w.wndproc =
        cls != classes().end() && cls->second.wndproc ? cls->second.wndproc : control_proc_addr();
    w.style = it.style;
    w.exstyle = it.exstyle;
    // A template stores every control rectangle in dialog units; it is pixels
    // only after the dialog's font base units scale it (Windows MulDiv).
    w.x = dialog_map_unit(it.x, base.x, 4);
    w.y = dialog_map_unit(it.y, base.y, 8);
    w.w = dialog_map_unit(it.cx, base.x, 4);
    w.h = dialog_map_unit(it.cy, base.y, 8);
    // A drop-down combo box's template height is the DROPPED height: Windows
    // creates the control one row tall (the closed field) and the list extends
    // to the template height. Without this the closed field is as tall as the
    // whole drop-down and overlaps the controls below it.
    if (w.cls == "combobox" && (it.style & 3u) != 1u /*not CBS_SIMPLE*/ && w.h > 20) {
        w.combo_drop_h = w.h;
        w.h = 20;
    }
    w.title_utf8 = it.text;
    w.parent = parent;
    w.id = it.id;
    w.font = font; // the dialog's DS_SETFONT face, as WM_SETFONT would set it
    w.thread = guest_current_thread_id();
    w.visible = true;
    w.shown = true;
    w.update_pending = true; // a new control has to be painted
    windows()[hwnd] = w;
    window_created(hwnd);
    return hwnd;
}
// Build a dialog window and its controls from a decoded template. Shared by the
// modal (DialogBoxParamA) and modeless (CreateDialogParamA) entry points. A
// template without WS_VISIBLE is created hidden, exactly as Windows does, so
// the caller's ShowWindow is what makes it appear and invalidates it.
uint32_t create_dialog_window(X86 *c, uint32_t hinst, uint32_t parent, uint32_t proc,
                              const DlgTemplate &t) {
    ensure_dialog_classes();
    uint32_t hwnd = g_next_hwnd;
    g_next_hwnd += 4;
    Window w;
    w.hwnd = hwnd;
    w.wndproc = proc;
    w.style = t.style;
    w.exstyle = t.exstyle;
    // Everything in the template is in dialog units, sized by the DS_SETFONT
    // face; the pixel size is what the window and its controls are placed with,
    // and the same face is what they draw their text with.
    DlgBaseUnit base;
    uint32_t font = 0;
    if ((t.style & 0x40 /*DS_SETFONT / DS_SHELLFONT*/) && t.pointsize > 0) {
        base = dialog_base_units(t.font, t.pointsize, t.weight);
        font = dialog_font(t.font, t.pointsize, t.weight);
    }
    w.dlg_base_x = base.x;
    w.dlg_base_y = base.y;
    w.font = font;
    w.w = dialog_map_unit(t.cx, base.x, 4);
    w.h = dialog_map_unit(t.cy, base.y, 8);
    w.cls = "#32770";
    // The template size above is the CLIENT size; the frame is fitted around
    // it (Windows AdjustWindowRect). DS_CENTER centres the whole window, so it
    // needs the outer size and x/y is the outer origin.
    apply_dialog_nc(w);
    const int32_t outer_w = w.w + w.nc_left + w.nc_right;
    const int32_t outer_h = w.h + w.nc_top + w.nc_bottom;
    if (t.style & 0x800 /*DS_CENTER*/) {
        // Windows centres a DS_CENTER dialog in the working area of the monitor
        // that holds the owner. The port reports a 1024x768 desktop
        // (GetSystemMetrics, which the game's resolution check requires) but
        // presents only the game's window, so it centres on the owner instead,
        // keeping the dialog on the presented frame (TD-061).
        int32_t ow = 0, oh = 0;
        if (const Window *owner = find_window(parent)) {
            ow = owner->w;
            oh = owner->h;
        } else {
            uint32_t dw = 1024, dh = 768, bpp = 32;
            win32_display_mode(&dw, &dh, &bpp);
            ow = int32_t(dw);
            oh = int32_t(dh);
        }
        w.x = (ow - outer_w) / 2;
        w.y = (oh - outer_h) / 2;
        // A dialog larger than its owner would centre off the top-left; keep it
        // on screen rather than lose it (the port cannot drag a window back).
        if (w.x < 0)
            w.x = 0;
        if (w.y < 0)
            w.y = 0;
    } else {
        w.x = dialog_map_unit(t.x, base.x, 4);
        w.y = dialog_map_unit(t.y, base.y, 8);
    }
    w.title_utf8 = t.title;
    w.parent = parent;
    w.thread = guest_current_thread_id();
    w.hinstance = hinst;
    w.visible = (t.style & WS_VISIBLE) != 0;
    w.shown = w.visible;
    w.update_pending = w.visible;
    windows()[hwnd] = w;
    window_created(hwnd);
    for (const DlgItemTemplate &item : t.items)
        dlg_create_child(hwnd, item, base, font);
    // The dialog manager selects the template font into every control before
    // WM_INITDIALOG. The children are the runtime's own control procedures, so
    // dispatching it is safe and makes WM_GETFONT answer with the template face
    // (the dialog's own font lives in its Window record; its procedure is the
    // guest's, and Windows procs are not driven with a message they never asked
    // for before WM_INITDIALOG).
    if (font)
        for (const auto &kv : windows())
            if (kv.second.parent == hwnd)
                host_dispatch_to_wndproc(c, kv.first, 0x0030 /* WM_SETFONT */, font, 1);
    // The dialog manager gives the keyboard to the first tab stop, which is
    // where the host's keystrokes go (an edit control in a naming dialog). The
    // whole-text selection an edit gets on focus happens after WM_INITDIALOG
    // (initial_dialog_focus), once the procedure has filled the field.
    for (const auto &kv : windows())
        if (kv.second.parent == hwnd && kv.second.visible &&
            (kv.second.style & 0x10000 /*WS_TABSTOP*/)) {
            set_focus_window(kv.first);
            break;
        }
    return hwnd;
}
namespace user32 {
// Give an edit the focus the way the dialog manager does: its whole text is
// selected, so the next keystroke replaces it. A click uses edit_move_caret
// instead, which is why this is separate.
void focus_edit_select_all(uint32_t hwnd) {
    Window *w = find_window(hwnd);
    if (!w || w->cls != "edit")
        return;
    edit_clamp(w);
    w->sel_anchor = 0;
    w->caret = int32_t(w->title_utf8.size());
    w->update_pending = true;
}
// The dialog manager's focus after WM_INITDIALOG: a control the procedure
// focused keeps it (and an edit gets its whole text selected), otherwise the
// first WS_TABSTOP control does. Done after WM_INITDIALOG because the procedure
// fills the fields there, so the selection covers the text it wrote.
void initial_dialog_focus(uint32_t dlg) {
    uint32_t target = 0;
    Window *f = find_window(focus_window());
    if (f && f->parent == dlg) {
        target = f->hwnd;
    } else {
        for (const auto &kv : windows())
            if (kv.second.parent == dlg && kv.second.visible && kv.second.enabled &&
                (kv.second.style & 0x10000 /*WS_TABSTOP*/)) {
                target = kv.first;
                break;
            }
    }
    if (target) {
        set_focus_window(target);
        focus_edit_select_all(target);
    }
}
// The next (or previous) enabled, visible WS_TABSTOP child of a dialog, in
// creation order, wrapping at the ends - Windows' tab order.
uint32_t next_tab_stop(uint32_t dlg, uint32_t from, bool backward) {
    std::vector<uint32_t> stops;
    for (const auto &kv : windows())
        if (kv.second.parent == dlg && kv.second.visible && kv.second.enabled &&
            (kv.second.style & 0x10000 /*WS_TABSTOP*/))
            stops.push_back(kv.first);
    if (stops.empty())
        return 0;
    size_t idx = stops.size();
    for (size_t i = 0; i < stops.size(); ++i)
        if (stops[i] == from) {
            idx = i;
            break;
        }
    if (backward)
        idx = (idx == stops.size() || idx == 0) ? stops.size() - 1 : idx - 1;
    else
        idx = (idx == stops.size()) ? 0 : (idx + 1) % stops.size();
    return stops[idx];
}
// The dialog engine's IsDialogMessage. Windows runs it before DispatchMessage,
// so the modal pump has to: a dialog's OK button is pressed by Return, Escape
// cancels, and Tab moves the focus. Only WM_KEYDOWN is consumed; the WM_CHAR
// the host may also post for Return/Escape/Tab is a control character an edit
// ignores, so it cannot double-act.
bool dialog_key_message(X86 *c, uint32_t dlg, uint32_t msg, uint32_t wparam, uint32_t lparam) {
    (void)lparam;
    if (msg != 0x0100 /*WM_KEYDOWN*/)
        return false;
    Window *d = find_window(dlg);
    if (!d || d->cls != "#32770")
        return false;
    // Windows' IsDialogMessage only handles a message addressed to the dialog
    // or one of its controls.
    Window *f = find_window(focus_window());
    if (f && f->hwnd != dlg && f->parent != dlg)
        return false;
    // A multiline edit owns Return, Escape and Tab for itself.
    if (f && f->parent == dlg && f->cls == "edit" && (f->style & 0x0004u /*ES_MULTILINE*/))
        return false;
    const uint32_t vk = wparam;
    if (vk == 0x0d /*VK_RETURN*/) {
        uint32_t def = 0;
        for (const auto &kv : windows()) {
            if (kv.second.parent != dlg || kv.second.cls != "button" || !kv.second.visible ||
                !kv.second.enabled)
                continue;
            if ((kv.second.style & 0x0fu) == 1u /*BS_DEFPUSHBUTTON*/) {
                def = kv.first;
                break;
            }
        }
        // The default button is the template's BS_DEFPUSHBUTTON; a dialog with
        // none still answers Return with IDOK, as Windows does.
        const uint32_t id = def ? find_window(def)->id : 1u;
        host_dispatch_to_wndproc(c, dlg, 0x0111 /*WM_COMMAND*/, id & 0xffffu, def);
        return true;
    }
    if (vk == 0x1b /*VK_ESCAPE*/) {
        host_dispatch_to_wndproc(c, dlg, 0x0111 /*WM_COMMAND*/, 2 /*IDCANCEL*/, 0);
        return true;
    }
    if (vk == 0x09 /*VK_TAB*/) {
        const bool backward = (g_key_state[0x10] & 0x80) != 0; // Shift+Tab
        if (uint32_t next = next_tab_stop(dlg, focus_window(), backward)) {
            set_focus_window(next);
            focus_edit_select_all(next);
        }
        return true;
    }
    return false;
}
} // namespace user32
// Decode a dialog template that is already in guest memory and build the window
// and its controls. DialogBoxParamA/CreateDialogParamA reach the same code
// through a resource; this entry point lets a test drive the DS_SETFONT font
// application without a resource. Returns the dialog's handle, or 0.
namespace user32 {
uint32_t create_dialog_from_template(X86 *c, uint32_t hinst, uint32_t parent, uint32_t proc,
                                     uint32_t tmpl, uint32_t size) {
    DlgTemplate t;
    if (!tmpl || !size || !parse_dlg_template(tmpl, size, &t))
        return 0;
    return create_dialog_window(c, hinst, parent, proc, t);
}
} // namespace user32
// DialogBoxParamA reads the template from hInstance, builds the window and its
// controls, sends WM_INITDIALOG and pumps until EndDialog. Returns -1 when the
// template cannot be read or nobody can answer the dialog.
void u_DialogBoxParamA(X86 *c) {
    uint32_t hinst = arg(c, 0), name = arg(c, 1), parent = arg(c, 2), proc = arg(c, 3),
             init = arg(c, 4);
    if (!proc) {
        set_eax(c, 0xffffffffu);
        return;
    }
    std::string why;
    uint32_t entry = resource_find_in(hinst, 5 /* RT_DIALOG */, name, true, &why);
    uint32_t size = 0, p = entry ? resource_data(entry, &size, &why) : 0;
    LOGV("DialogBoxParamA: hinst=%08x name=%u entry=%08x size=%u", hinst, name, entry, size);
    DlgTemplate t;
    if (!p || !parse_dlg_template(p, size, &t)) {
        log_once("DialogBoxParamA-template",
                 "DialogBoxParamA: dialog template %u could not be read (%s)", name, why.c_str());
        set_eax(c, 0xffffffffu);
        return;
    }
    uint32_t hwnd = create_dialog_window(c, hinst, parent, proc, t);
    if (const Window *cw = find_window(hwnd)) {
        const Window *ow = find_window(parent);
        LOGV("DialogBoxParamA: name=%u parent=%08x owner=%dx%d rect=(%d,%d) %dx%d", name, parent,
             ow ? ow->w : -1, ow ? ow->h : -1, cw->x, cw->y, cw->w, cw->h);
    }
    // A modal dialog is shown whether or not the template asks for it.
    if (Window *w = find_window(hwnd)) {
        w->visible = true;
        w->shown = true;
        w->update_pending = true;
    }
    dialog_running[hwnd] = true;
    // WM_INITDIALOG carries the dialog's creation parameter in lParam (wParam
    // is the control to focus, which create_dialog_window already picked); the
    // engine used to pass it in wParam, so a DLGPROC that reads lParam — the
    // More Scenes list (FUN_00410410, selector 0x2cea) and the adoption pledge
    // (template 164, the pet bitmap) — saw zero.
    host_dispatch_to_wndproc(c, hwnd, 0x0110 /* WM_INITDIALOG */, 0, init);
    user32::initial_dialog_focus(hwnd);
    while (dialog_running.count(hwnd) && dialog_running[hwnd]) {
        host_pump_timers(c);
        pump_window_timers();
        pump_mouse_input(c);
        gdi_present_windows();
        bool dispatched = false;
        // Take one message out of the queue *before* dispatching it. The queue
        // is process-wide, and dispatching a message runs guest code: Petz 5's
        // profile DLGPROC calls DialogBoxParamA again for its Rename dialog, and
        // that nested pump drains and refills the same deque. An iterator left
        // pointing into the deque across the dispatch is invalidated by the
        // nested pump's erase, so the next `*it` reads a freed block (the
        // profile-rename SIGSEGV). Copy front()/pop_front() keeps no iterator
        // alive across guest code.
        auto &q = queue();
        while (!q.empty()) {
            Msg m = q.front();
            q.pop_front();
            if (m.message == 0x0012) { // WM_QUIT ends the dialog
                dialog_running[hwnd] = false;
                break;
            }
            last_message = m;
            // IsDialogMessage first: Return presses the default push button,
            // Escape cancels, Tab moves the focus. Windows does this in the
            // message loop, before DispatchMessage.
            if (!user32::dialog_key_message(c, hwnd, m.message, m.wparam, m.lparam)) {
                uint32_t target = find_window(m.hwnd) ? m.hwnd : hwnd;
                host_dispatch_to_wndproc(c, target, m.message, m.wparam, m.lparam);
            }
            dispatched = true;
            if (!dialog_running.count(hwnd) || !dialog_running[hwnd])
                break;
        }
        if (!dialog_running.count(hwnd) || !dialog_running[hwnd])
            break;
        if (dispatched)
            continue;
        // Windows' own modal loop is a GetMessage loop, and GetMessage is what
        // synthesises WM_PAINT from an update region. Without this a dialog
        // engine that only drains the queue never paints anything.
        if (Window *pw = first_paint_window()) {
            // The dialog's own frame and client background are painted before
            // its client WM_PAINT, standing in for WM_NCPAINT/WM_ERASEBKGND, and
            // its children follow it (paint_window_tree).
            paint_window_tree(c, pw->hwnd);
            continue;
        }
        if (!g_message_waiter) {
            log_once("DialogBoxParamA-nohost",
                     "DialogBoxParamA: nothing is queued and there is no host to wait on");
            break;
        }
        g_message_waiter();
        if (!host_guest_yield())
            guest_sleep_ms(1);
    }
    int32_t result = dialog_results.count(hwnd) ? dialog_results[hwnd] : 0;
    dialog_results.erase(hwnd);
    dialog_running.erase(hwnd);
    if (find_window(hwnd))
        destroy_window(c, hwnd);
    set_eax(c, uint32_t(result));
}

// A modeless dialog: build the window and its controls, send WM_INITDIALOG and
// return the handle. The guest's own message loop drives it afterwards, which
// is why the splash/gauge screen (whose wndproc paints a pre-rendered bitmap)
// only appears once this exists.
void u_CreateDialogParamA(X86 *c) {
    uint32_t hinst = arg(c, 0), name = arg(c, 1), parent = arg(c, 2), proc = arg(c, 3),
             init = arg(c, 4);
    if (!proc) {
        set_eax(c, 0);
        return;
    }
    std::string why;
    uint32_t entry = resource_find_in(hinst, 5 /* RT_DIALOG */, name, true, &why);
    uint32_t size = 0, p = entry ? resource_data(entry, &size, &why) : 0;
    DlgTemplate t;
    if (!p || !parse_dlg_template(p, size, &t)) {
        log_once("CreateDialogParamA-template",
                 "CreateDialogParamA: dialog template %u could not be read (%s)", name,
                 why.c_str());
        set_eax(c, 0);
        return;
    }
    uint32_t hwnd = create_dialog_window(c, hinst, parent, proc, t);
    if (!hwnd) {
        set_eax(c, 0);
        return;
    }
    host_dispatch_to_wndproc(c, hwnd, 0x0110 /* WM_INITDIALOG */, 0, init);
    user32::initial_dialog_focus(hwnd);
    set_eax(c, hwnd);
}

const ImportShim g_user32_shims[] = {
    {"USER32.dll", "RegisterClassA", 1, u_RegisterClassA},
    {"USER32.dll", "UnregisterClassA", 2, u_UnregisterClassA},
    {"USER32.dll", "CreateWindowExA", 12, u_CreateWindowExA},
    {"USER32.dll", "DestroyWindow", 1, u_DestroyWindow},
    {"USER32.dll", "ShowWindow", 2, u_ShowWindow},
    {"USER32.dll", "UpdateWindow", 1, u_UpdateWindow},
    {"USER32.dll", "SetWindowPos", 7, u_SetWindowPos},
    {"USER32.dll", "GetWindowRect", 2, u_GetWindowRect},
    {"USER32.dll", "GetClientRect", 2, u_GetClientRect},
    {"USER32.dll", "SystemParametersInfoA", 4, u_SystemParametersInfoA},
    {"USER32.dll", "AdjustWindowRectEx", 4, u_AdjustWindowRectEx},
    {"USER32.dll", "ClientToScreen", 2, u_ClientToScreen},
    {"USER32.dll", "SetRect", 5, u_SetRect},
    {"USER32.dll", "InvalidateRect", 3, u_InvalidateRect},
    {"USER32.dll", "SetLayeredWindowAttributes", 4, u_SetLayeredWindowAttributes},
    {"USER32.dll", "GetLayeredWindowAttributes", 4, u_GetLayeredWindowAttributes},
    {"USER32.dll", "SetWindowLongA", 3, u_SetWindowLongA},
    {"USER32.dll", "GetWindowLongA", 2, u_GetWindowLongA},
    {"USER32.dll", "SetWindowTextA", 2, u_SetWindowTextA},
    {"USER32.dll", "SetDlgItemTextA", 3, u_SetDlgItemTextA},
    {"USER32.dll", "CreateDialogParamA", 5, u_CreateDialogParamA},
    {"USER32.dll", "PeekMessageA", 5, u_PeekMessageA},
    {"USER32.dll", "GetMessageA", 4, u_GetMessageA},
    {"USER32.dll", "GetMessagePos", 0, u_GetMessagePos},
    {"USER32.dll", "GetMessageTime", 0, u_GetMessageTime},
    {"USER32.dll", "TranslateMessage", 1, u_TranslateMessage},
    {"USER32.dll", "DispatchMessageA", 1, u_DispatchMessageA},
    {"USER32.dll", "PostMessageA", 4, u_PostMessageA},
    {"USER32.dll", "DefWindowProcA", 4, u_DefWindowProcA},
    {"USER32.dll", "GetDC", 1, u_GetDC},
    {"USER32.dll", "ReleaseDC", 2, u_ReleaseDC},
    {"USER32.dll", "BeginPaint", 2, u_BeginPaint},
    {"USER32.dll", "EndPaint", 2, u_EndPaint},
    {"USER32.dll", "LoadIconA", 2, u_LoadIconA},
    {"USER32.dll", "LoadCursorA", 2, u_LoadCursorA},
    {"USER32.dll", "ValidateRect", 2, u_ValidateRect},
    {"USER32.dll", "GetUpdateRect", 3, u_GetUpdateRect},
    {"USER32.dll", "GetScrollBarInfo", 3, nullptr},
    {"USER32.dll", "LoadCursorFromFileA", 1, u_LoadCursorFromFileW},
    {"USER32.dll", "LoadCursorFromFileW", 1, u_LoadCursorFromFileW},
    {"USER32.dll", "CreateIconIndirect", 1, u_CreateIconIndirect},
    {"USER32.dll", "ScreenToClient", 2, u_ScreenToClient},
    {"USER32.dll", "OpenIcon", 1, u_OpenIcon},
    {"USER32.dll", "FindWindowA", 2, u_FindWindowA},
    {"USER32.dll", "DestroyIcon", 1, u_DestroyIcon},
    {"USER32.dll", "GetSystemMetrics", 1, u_GetSystemMetrics},
    {"USER32.dll", "IsWindowUnicode", 1, u_IsWindowUnicode},
    {"USER32.dll", "SetCursor", 1, u_SetCursor},
    {"USER32.dll", "SetCursorPos", 2, u_SetCursorPos},
    {"USER32.dll", "GetDoubleClickTime", 0, u_GetDoubleClickTime},
    {"USER32.dll", "GetKeyboardType", 1, u_GetKeyboardType},
    {"USER32.dll", "GetKeyboardLayout", 1, u_GetKeyboardLayout},
    {"USER32.dll", "MessageBoxA", 4, u_MessageBoxA},
    {"USER32.dll", "MessageBoxW", 4, u_MessageBoxW},
    {"USER32.dll", "wvsprintfA", 3, u_wvsprintfA},
    // Not imported by D3DPopTB.exe, but registered so GetProcAddress and the
    // host layer can reach them.
    {"USER32.dll", "SendMessageA", 4, u_SendMessageA},
    {"USER32.dll", "PostQuitMessage", 1, u_PostQuitMessage},
    {"USER32.dll", "GetCursorPos", 1, u_GetCursorPos},
    {"USER32.dll", "GetAsyncKeyState", 1, u_GetAsyncKeyState},
    {"USER32.dll", "GetKeyState", 1, u_GetKeyState},
    {"USER32.dll", "ShowCursor", 1, u_ShowCursor},
    {"USER32.dll", "ClipCursor", 1, u_ClipCursor},
    {"USER32.dll", "GetClipCursor", 1, u_GetClipCursor},
    {"USER32.dll", "RegisterClassExA", 1, u_RegisterClassExA},
    {"USER32.dll", "AdjustWindowRect", 3, u_AdjustWindowRect},
    {"USER32.dll", "SetCapture", 1, u_SetCapture},
    {"USER32.dll", "ReleaseCapture", 0, u_ReleaseCapture},
    {"USER32.dll", "GetDesktopWindow", 0, nullptr},
    {"USER32.dll", "MapVirtualKeyA", 2, u_MapVirtualKeyA},
    {"USER32.dll", "MapVirtualKeyExA", 3, u_MapVirtualKeyA},
    {"USER32.dll", "ToUnicode", 6, nullptr},
    {"USER32.dll", "SendInput", 3, nullptr},
    {"USER32.dll", "PostThreadMessageA", 4, nullptr},
    {"USER32.dll", "GetForegroundWindow", 0, u_GetForegroundWindow},
    {"USER32.dll", "WaitMessage", 0, u_WaitMessage},
    {"USER32.dll", "GetActiveWindow", 0, u_GetActiveWindow},
    {"USER32.dll", "SetFocus", 1, u_SetFocus},
    {"USER32.dll", "GetMenu", 1, u_GetMenu},
    {"USER32.dll", "IsIconic", 1, u_IsIconic},
    {"USER32.dll", "CloseWindow", 1, u_CloseWindow},
    {"USER32.dll", "SetForegroundWindow", 1, u_SetForegroundWindow},
    {"USER32.dll", "SetActiveWindow", 1, u_SetActiveWindow},
    {"USER32.dll", "OpenClipboard", 1, u_OpenClipboard},
    {"USER32.dll", "CloseClipboard", 0, u_CloseClipboard},
    {"USER32.dll", "IsClipboardFormatAvailable", 1, u_IsClipboardFormatAvailable},
    {"USER32.dll", "GetClipboardData", 1, u_GetClipboardData},
    // Rectangles, window placement and dialogs the game's UI needs.
    {"USER32.dll", "InflateRect", 3, u_InflateRect},
    {"USER32.dll", "OffsetRect", 3, u_OffsetRect},
    {"USER32.dll", "PtInRect", 3, u_PtInRect},
    {"USER32.dll", "InvertRect", 2, u_InvertRect},
    {"USER32.dll", "MoveWindow", 6, u_MoveWindow},
    {"USER32.dll", "BringWindowToTop", 1, u_BringWindowToTop},
    {"USER32.dll", "GetWindowTextLengthA", 1, u_GetWindowTextLengthA},
    {"USER32.dll", "ScrollWindowEx", 8, u_ScrollWindowEx},
    {"USER32.dll", "CallWindowProcA", 5, u_CallWindowProcA},
    {"USER32.dll", "DrawStateA", 10, u_DrawStateA},
    {"USER32.dll", "WinHelpA", 4, u_WinHelpA},
    {"USER32.dll", "GetDlgItem", 2, u_GetDlgItem},
    {"USER32.dll", "GetDlgItemTextA", 4, u_GetDlgItemTextA},
    {"USER32.dll", "SetDlgItemInt", 4, u_SetDlgItemInt},
    {"USER32.dll", "GetDlgItemInt", 4, u_GetDlgItemInt},
    {"USER32.dll", "SendDlgItemMessageA", 5, u_SendDlgItemMessageA},
    {"USER32.dll", "CheckDlgButton", 3, u_CheckDlgButton},
    {"USER32.dll", "IsDlgButtonChecked", 2, u_IsDlgButtonChecked},
    {"USER32.dll", "EndDialog", 2, u_EndDialog},
    {"USER32.dll", "DialogBoxParamA", 5, u_DialogBoxParamA},
    {"USER32.dll", "MapDialogRect", 2, u_MapDialogRect},
};
const size_t g_user32_shim_count = sizeof(g_user32_shims) / sizeof(g_user32_shims[0]);
