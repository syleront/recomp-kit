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
    wr32(r, x);
    wr32(r + 4, y);
    wr32(r + 8, x + w->w);
    wr32(r + 12, y + w->h);
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
    if (w)
        w->title_utf8 = gm_str(arg(c, 1));
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
        return w;
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
        if (w)
            w->title_utf8 = wide ? gm_wstr(arg(c, 3)) : gm_str(arg(c, 3));
        set_eax(c, w ? 1 : 0);
        return;
    case 0x000d: // WM_GETTEXT
        set_eax(c, put_text(arg(c, 3), arg(c, 2), w ? w->title_utf8 : "", wide));
        return;
    case 0x000e: // WM_GETTEXTLENGTH
        set_eax(c, w ? (wide ? wide_units(w->title_utf8) : uint32_t(w->title_utf8.size())) : 0);
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
    set_eax(c, gdi_window_dc(arg(c, 0)));
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
// The answer a message box gets when there is nobody to click it.
//
// There is no display, so the box cannot be shown and cannot be answered by a
// person. What it must not do is answer with a button the caller never
// offered: returning IDOK to an MB_YESNO box is not one of that box's two
// possible answers, and a caller that switches on the result then takes a path
// Windows could never have produced. So the reply is the default button of the
// button set the caller asked for, which is what pressing Return on the box
// gives, and MB_DEFBUTTON1/2/3 select which of them that is.
uint32_t message_box_default(uint32_t type) {
    // MB_OK, MB_OKCANCEL, MB_ABORTRETRYIGNORE, MB_YESNOCANCEL, MB_YESNO,
    // MB_RETRYCANCEL, MB_CANCELTRYCONTINUE, indexed by type & MB_TYPEMASK.
    static const uint8_t sets[7][3] = {
        {1, 0, 0},   // IDOK
        {1, 2, 0},   // IDOK, IDCANCEL
        {3, 4, 5},   // IDABORT, IDRETRY, IDIGNORE
        {6, 7, 2},   // IDYES, IDNO, IDCANCEL
        {6, 7, 0},   // IDYES, IDNO
        {4, 2, 0},   // IDRETRY, IDCANCEL
        {2, 10, 11}, // IDCANCEL, IDTRYAGAIN, IDCONTINUE
    };
    uint32_t set = type & 0x0000000fu;
    if (set > 6)
        set = 0;
    uint32_t want = (type & 0x00000f00u) >> 8; // MB_DEFBUTTON1/2/3
    if (want > 2 || sets[set][want] == 0)
        want = 0;
    return sets[set][want];
}

void u_MessageBoxA(X86 *c) {
    uint32_t answer = message_box_default(arg(c, 3));
    LOGW("MessageBoxA: [%s] %s -> default button %u", gm_str(arg(c, 2), 256).c_str(),
         gm_str(arg(c, 1), 1024).c_str(), answer);
    set_eax(c, answer);
}

