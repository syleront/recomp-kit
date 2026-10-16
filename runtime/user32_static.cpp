// The system STATIC control. Windows draws a static control's caption in the
// class's own window procedure, and a program that subclasses the class - the
// VCL's TStaticText does, through GetClassInfoW - forwards every message it
// leaves unhandled there. Without the class that lookup fails, the subclass
// falls back to DefWindowProc, and every caption stays blank.
#include "memory.h"
#include "user32_internal.h"
#include <climits>

namespace user32 {
namespace {
// DrawText flags for the styles that show text (SS_TYPEMASK), and 0 for the
// ones that show an icon, a bitmap or a frame instead.
uint32_t text_flags(uint32_t style) {
    constexpr uint32_t center = 1, right = 2, vcenter = 4, wordbreak = 0x10, singleline = 0x20,
                       expandtabs = 0x40, noclip = 0x100, noprefix = 0x800;
    uint32_t flags;
    switch (style & 0x1f) {
    case 0x0: // SS_LEFT
        flags = wordbreak | expandtabs;
        break;
    case 0x1: // SS_CENTER
        flags = center | wordbreak | expandtabs;
        break;
    case 0x2: // SS_RIGHT
        flags = right | wordbreak | expandtabs;
        break;
    case 0xb: // SS_SIMPLE
        flags = singleline | noclip;
        break;
    case 0xc: // SS_LEFTNOWORDWRAP
        flags = expandtabs;
        break;
    default:
        return 0;
    }
    if (style & 0x80) // SS_NOPREFIX
        flags |= noprefix;
    if (style & 0x200) // SS_CENTERIMAGE
        flags |= vcenter | singleline;
    return flags;
}

// `into` is WM_PAINT's or WM_PRINTCLIENT's wParam: a DC to paint into instead
// of the window. A program that double-buffers its controls - the VCL does -
// paints them into a memory DC this way and copies it over the window after,
// so painting the window directly would be covered by that copy.
} // namespace

// Shared with the dialog engine's control_proc, so a dialog control the engine
// created paints its caption the same way the STATIC class does.
void paint_control(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false; // painting validates, as BeginPaint does
    const uint32_t parent = w->parent, font = w->font, flags = text_flags(w->style);
    const int32_t width = w->w, height = w->h;
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc)
        return;
    const uint32_t caller_font = dc->font;
    if (font)
        dc->font = font;
    // The parent chooses the colours: it sets them on the DC and returns the
    // background brush, or a null brush to leave what is underneath. The VCL
    // reflects the message back to the control, which is where it answers.
    uint32_t brush =
        parent ? host_dispatch_to_wndproc(c, parent, 0x0138 /* WM_CTLCOLORSTATIC */, hdc, hwnd) : 0;
    uint32_t pixel;
    bool have_pixel = brush && gdi::brush_color(brush, &pixel);
    if (!have_pixel && parent) {
        // A null brush from WM_CTLCOLORSTATIC is "do not erase": the control
        // leaves whatever its parent already painted. Windows' DefDlgProc
        // passes the message on rather than inventing a brush, so a dialog that
        // leaves it unhandled (Petz 5's profile procedure returns 0) gets a
        // transparent control. The dialog engine has already filled the whole
        // client with its face colour (paint_dialog_chrome), so a caption still
        // sits on the dialog face - and a dialog procedure that has drawn a
        // picture into its own client DC (the profile's pet portrait, which it
        // BitBlts into the BeginPaint DC at this control's rectangle) keeps it.
        uint32_t dlg_brush =
            host_dispatch_to_wndproc(c, parent, 0x0131 /* WM_CTLCOLORDLG */, hdc, parent);
        have_pixel = dlg_brush && gdi::brush_color(dlg_brush, &pixel);
    }
    if (have_pixel)
        gdi::fill(hdc, {0, 0, width, height}, pixel);
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (flags && units && units < (UINT32_MAX - 32) / 2) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            wr32(block + 8, uint32_t(width));
            wr32(block + 12, uint32_t(height));
            gm_put_wstr(block + 16, text, units + 1);
            gdi::draw_text(hdc, block + 16, units, block, flags);
            heap_free(block);
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc)) // the caller's DC keeps its own font
        caller->font = caller_font;
}

