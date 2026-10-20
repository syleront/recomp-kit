// Internal USER32 model. All access is on the guest scheduler baton; guest
// callbacks may erase windows, so never retain a Window pointer across one.
#pragma once
#include "imports.h"
#include "gdi32_internal.h"
#include "win32.h"
#include <deque>
#include <map>
#include <set>
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
    // GWL_USERDATA: storage the PROGRAM owns, read and written only through
    // GetWindowLongA/SetWindowLongA. The button class's own pressed/check state
    // lives in `pressed`/`check_state` below, NOT here: a program that stores a
    // subclass procedure in GWL_USERDATA (the Petz 5 rename edit does) must get
    // it back bit for bit, so an edit click may not OR a pressed bit into it.
    uint32_t userdata = 0;
    // The button class's press/check state. A press is not a check state and a
    // check state is not GWL_USERDATA (TD-079).
    bool pressed = false;
    uint32_t check_state = 0;
    // An EDIT control's text model: `caret` is a byte index into title_utf8 and
    // `sel_anchor` the other end of the selection. The selection is
    // [min(sel_anchor,caret), max(...)); empty when the two are equal. Windows
    // selects the whole text when the dialog manager gives an edit the focus.
    int32_t caret = 0;
    int32_t sel_anchor = 0;
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
    // and one selection; the engine paints them and a click selects one. A
    // combo box's drop-down and a tab control's tabs share the same model, since
    // none of the three ever coexists on one window.
    std::vector<std::string> list_items;
    int32_t list_sel = -1;
    int32_t list_top = 0;
    // A multi-select (LBS_MULTIPLESEL / LBS_EXTENDEDSEL) listbox's selected
    // rows. A single-select box keeps this in step with `list_sel` (the click
    // and LB_SETCURSEL replace it), so LB_GETSELCOUNT/LB_GETSEL/LB_GETSELITEMS
    // answer for either style. The Contact Sound page (Petz 5's Editor) is
    // LBS_EXTENDEDSEL and drives its selection entirely through LB_SETSEL.
    std::set<int32_t> list_selected;
    // A range control's model, shared by the trackbar, the scrollbar and the
    // up-down: `range_min`/`range_max` are the inclusive bounds, `range_pos` the
    // current value, `range_page` a page step and `range_line` a line step. The
    // progress bar uses min/max/pos too.
    int32_t range_min = 0, range_max = 100, range_pos = 0;
    int32_t range_page = 0, range_line = 1; // 0 = the class default page step
    // A combo box's dropped state and the row the pointer is over while it is
    // open (`combo_hot`, -1 when none).
    bool combo_open = false;
    int32_t combo_hot = -1;
    // A combo box's DROPPED height in pixels (the template's cy for a drop-down;
    // Windows creates the control at its one-row closed height and the list
    // extends to this). Zero for a non-combo window.
    int32_t combo_drop_h = 0;
    // The combo's open drop-down: a top-level popup window with its own surface,
    // so the list is composited over the dialog rather than drawn into it (a
    // control's DC is clipped to its own rectangle, and a dialog's client may be
    // transparent). Zero when closed.
    uint32_t combo_popup = 0;
    // An up-down's buddy edit (UDM_SETBUDDY) and its number base.
    uint32_t spin_buddy = 0;
    int32_t spin_base = 10;
    // A trackbar/scrollbar thumb drag in progress.
    bool thumb_drag = false;
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
    // True on a dialog built by CreateDialogParamA. The guest's own message loop
    // paints such a dialog (there is no modal pump to do it), so the paint
    // synthesis paints its frame and client background once before the first
    // client paint (TD-072/TD-108). `dlg_client_painted` records that the
    // one-time chrome/erase has run; later paints must not refill the client,
    // which would erase what the dialog's own WM_PAINT drew (the splash).
    bool dlg_modeless = false;
    bool dlg_client_painted = false;
    // A combo box's per-item data (CB_SETITEMDATA/CB_GETITEMDATA), kept in step
    // with list_items. A listbox has its own item data path and never uses this.
    std::vector<uint32_t> combo_item_data;
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
// A scan code whose character the host has ALREADY delivered as a WM_CHAR. The
// host stands in for the OS and pre-translates a key (it posts WM_CHAR next to
// WM_KEYDOWN) because the modal DialogBoxParamA pump never calls
// TranslateMessage. A guest message loop DOES call TranslateMessage, which
// would synthesise a second WM_CHAR for the same physical key and type every
// character twice (the Editor's "Playscene name" page). TranslateMessage skips
// a scan code marked here and clears it; host_post_key_message sets it. Keyed
// by scan code, which the key lParam carries, so it pairs with the key it came
// from and a host character message without one (the headless `text=` seam
// posts scan code 0) marks nothing.
extern uint8_t g_host_char_pending[256];
extern bool (*g_message_waiter)();
extern Msg last_message;
constexpr uint32_t desktop_handle = 0x00020000;
void pump_window_timers();
// Whether some visible window has an update region a paint would answer.
bool paint_pending();
void window_created(uint32_t hwnd);
// The next free window handle. Exposed so a runtime-owned window (the popup
// menu) is allocated from the same sequence as every other one.
uint32_t alloc_window_handle();
// The first visible window with an update region, in paint order (the modal
// pump's paint synthesis asks for it).
Window *first_paint_window();
// Give the keyboard focus to a window (the dialog engine's first tab stop).
void set_focus_window(uint32_t hwnd);
// Give an edit control the focus the way the dialog manager does: the whole
// text is selected, so typing replaces it. Does nothing on a non-edit.
void focus_edit_select_all(uint32_t hwnd);
// The dialog manager's post-WM_INITDIALOG focus: keep whatever the procedure
// focused (and select an edit's text), else focus the first WS_TABSTOP control.
// Called once the procedure has filled the controls.
void initial_dialog_focus(uint32_t dlg);
// The dialog engine's IsDialogMessage: VK_RETURN presses the default push
// button (IDOK unless a BS_DEFPUSHBUTTON names another id), VK_ESCAPE sends
// IDCANCEL, VK_TAB moves the focus to the next/previous WS_TABSTOP control, and
// any of the three selects an edit's whole text when it takes the focus. A
// multiline edit keeps Return, Escape and Tab for itself. Returns true when the
// message was consumed. Windows runs this before DispatchMessage, so the modal
// pump (not the guest's own loop) has to do it.
bool dialog_key_message(X86 *c, uint32_t dlg, uint32_t msg, uint32_t wparam, uint32_t lparam);
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
// Mouse capture (SetCapture): the window that receives every pointer message
// regardless of where the pointer is. Exposed so a runtime-owned control that
// needs the pointer (a combo box's open drop-down) can take it, and the input
// pump can release it when the drag/drop finishes.
void set_capture_window(uint32_t hwnd);
uint32_t capture_window();
// A framed dialog's non-client hit test, exactly as DefWindowProc answers
// WM_NCHITTEST: HTCLOSE on the close box, HTCAPTION on the caption, HTBORDER on
// the frame, HTCLIENT in the client area. `x`/`y` are CLIENT coordinates (a
// point in the non-client area is negative or past the client size).
int32_t dialog_hit_test(const Window *w, int32_t x, int32_t y);
// A caption drag (Windows' HTCAPTION move loop): begin it at a screen point on
// a framed dialog's caption, move the window with the pointer, and end it on
// the button release. The input pump drives all three.
bool dialog_begin_move(X86 *c, uint32_t hwnd, int32_t screen_x, int32_t screen_y);
void dialog_move_to(X86 *c, int32_t screen_x, int32_t screen_y);
void dialog_end_move();
bool dialog_move_active();
void forget_window_services(uint32_t hwnd);
bool destroy_window(X86 *c, uint32_t hwnd);
// Ask every visible window to repaint: what a hidden or destroyed window
// covered has to be drawn again (there is no damage region here).
void repaint_exposed_windows();
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
// Alt+F4 and Alt+Space on a dialog (Windows' DefDlgProc/DefWindowProc system
// commands). `msg` is WM_KEYDOWN/WM_SYSKEYDOWN and `vk` the virtual key; the
// Alt state is read from the guest's key state. Returns true when the message
// was a system command and must not be dispatched: Alt+F4 asks the dialog to
// close (WM_COMMAND IDCANCEL then WM_CLOSE, the close-box rule), Alt+Space
// opens its system menu at the caption. Only a #32770 with WS_SYSMENU answers.
bool dialog_system_key(X86 *c, uint32_t hwnd, uint32_t msg, uint32_t vk);
// Pop up a dialog's system menu (Move/Close) at a screen point and apply the
// chosen command. Only a framed #32770 with WS_SYSMENU has one.
void dialog_open_system_menu(X86 *c, uint32_t hwnd, int32_t screen_x, int32_t screen_y);
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
// Paint one window the way the modal pump does: its chrome and background
// erase, its own WM_PAINT, and then its visible children in z-order. A control
// has no surface of its own - its DC writes into the top-level window's surface
// - so a parent's background fill would otherwise be the last thing written over
// a control that was painted first. Windows never has that problem (a control
// owns its pixels), which is why the runtime repaints the children right after
// the parent: the result no longer depends on the order invalidations arrived
// in. The pump's paint order (see first_paint_window) puts a window's pending
// ancestors before it as well.
void paint_window_tree(X86 *c, uint32_t hwnd);
// Registers the dialog control classes ("button", "edit", "listbox", ...) on
// first use. The message loop calls it before a dialog is created; a direct
// CreateWindowExA of a control class has to as well.
void ensure_dialog_classes();
void user32_wide_register();

void user32_vcl_register();
