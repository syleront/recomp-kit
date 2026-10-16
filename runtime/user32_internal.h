// Internal USER32 model. All access is on the guest scheduler baton; guest
// callbacks may erase windows, so never retain a Window pointer across one.
#pragma once
#include "imports.h"
#include "gdi32_internal.h"
#include "win32.h"
#include <deque>
#include <map>
#include <string>
#include <vector>
namespace user32 {
struct WndClass {
    uint32_t style = 0;
    uint32_t wndproc = 0;
    uint32_t cls_extra = 0;
    uint32_t wnd_extra = 0;
    uint32_t hinstance = 0;
    uint32_t hicon = 0;
    uint32_t hcursor = 0;
    uint32_t hbrush = 0;
    std::string menu, name;
    uint32_t menu_id = 0, atom = 0;
    bool unicode = false;
    std::vector<uint32_t> extra;
};

struct Window {
    gdi::Surface surface;
    uint32_t hwnd = 0;
    uint32_t wndproc = 0;
    uint32_t style = 0, exstyle = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;
    std::string cls, title_utf8;
    bool unicode = false;
    std::map<std::string, uint32_t> props;
    uint32_t menu = 0, parent = 0, owner = 0, id = 0;
    uint32_t thread = 0;
    bool enabled = true;
    bool owned_hidden = false;
    uint32_t region = 0;
    uint32_t show_cmd = 0;
    uint32_t userdata = 0;
    uint32_t font = 0; // WM_SETFONT, kept for the system control classes
    uint32_t hinstance = 0;
    std::vector<uint32_t> extra;
    bool visible = false;
    bool shown = false;
    // Windows tracks an update region per window; the runtime only needs to
    // know whether it is empty, which is what UpdateWindow and BeginPaint act
    // on. Showing a window invalidates it, painting it validates it.
    bool update_pending = false;
    // SetLayeredWindowAttributes: LWA_COLORKEY (1) makes every pixel of that
    // colour transparent, LWA_ALPHA (2) applies one opacity to the whole
    // window. Both are honoured where the window surfaces are composited.
    uint32_t layered_flags = 0, layered_key = 0, layered_alpha = 255;
    // Listbox content (LB_ADDSTRING and friends). A dialog listbox has strings
    // and one selection; the engine paints them and a click selects one.
    std::vector<std::string> list_items;
    int32_t list_sel = -1;
    int32_t list_top = 0;
    // The row height is one value for drawing, hit-testing, the item rects and
    // the scroll. It comes from LB_SETITEMHEIGHT or the parent's WM_MEASUREITEM
    // for an owner-draw box (0 until measured); an LBS_OWNERDRAWVARIABLE box can
    // give a different height per row, cached in list_item_heights.
    int32_t list_item_height = 0;
    std::vector<int32_t> list_item_heights;
    // Dialog base units, in pixels per 4 DLU across and per 8 DLU down, from the
    // template's DS_SETFONT face. Zero on a non-dialog window; MapDialogRect
    // then uses the 8x16 bitmap-cell default, matching a fontless template.
    int32_t dlg_base_x = 0, dlg_base_y = 0;
    // Non-client insets in pixels. A dialog's template rectangle is its CLIENT
    // size (Windows AdjustWindowRect semantics), so for a framed dialog x/y is
    // the OUTER origin, w/h stays the client size, and the client origin is
    // (x + nc_left, y + nc_top). Zero on every other window, so the game's own
    // frame window, its popups and its controls keep the old geometry exactly.
    int32_t nc_left = 0, nc_top = 0, nc_right = 0, nc_bottom = 0;
    // True when the caption bar above the client is drawn (WS_CAPTION); the
    // chrome painter needs it to place the title and the close box.
    bool nc_caption = false;
};

struct Msg {
    uint32_t hwnd, message, wparam, lparam, time, ptx, pty;
};

std::map<std::string, WndClass> &classes();
std::map<uint32_t, Window> &windows();
std::deque<Msg> &queue();
Window *find_window(uint32_t hwnd);
std::string class_key(uint32_t p, bool wide = false);
uint32_t wide_units(const std::string &text);
uint32_t put_text(uint32_t out, uint32_t cap, const std::string &text, bool wide);
void register_class_named(X86 *c, bool wide, uint32_t shift);
void create_window_named(X86 *c, bool wide);
void def_window_proc(X86 *c, bool wide);
void send_message(X86 *c, bool wide);
void peek_message(X86 *c);
void dispatch_message(X86 *c);
uint32_t deliver_message(X86 *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool wide);
extern uint32_t g_main_hwnd, g_cursor;
extern int32_t g_cursor_x, g_cursor_y;
extern uint8_t g_key_state[256];
extern bool (*g_message_waiter)();
extern Msg last_message;
constexpr uint32_t desktop_handle = 0x00020000;
void pump_window_timers();
// Whether some visible window has an update region a paint would answer.
bool paint_pending();
void window_created(uint32_t hwnd);
// Give the keyboard focus to a window (the dialog engine's first tab stop).
void set_focus_window(uint32_t hwnd);
// The window that currently has the keyboard focus (GetFocus's answer), so a
// control can report it in BM_GETSTATE and an owner can decide a tab order.
uint32_t focus_window();
// Dialog base units: the pixels a dialog unit spans, in quarters across and
// eighths down, from a DS_SETFONT face and point size. `dialog_map_unit` is the
// same scaling (Windows' MulDiv rounding) the template engine lays a dialog out
// with. Exposed so the mapping can be tested apart from the template decoder.
struct DlgBaseUnit {
    int32_t x = 8, y = 16;
};
DlgBaseUnit dialog_base_units(const std::string &face, int32_t pointsize, int32_t weight);
int32_t dialog_map_unit(int32_t dlu, int32_t base, int32_t denom);
// Siblings in back-to-front stacking order, respecting WS_EX_TOPMOST.
std::vector<uint32_t> window_z_order(uint32_t parent);
void reorder_window(uint32_t hwnd, uint32_t after);
void pump_mouse_input(X86 *c);
void forget_window_services(uint32_t hwnd);
bool destroy_window(X86 *c, uint32_t hwnd);
void client_origin(uint32_t hwnd, int32_t *x, int32_t *y);
// A window's OUTER (frame) origin: like client_origin but without the
// non-client insets. Equal to client_origin on every window without a frame.
void window_origin(uint32_t hwnd, int32_t *x, int32_t *y);
void display_rect(uint32_t out);
void alias_ansi(X86 *c, const char *name);
// Registers the system control classes (STATIC) on first use.
void ensure_system_classes();
// Paints a control's caption the way the STATIC class does (fill from the
// parent's WM_CTLCOLORSTATIC brush, then the text). Shared with the dialog
// engine's control_proc.
void paint_control(X86 *c, uint32_t hwnd, uint32_t into);
// Builds a dialog and its controls from a DLGTEMPLATE that is already in guest
// memory (address `tmpl`, `size` bytes), applying the template's DS_SETFONT
// face to the dialog and each control. The resource entry points share this
// path; the separate seam lets a test exercise the font application directly.
uint32_t create_dialog_from_template(X86 *c, uint32_t hinst, uint32_t parent, uint32_t proc,
                                     uint32_t tmpl, uint32_t size);
} // namespace user32

// The close box's rule (DefDlgProc): WM_COMMAND IDCANCEL, then WM_CLOSE when
// the dialog procedure does not end the dialog. A global so the input layer
// reaches it without the dialog engine's maps.
bool close_box_request(X86 *c, uint32_t hwnd);
// Paints a framed dialog's non-client chrome and client background before its
// client WM_PAINT, exactly as the modal pump does. Exposed so a test can render
// a dialog to an offscreen surface without running the pump.
void paint_dialog_chrome(X86 *c, uint32_t hwnd);
// Registers the dialog control classes ("button", "edit", "listbox", ...) on
// first use. The message loop calls it before a dialog is created; a direct
// CreateWindowExA of a control class has to as well.
void ensure_dialog_classes();
void user32_wide_register();

void user32_vcl_register();