void u_MessageBoxW(X86 *c) {
    std::string text;
    uint32_t p = arg(c, 1);
    for (int i = 0; i < 1024; ++i) {
        uint16_t w = rd16(p + 2 * i);
        if (!w)
            break;
        text.push_back((char)(w < 256 ? w : '?'));
    }
    uint32_t answer = message_box_default(arg(c, 3));
    LOGW("MessageBoxW: %s -> default button %u", text.c_str(), answer);
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
// DrawState renders a control-like glyph through a callback. The game uses it
// for its disabled/checked menu marks; there is no glyph to draw here, so the
// call succeeds without painting.
void u_DrawStateA(X86 *c) {
    log_once("DrawStateA", "DrawStateA: glyph not drawn; reporting success");
    set_eax(c, 1);
}
// WinHelp opens the old WinHelp engine, which does not exist here. Reporting
// success lets a caller that bails out on failure carry on.
void u_WinHelpA(X86 *c) {
    log_once("WinHelpA", "WinHelpA: help is unavailable; reporting success");
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
        wr32(ds + 0x00, 2);                          // CtlType = ODT_LISTBOX
        wr32(ds + 0x04, w->id);                      // CtlID
        wr32(ds + 0x08, uint32_t(i));                // itemID = the row index
        wr32(ds + 0x0c, 1);                          // itemAction = ODA_DRAWENTIRE
        wr32(ds + 0x10, i == w->list_sel ? 1u : 0u); // itemState = ODS_SELECTED
        wr32(ds + 0x14, hwnd);                       // hwndItem
        wr32(ds + 0x18, hdc);                        // hDC
        wr32(ds + 0x1c, 0);                          // rcItem.left
        wr32(ds + 0x20, uint32_t(y));                // rcItem.top
        wr32(ds + 0x24, uint32_t(width));            // rcItem.right
        wr32(ds + 0x28, uint32_t(y + ih));           // rcItem.bottom
        wr32(ds + 0x2c, 0);                          // itemData
        uint32_t r = host_dispatch_to_wndproc(c, w->parent, 0x002b /* WM_DRAWITEM */, w->id, ds);
        heap_free(ds);
        if (!r)
            return false;
        y += ih;
    }
    return true;
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
    if (!gdi::dc_of(hdc))
        return;
    gdi::fill(hdc, {0, 0, width, height}, 0x00ffffff);
    bool drawn = owner_draw_listbox(c, w, hwnd, hdc, width, height);
    if (drawn) {
        if (!into)
            gdi_release_window_dc(hwnd, hdc);
        return;
    }
    int32_t y = 0;
    for (int32_t i = w->list_top; i < int32_t(w->list_items.size()) && y < height; ++i) {
        int32_t ih = listbox_item_height(c, w, i);
        if (i == w->list_sel)
            gdi::fill(hdc, {0, y, width, y + ih}, 0x00c00000);
        uint32_t units = wide_units(w->list_items[i]);
        if (!units) {
            y += ih;
            continue;
        }
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            wr32(block + 0, 2);
            wr32(block + 4, y);
            wr32(block + 8, width - 2);
            wr32(block + 12, y + ih);
            gm_put_wstr(block + 16, w->list_items[i], units + 1);
            gdi::draw_text(hdc, block + 16, units, block, 0x24 /* DT_SINGLELINE|DT_VCENTER */);
            heap_free(block);
        }
        y += ih;
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
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
        else
            paint_control(c, hwnd, arg(c, 2));
        set_eax(c, 0);
        return;
    case 0x000c: // WM_SETTEXT: the new caption has to be drawn
        def_window_proc(c, w && w->unicode);
        if (Window *after = find_window(hwnd))
            after->update_pending = true;
        return;
    case 0x0102: { // WM_CHAR: an edit control appends the typed character
        if (w && w->cls == "edit") {
            uint32_t ch = arg(c, 2);
            if (ch == 8) {
                if (!w->title_utf8.empty())
                    w->title_utf8.pop_back();
            } else if (ch >= 0x20 && ch < 0x7f) {
                w->title_utf8.push_back(char(ch));
            }
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    }
    case 0x0100: { // WM_KEYDOWN: listbox navigation, backspace edits
        uint32_t vk = arg(c, 2);
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
                listbox_scroll_into_view(c, w);
                w->update_pending = true;
                if (w->parent)
                    host_dispatch_to_wndproc(c, w->parent, 0x0111 /* WM_COMMAND */, w->id & 0xffffu,
                                             hwnd); // LBN_SELCHANGE
            }
            set_eax(c, 0);
            return;
        }
        if (w && w->cls == "edit" && vk == 8) {
            if (!w->title_utf8.empty())
                w->title_utf8.pop_back();
            w->update_pending = true;
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
            // is keyed by index, so drop it and measure the moved rows again.
            w->list_item_heights.clear();
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
            w->list_item_heights.clear();
            w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0186: { // LB_SETCURSEL
        int32_t i = int32_t(arg(c, 2));
        if (w)
            w->list_sel = (i < 0 || i >= int32_t(w->list_items.size())) ? -1 : i;
        if (w)
            w->update_pending = true;
        set_eax(c, w ? uint32_t(w->list_sel) : 0xffffffffu);
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
    case 0x0190: { // LB_SELECTSTRING
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
        if (w && found >= 0) {
            w->list_sel = found;
            w->update_pending = true;
        }
        set_eax(c, found < 0 ? 0xffffffffu : uint32_t(found));
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
        set_eax(c, w ? uint32_t(listbox_item_height(c, w, int32_t(arg(c, 2)))) : 0xffffffffu);
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
    case 0x00f1: // BM_SETCHECK
        if (w)
            w->userdata = arg(c, 2);
        set_eax(c, 0);
        return;
    case 0x00f0: // BM_GETCHECK
        set_eax(c, w ? w->userdata : 0);
        return;
    case 0x00f2: // BM_GETSTATE
        set_eax(c, 0);
        return;
    case 0x0201: // WM_LBUTTONDOWN: remember the press so the click is a pair
        LOGW("control: WM_LBUTTONDOWN hwnd=%08x id=%u", hwnd, w ? w->id : 0);
        if (w && w->cls == "listbox") {
            // Windows gives the keyboard to a listbox that is clicked, so the
            // arrow keys that follow reach it. The dialog manager otherwise
            // leaves focus on the first tab stop (its OK button).
            set_focus_window(hwnd);
            // The click row comes from the same heights the paint and
            // LB_GETITEMRECT use; a hard 16 px divisor here selected the wrong
            // row once WM_MEASUREITEM gave the box a different height.
            int32_t row = listbox_item_from_y(c, w, int16_t((arg(c, 3) >> 16) & 0xffff));
            if (row >= 0 && row != w->list_sel) {
                w->list_sel = row;
                w->update_pending = true;
                if (w->parent)
                    host_dispatch_to_wndproc(c, w->parent, 0x0111 /* WM_COMMAND */, w->id & 0xffffu,
                                             hwnd); // LBN_SELCHANGE
            }
            set_eax(c, 0);
            return;
        }
        if (w)
            w->userdata |= 1u;
        set_eax(c, 0);
        return;
    case 0x0202: { // WM_LBUTTONUP: a completed click reports WM_COMMAND
        uint32_t parent = w ? w->parent : 0, id = w ? w->id : 0;
        LOGW("control: WM_LBUTTONUP hwnd=%08x id=%u parent=%08x", hwnd, id, parent);
        if (w)
            w->userdata &= ~1u;
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
    for (const char *name : {"button", "edit", "combobox", "listbox", "scrollbar"}) {
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
uint32_t dlg_create_child(uint32_t parent, const DlgItemTemplate &it, const DlgBaseUnit &base) {
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
    w.title_utf8 = it.text;
    w.parent = parent;
    w.id = it.id;
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
uint32_t create_dialog_window(uint32_t hinst, uint32_t parent, uint32_t proc,
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
    // face; the pixel size is what the window and its controls are placed with.
    DlgBaseUnit base;
    if ((t.style & 0x40 /*DS_SETFONT / DS_SHELLFONT*/) && t.pointsize > 0)
        base = dialog_base_units(t.font, t.pointsize, t.weight);
    w.dlg_base_x = base.x;
    w.dlg_base_y = base.y;
    w.w = dialog_map_unit(t.cx, base.x, 4);
    w.h = dialog_map_unit(t.cy, base.y, 8);
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
        w.x = (ow - w.w) / 2;
        w.y = (oh - w.h) / 2;
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
    w.cls = "#32770";
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
        dlg_create_child(hwnd, item, base);
    // The dialog manager gives the keyboard to the first tab stop, which is
    // where the host's keystrokes go (an edit control in a naming dialog).
    for (const auto &kv : windows())
        if (kv.second.parent == hwnd && kv.second.visible &&
            (kv.second.style & 0x10000 /*WS_TABSTOP*/)) {
            set_focus_window(kv.first);
            break;
        }
    return hwnd;
}
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
    uint32_t hwnd = create_dialog_window(hinst, parent, proc, t);
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
    while (dialog_running.count(hwnd) && dialog_running[hwnd]) {
        host_pump_timers(c);
        pump_window_timers();
        pump_mouse_input(c);
        gdi_present_windows();
        bool dispatched = false;
        auto &q = queue();
        for (auto it = q.begin(); it != q.end();) {
            Msg m = *it;
            it = q.erase(it);
            if (m.message == 0x0012) { // WM_QUIT ends the dialog
                dialog_running[hwnd] = false;
                break;
            }
            last_message = m;
            uint32_t target = find_window(m.hwnd) ? m.hwnd : hwnd;
            host_dispatch_to_wndproc(c, target, m.message, m.wparam, m.lparam);
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
            host_dispatch_to_wndproc(c, pw->hwnd, 0x000f /* WM_PAINT */, 0, 0);
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
    uint32_t hwnd = create_dialog_window(hinst, parent, proc, t);
    if (!hwnd) {
        set_eax(c, 0);
        return;
    }
    host_dispatch_to_wndproc(c, hwnd, 0x0110 /* WM_INITDIALOG */, 0, init);
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