// An EDIT control. The dialog engine's generic control path knows only a STATIC
// caption, so an edit's own text (set with SetDlgItemTextA/WM_SETTEXT - Petz 5's
// profile fills both of its multi-line edits with the pet's information) was
// never painted, and neither was the field itself. Windows draws the client in
// the parent's brush (WM_CTLCOLOREDIT, or WM_CTLCOLORSTATIC for a read-only
// edit; COLOR_WINDOW when the dialog procedure answers nothing) inside a sunken
// border, with the text left-aligned and clipped to the field.
void paint_edit(X86 *c, uint32_t hwnd, uint32_t into) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (!into)
        w->update_pending = false;
    const int32_t width = w->w, height = w->h;
    const uint32_t parent = w->parent;
    const std::string text = w->title_utf8;
    uint32_t hdc = into ? into : gdi_window_dc(hwnd);
    auto *dc = gdi::dc_of(hdc);
    if (!dc)
        return;
    const uint32_t caller_font = dc->font;
    if (w->font)
        dc->font = w->font;
    const bool readonly = (w->style & 0x0800u) != 0; // ES_READONLY
    uint32_t brush = parent ? host_dispatch_to_wndproc(c, parent,
                                                       readonly ? 0x0138u /*WM_CTLCOLORSTATIC*/
                                                                : 0x0133u /*WM_CTLCOLOREDIT*/,
                                                       hdc, hwnd)
                            : 0;
    uint32_t pixel = gdi::argb(0x00ffffff); // COLOR_WINDOW
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
    uint32_t units = text.empty() ? 0 : wide_units(text);
    if (units && units < (UINT32_MAX - 32) / 2 && width > 2 * inset && height > 2 * inset) {
        if (uint32_t block = heap_alloc(16 + 2 * (units + 1), true)) {
            dc->text_color = 0x00000000;
            dc->bk_mode = 1; // TRANSPARENT
            wr32(block + 0, uint32_t(inset + 2));
            wr32(block + 4, uint32_t(inset + 1));
            wr32(block + 8, uint32_t(width - inset - 1));
            wr32(block + 12, uint32_t(height - inset));
            gm_put_wstr(block + 16, text, units + 1);
            uint32_t flags = 0x800; // DT_NOPREFIX
            if (w->style & 0x0004u) // ES_MULTILINE
                flags |= 0x10;      // DT_WORDBREAK
            else
                flags |= 0x20 | 0x4; // DT_SINGLELINE | DT_VCENTER (DT_LEFT is 0)
            gdi::draw_text(hdc, block + 16, units, block, flags);
            heap_free(block);
        }
    }
    if (!into)
        gdi_release_window_dc(hwnd, hdc);
    else if (auto *caller = gdi::dc_of(hdc))
        caller->font = caller_font;
}

namespace {
void static_proc(X86 *c) {
    uint32_t hwnd = arg(c, 0), msg = arg(c, 1);
    Window *w = find_window(hwnd);
    switch (msg) {
    case 0x000c: // WM_SETTEXT: the new caption has to be drawn
        def_window_proc(c, w && w->unicode);
        if (Window *after = find_window(hwnd))
            after->update_pending = true;
        return;
    case 0x000f: // WM_PAINT
    case 0x0318: // WM_PRINTCLIENT
        paint_control(c, hwnd, arg(c, 2));
        set_eax(c, 0);
        return;
    case 0x0030: // WM_SETFONT
        if (w) {
            w->font = arg(c, 2);
            if (arg(c, 3))
                w->update_pending = true;
        }
        set_eax(c, 0);
        return;
    case 0x0031: // WM_GETFONT
        set_eax(c, w ? w->font : 0);
        return;
    default:
        def_window_proc(c, w && w->unicode);
    }
}
} // namespace

void ensure_system_classes() {
    if (classes().count("static"))
        return;
    WndClass wc;
    wc.style = 0x4088; // CS_GLOBALCLASS | CS_PARENTDC | CS_DBLCLKS
    wc.wndproc = imports_alloc_trampoline("USER32.dll", "STATIC window procedure", static_proc, 4);
    wc.name = "Static";
    wc.unicode = true;
    wc.atom = 0xc000 + uint32_t(classes().size());
    classes()["static"] = wc;
    classes()["#atom" + std::to_string(wc.atom)] = wc;
}
} // namespace user32
