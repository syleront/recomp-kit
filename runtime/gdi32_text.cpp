// Text for guest canvases: a TrueType face the program registered when its font
// names one, the bundled stand-in when it names a Windows interface face, else
// fixed 8x16 bitmap cells. No host font services or guest
// pointers escape this file; callbacks receive temporary guest-heap records.
#include "gdi32_internal.h"
#include <cstdlib>
#include "../platform/os.h"
#include "gdi32_font8x16.h"
#include "gdi32_truetype.h"
#include "memory.h"
#include "win32.h"
#include <algorithm>
#include <cstring>
#include <climits>
using namespace gdi;
namespace {
uint32_t font_word(const Object &font, int offset) {
    uint32_t v;
    memcpy(&v, font.logfont.data() + offset, 4);
    return v;
}
int32_t font_scale(uint32_t dc) {
    auto *d = dc_of(dc);
    auto it = objects().find(d ? d->font : 0);
    int64_t height = it == objects().end() ? 16 : int32_t(font_word(it->second, 0));
    return int32_t(std::max<int64_t>(1, std::abs(height) / 16));
}
void create_font(X86 *c) {
    uint32_t p = arg(c, 0);
    if (!p || !gm_valid(p, 92)) {
        set_eax(c, 0);
        return;
    }
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 92);
    set_eax(c, make_object(font));
}
// CreateFontIndirectA receives a 60-byte LOGFONTA. The fourteen scalar fields
// before lfFaceName are identical to LOGFONTW, only the 32-byte face name
// differs; widening it gives the same 92-byte object the W form stores.
uint32_t font_from_logfont_a(uint32_t p) {
    Object font;
    font.kind = Object::Font;
    memcpy(font.logfont.data(), g_mem + p, 28);
    size_t n = 0;
    while (n < 31 && rd8(p + 28 + n))
        ++n;
    for (size_t i = 0; i < n; ++i)
        font.logfont[28 + 2 * i] = rd8(p + 28 + i);
    return make_object(font);
}
void create_font_a(X86 *c) {
    uint32_t p = arg(c, 0);
    set_eax(c, p && gm_valid(p, 60) ? font_from_logfont_a(p) : 0);
}
// CreateFontA passes the LOGFONTA fields as fourteen scalar arguments instead
// of a struct. Build the same 60-byte LOGFONTA and share the widening above,
// so a program that names a font directly gets the same object.
void create_font_named(X86 *c) {
    uint32_t buf = heap_alloc(60, true);
    if (!buf) {
        set_eax(c, 0);
        return;
    }
    for (int i = 0; i < 5; ++i)
        wr32(buf + 4 * i, arg(c, i)); // cHeight..cWeight
    for (int i = 5; i < 13; ++i)
        wr8(buf + 20 + (i - 5), uint8_t(arg(c, i))); // italic..pitchAndFamily
    std::string face = gm_str(arg(c, 13));
    size_t n = 0;
    while (n < 31 && n < face.size()) {
        wr8(buf + 28 + n, uint8_t(face[n]));
        ++n;
    }
    uint32_t handle = font_from_logfont_a(buf);
    heap_free(buf);
    // Which face a program asks for, and at what cell, is the first thing a
    // stretched or wrong-sized caption needs: one line per distinct request.
    LOGV("CreateFontA face=\"%s\" height=%d width=%d weight=%d escapement=%d italic=%d",
         face.c_str(), int32_t(arg(c, 0)), int32_t(arg(c, 1)), int32_t(arg(c, 4)),
         int32_t(arg(c, 2)), int32_t(arg(c, 5)));
    set_eax(c, handle);
}
// TEXTMETRICW is 60 bytes on x86: eleven DWORDs, four UTF-16
// characters, five BYTE fields and three bytes of trailing padding.
void metrics(uint32_t out, int32_t scale, uint32_t weight) {
    memset(g_mem + out, 0, 60);
    wr32(out, 16u * scale);
    wr32(out + 4, 13u * scale);
    wr32(out + 8, 3u * scale);
    wr32(out + 20, 8u * scale);
    wr32(out + 24, 8u * scale);
    wr32(out + 28, weight);
    wr32(out + 36, 96);
    wr32(out + 40, 96);
    wr16(out + 44, 32);
    wr16(out + 46, 127);
    wr16(out + 48, '?');
    wr16(out + 50, ' ');
    wr8(out + 55, 0x30);
    wr8(out + 56, 0);
}
// All wide text entry points share these bitmap glyphs and DC colour/clip rules.
void glyph(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline = false) {
    auto *dc = dc_of(hdc);
    if (ch < 32 || ch > 127)
        ch = '?';
    int64_t scale = font_scale(hdc), width = 8 * scale, height = 16 * scale;
    Rect clip = clip_box(hdc);
    uint32_t fg = argb(dc->text_color), bg = argb(dc->bk_color);
    for (int64_t yy = std::max<int64_t>(y, clip.t); yy < std::min<int64_t>(y + height, clip.b);
         ++yy)
        for (int64_t xx = std::max<int64_t>(x, clip.l); xx < std::min<int64_t>(x + width, clip.r);
             ++xx) {
            uint8_t row = recomp_font::font8x16[ch - 32][size_t((yy - y) / scale)];
            if ((row & (0x80 >> ((xx - x) / scale))) || (underline && yy - y >= 14 * scale))
                write_pixel(hdc, xx, yy, fg);
            else if (dc->bk_mode == 2)
                write_pixel(hdc, xx, yy, bg);
        }
}
// How one DC's text is measured and drawn: from the registered TrueType face
// its font names, or from the bitmap cells.
struct Pen {
    const TrueTypeFace *face = nullptr;
    double scale = 0;
    int64_t cells = 1;
    TrueTypeMetrics metrics;
    int64_t advance(uint32_t ch) const {
        return face ? truetype_advance(face, scale, ch) : 8 * cells;
    }
    int64_t height() const {
        return face ? metrics.height : 16 * cells;
    }
    void draw(uint32_t hdc, int64_t x, int64_t y, uint32_t ch, bool underline) const {
        if (!face) {
            glyph(hdc, x, y, ch, underline);
            return;
        }
        auto *dc = dc_of(hdc);
        if (!dc)
            return;
        const uint32_t fg = argb(dc->text_color) & 0xffffffu;
        const int64_t width = advance(ch);
        if (dc->bk_mode == 2)
            fill(hdc, {int32_t(x), int32_t(y), int32_t(x + width), int32_t(y + metrics.height)},
                 argb(dc->bk_color));
        const TrueTypeGlyph &g = truetype_glyph(face, scale, ch);
        const int64_t baseline = y + metrics.ascent;
        for (int32_t j = 0; j < g.h; ++j)
            for (int32_t i = 0; i < g.w; ++i)
                if (uint8_t cover = g.coverage[size_t(j) * size_t(g.w) + size_t(i)])
                    write_pixel(hdc, x + g.x + i, baseline + g.y + j, (uint32_t(cover) << 24) | fg,
                                true);
        if (underline)
            for (int64_t yy = baseline + 1;
                 yy <= baseline + std::max<int64_t>(1, metrics.height / 16); ++yy)
                for (int64_t xx = x; xx < x + width; ++xx)
                    write_pixel(hdc, xx, yy, 0xff000000u | fg);
    }
};
// Resolve one LOGFONT object's face: a face the program registered, else the
// bundled stand-in for a Windows interface face, else no TrueType face at all
// (the caller then uses the 8x16 bitmap cells). Shared by the DC query below
// and the dialog-font measurement below it.
Pen pen_from_font(const Object &font) {
    Pen pen;
    pen.cells = std::max<int64_t>(1, std::abs(int32_t(font_word(font, 0))) / 16);
    std::string family;
    for (size_t i = 0; i < 32; ++i) {
        uint16_t unit = uint16_t(font.logfont[28 + 2 * i] | (font.logfont[29 + 2 * i] << 8));
        if (!unit)
            break;
        family.push_back(unit < 128 ? char(unit) : '?');
    }
    if (family.empty())
        return pen;
    pen.face = truetype_find(family);
    if (!pen.face) {
        // A face every Windows has and the program never supplied: what it
        // gets with no font of its own set, from the VCL's default font.
        const int32_t weight = int32_t(font_word(font, 16));
        pen.face = truetype_windows_substitute(family, weight);
        if (pen.face)
            log_once(("gdi-font:" + family).c_str(),
                     "gdi: \"%s\" is a Windows font the program did not register; drawing it "
                     "with the bundled Open Sans",
                     family.c_str());
    }
    if (!pen.face) {
        // No TrueType stand-in for this face: the 8x16 cells are drawn at the
        // requested cell count, so a face asking for a height under 16 px comes
        // out wider and taller than Windows would draw it (TD-086). Say which
        // face once so a stretched caption can be traced to its LOGFONT.
        log_once(("gdi-cells:" + family).c_str(),
                 "gdi: no stand-in for face \"%s\"; drawing it with the 8x16 cells",
                 family.c_str());
        return pen;
    }
    pen.scale = truetype_scale(pen.face, int32_t(font_word(font, 0)));
    pen.metrics = truetype_metrics(pen.face, pen.scale);
    return pen;
}
Pen pen_of(uint32_t hdc) {
    auto *dc = dc_of(hdc);
    auto it = objects().find(dc ? dc->font : 0);
    if (it == objects().end() || it->second.kind != Object::Font) {
        Pen pen;
        pen.cells = font_scale(hdc);
        return pen;
    }
    return pen_from_font(it->second);
}
void get_metrics(X86 *c) {
    auto *dc = dc_of(arg(c, 0));
    uint32_t out = arg(c, 1);
    if (!dc || !out || !gm_valid(out, 60)) {
        set_eax(c, 0);
        return;
    }
    auto font = objects().find(dc->font);
    uint32_t weight = font == objects().end() ? 400 : font_word(font->second, 16);
    if (!weight)
        weight = 400;
    metrics(out, font_scale(arg(c, 0)), weight);
    const Pen pen = pen_of(arg(c, 0));
    if (pen.face) {
        wr32(out, uint32_t(pen.metrics.height));
        wr32(out + 4, uint32_t(pen.metrics.ascent));
        wr32(out + 8, uint32_t(pen.metrics.descent));
        wr32(out + 12, uint32_t(pen.metrics.internal));
        wr32(out + 20, uint32_t(pen.metrics.average));
        wr32(out + 24, uint32_t(pen.metrics.maximum));
    }
    set_eax(c, 1);
}
bool extent_units(uint32_t dc, uint32_t text, uint32_t n, uint32_t out) {
    if (!dc_of(dc) || !out || !gm_valid(out, 8) || n > GUEST_SIZE / 2 ||
        (n && (!text || !gm_valid(text, n * 2))))
        return false;
    const Pen pen = pen_of(dc);
    uint64_t width = 0;
    for (uint32_t i = 0; i < n; ++i)
        width += uint64_t(pen.advance(rd16(text + 2 * i)));
    if (width > INT_MAX)
        return false;
    wr32(out, uint32_t(width));
    wr32(out + 4, uint32_t(pen.height()));
    return true;
}
void extent(X86 *c) {
    set_eax(c, extent_units(arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)));
}
// GetTextExtentPoint32A widens the byte string into a temporary guest buffer so
// the one measurement path (with its TrueType/bitmap font choice) serves both.
void extent_a(X86 *c) {
    uint32_t dc = arg(c, 0), text = arg(c, 1), n = arg(c, 2), out = arg(c, 3);
    if (!dc_of(dc) || !out || !gm_valid(out, 8)) {
        set_eax(c, 0);
        return;
    }
    std::string s = n == 0xffffffffu ? gm_str(text) : gm_str(text, n);
    uint32_t cap = uint32_t(s.size()) + 1;
    uint32_t buf = heap_alloc(cap * 2, true);
    if (!buf) {
        set_eax(c, 0);
        return;
    }
    uint32_t units = gm_put_wstr(buf, s, cap);
    bool ok = extent_units(dc, buf, units, out);
    heap_free(buf);
    set_eax(c, ok);
}
void text_out(X86 *c) {
    uint32_t hdc = arg(c, 0), flags = arg(c, 3), rp = arg(c, 4), text = arg(c, 5), n = arg(c, 6),
             dx = arg(c, 7);
    auto *dc = dc_of(hdc);
    int w, h;
    if (!dc || !dc_size(hdc, &w, &h) || n > GUEST_SIZE / 4 ||
        (n && (!text || !gm_valid(text, n * 2))) || (dx && !gm_valid(dx, n * 4)) ||
        ((flags & 6) && (!rp || !gm_valid(rp, 16)))) {
        set_eax(c, 0);
        return;
    }
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    Rect rect{};
    if (flags & 6)
        rect = {int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
                int32_t(rd32(rp + 12))};
    if (flags & 4) {
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    uint32_t bg = argb(dc->bk_color);
    if (flags & 2)
        fill(hdc, rect, bg);
    const Pen pen = pen_of(hdc);
    int64_t x = int32_t(arg(c, 1)), y = int32_t(arg(c, 2));
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t unit = rd16(text + 2 * i);
        pen.draw(hdc, x, y, unit, false);
        x += dx ? int32_t(rd32(dx + 4 * i)) : pen.advance(unit);
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    set_eax(c, 1);
}
// The fonts are kept, so a DC whose font names one of them draws with it. Data
// the rasterizer cannot read still gets a handle, as it always has here.
void memory_font(X86 *c) {
    uint32_t data = arg(c, 0), size = arg(c, 1), count = arg(c, 3);
    if (count && !gm_valid(count, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t fonts =
        data && size && gm_valid(data, size) ? truetype_add_memory(g_mem + data, size) : 0;
    Object font;
    font.kind = Object::Font;
    uint32_t handle = make_object(font);
    if (count)
        wr32(count, std::max<uint32_t>(fonts, 1));
    set_eax(c, handle);
}
void enumerate(X86 *c) {
    uint32_t cb = arg(c, 2), param = arg(c, 3);
    if (!dc_of(arg(c, 0)) || !cb) {
        set_eax(c, 0);
        return;
    }
    uint32_t data = heap_alloc(92 + 60, true);
    if (!data) {
        set_eax(c, 0);
        return;
    }
    wr32(data, 16);
    wr32(data + 4, 8);
    wr32(data + 16, 400);
    wr8(data + 27, 0x30);
    gm_put_wstr(data + 28, "recomp", 32);
    metrics(data + 92, 1, 400);
    uint32_t result = guest_call(c, cb, data, data + 92, 1, param);
    heap_free(data);
    set_eax(c, result);
}
void unsupported(X86 *c) {
    set_eax(c, 0);
}
} // namespace
namespace gdi {
// The advance width and line height of an ANSI string in a DC's selected font:
// the same measurement DrawText/ExtTextOut lay text out with, used for an edit
// control's caret x and selection extent. One byte per character is what an
// edit's own text is; a non-ASCII byte is a single glyph in the ANSI code page,
// which is what the raster/TrueType path draws for it too.
int32_t text_width(uint32_t dc, const std::string &text) {
    if (!dc_of(dc))
        return 0;
    const Pen pen = pen_of(dc);
    int64_t width = 0;
    for (unsigned char c : text)
        width += pen.advance(c);
    return int32_t(width > INT_MAX ? INT_MAX : width);
}
int32_t text_height(uint32_t dc) {
    if (!dc_of(dc))
        return 0;
    return int32_t(pen_of(dc).height());
}
// The dialog base units of a DS_SETFONT face. A dialog unit (DLU) is a quarter
// of the font's average character width horizontally and an eighth of its cell
// height vertically, so these two numbers are all a template needs to lay
// itself out. Windows builds the dialog font from the template's point size at
// the screen DPI (96 here: a negative lfHeight is the em in pixels) and reads
// its TEXTMETRIC; the runtime measures the same font it will draw the dialog
// with. False when the face has no measurable font (a raster face such as
// System), so the caller keeps the 8x16 bitmap cell.
Object dialog_font_object(const std::string &face, int32_t point_size, int32_t weight) {
    Object font;
    font.kind = Object::Font;
    int32_t height = -int32_t((int64_t(point_size) * 96 + 36) / 72); // MulDiv(pt, 96, 72)
    memcpy(font.logfont.data(), &height, 4);
    int32_t w = weight > 0 ? weight : 400;
    memcpy(font.logfont.data() + 16, &w, 4);
    for (size_t i = 0; i < 31 && i < face.size(); ++i)
        font.logfont[28 + 2 * i] = uint8_t(face[i]);
    return font;
}
bool font_dialog_units(const std::string &face, int32_t point_size, int32_t weight,
                       int32_t *ave_width, int32_t *cell_height) {
    if (face.empty() || point_size <= 0)
        return false;
    Pen pen = pen_from_font(dialog_font_object(face, point_size, weight));
    if (!pen.face || pen.metrics.average <= 0 || pen.metrics.height <= 0)
        return false;
    *ave_width = int32_t(pen.metrics.average);
    *cell_height = int32_t(pen.metrics.height);
    return true;
}
// DrawText's basic VCL layout uses the same fixed font as ExtTextOutW.
// Measurement must use that font too; CALCRECT never mutates canvas pixels.
uint32_t draw_text(uint32_t hdc, uint32_t text, uint32_t count, uint32_t rp, uint32_t flags) {
    auto *dc = dc_of(hdc);
    if (recomp_env("TRACE_GDI")) {
        int dw = -1, dh = -1;
        dc_size(hdc, &dw, &dh);
        LOGW("gdi: DrawText hdc=%08x(%dx%d)%s flags=%08x \"%s\"", hdc, dw, dh,
             dc ? "" : " NO SUCH DC", flags, gm_wstr(text, 64).c_str());
    }
    if (!dc || !rp || !gm_valid(rp, 16) || !text)
        return 0;
    if (count == UINT32_MAX) {
        count = 0;
        while (uint64_t(text) + 2ull * count + 2 <= GUEST_SIZE && gm_valid(text + count * 2, 2) &&
               rd16(text + count * 2))
            ++count;
    }
    if (!count || count > GUEST_SIZE / 2 || !gm_valid(text, count * 2))
        return 0;
    Rect rect{int32_t(rd32(rp)), int32_t(rd32(rp + 4)), int32_t(rd32(rp + 8)),
              int32_t(rd32(rp + 12))};
    const Pen pen = pen_of(hdc);
    const int64_t ch = pen.height();
    struct Cell {
        uint16_t ch;
        bool underline;
    };
    std::vector<std::vector<Cell>> lines(1);
    auto width_of = [&](const std::vector<Cell> &line) {
        int64_t width = 0;
        for (Cell cell : line)
            width += pen.advance(cell.ch);
        return width;
    };
    bool prefix = false;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t unit = rd16(text + 2 * i);
        if (!(flags & 0x800) && unit == '&') { // DT_NOPREFIX
            if (i + 1 < count && rd16(text + 2 * (i + 1)) == '&')
                ++i;
            else {
                prefix = true;
                continue;
            }
        }
        if (unit == '\r' || unit == '\n') {
            if (unit == '\r' && i + 1 < count && rd16(text + 2 * (i + 1)) == '\n')
                ++i;
            if (!(flags & 0x20)) {
                lines.emplace_back();
                prefix = false;
                continue;
            }
            unit = ' ';
        }
        if (unit == '\t' && (flags & 0x40)) {
            size_t spaces = 8 - lines.back().size() % 8;
            lines.back().insert(lines.back().end(), spaces, Cell{' ', false});
        } else
            lines.back().push_back({unit, prefix && !(flags & 0x100000)});
        prefix = false;
    }
    if ((flags & 0x10) && !(flags & 0x20) && rect.r > rect.l) { // DT_WORDBREAK
        const int64_t limit = int64_t(rect.r) - rect.l;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto &line = lines[i];
            size_t cols = 0; // how many characters fit the width
            for (int64_t used = 0; cols < line.size(); ++cols) {
                used += pen.advance(line[cols].ch);
                if (used > limit)
                    break;
            }
            if (!cols || cols >= line.size())
                continue;
            size_t split = std::min(cols, line.size() - 1);
            while (split && line[split].ch != ' ')
                --split;
            if (!split) { // A long word is never split in the middle.
                split = cols;
                while (split < line.size() && line[split].ch != ' ')
                    ++split;
            }
            if (split < line.size()) {
                std::vector<Cell> tail(line.begin() + split + 1, line.end());
                line.resize(split);
                lines.insert(lines.begin() + i + 1, std::move(tail));
            }
        }
    }
    int64_t height = ch * lines.size(), maxwidth = 0;
    for (const auto &line : lines)
        maxwidth = std::max(maxwidth, width_of(line));
    if (height > INT_MAX || maxwidth > INT_MAX)
        return 0;
    if (flags & 0x400) {
        wr32(rp + 8, uint32_t(int64_t(rect.l) + maxwidth));
        wr32(rp + 12, uint32_t(int64_t(rect.t) + height));
        return uint32_t(height);
    }
    int w, h;
    if (!dc_size(hdc, &w, &h))
        return 0;
    bool old_clipped = dc->clipped;
    std::vector<Rect> old_clip = dc->clip;
    if (!(flags & 0x100)) { // DT_NOCLIP
        Rect cut = to_device(hdc, rect);
        std::vector<Rect> source = dc->clipped ? dc->clip : std::vector<Rect>{{0, 0, w, h}};
        dc->clip.clear();
        dc->clipped = true;
        for (Rect r : source) {
            Rect hit{std::max(r.l, cut.l), std::max(r.t, cut.t), std::min(r.r, cut.r),
                     std::min(r.b, cut.b)};
            if (hit.l < hit.r && hit.t < hit.b)
                dc->clip.push_back(hit);
        }
    }
    int64_t y = rect.t;
    if (flags & 0x20) {
        if (flags & 4)
            y += (int64_t(rect.b) - rect.t - height) / 2;
        else if (flags & 8)
            y = int64_t(rect.b) - height;
    }
    for (const auto &line : lines) {
        int64_t x = rect.l, width = width_of(line);
        if (flags & 1)
            x += (int64_t(rect.r) - rect.l - width) / 2;
        else if (flags & 2)
            x = int64_t(rect.r) - width;
        for (Cell cell : line) {
            pen.draw(hdc, x, y, cell.ch, cell.underline);
            x += pen.advance(cell.ch);
        }
        y += ch;
    }
    dc->clip = std::move(old_clip);
    dc->clipped = old_clipped;
    return uint32_t(y - rect.t);
}
void register_text() {
#define G(n, a, f)                                                                                 \
    {                                                                                              \
        "GDI32.dll", n, a, f                                                                       \
    }
    static const ImportShim shims[] = {G("CreateFontIndirectW", 1, create_font),
                                       G("CreateFontIndirectA", 1, create_font_a),
                                       G("CreateFontA", 14, create_font_named),
                                       G("ExtTextOutW", 8, text_out),
                                       G("GetTextExtentPoint32W", 4, extent),
                                       G("GetTextExtentPoint32A", 4, extent_a),
                                       G("GetTextExtentPointW", 4, extent),
                                       G("GetTextMetricsW", 2, get_metrics),
                                       G("AddFontMemResourceEx", 4, memory_font),
                                       G("EnumFontsW", 4, enumerate),
                                       G("EnumFontFamiliesExW", 5, enumerate),
                                       G("EnumFontFamiliesExA", 5, enumerate),
                                       G("CreateEnhMetaFileW", 4, unsupported),
                                       G("CreateEnhMetaFileA", 4, unsupported),
                                       G("CloseEnhMetaFile", 1, unsupported),
                                       G("DeleteEnhMetaFile", 1, unsupported),
                                       G("GetEnhMetaFileW", 1, unsupported),
                                       G("GetEnhMetaFileA", 1, unsupported),
                                       G("GetEnhMetaFileBits", 3, unsupported),
                                       G("GetEnhMetaFileHeader", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionW", 3, unsupported),
                                       G("GetEnhMetaFileDescriptionA", 3, unsupported),
                                       G("GetEnhMetaFilePaletteEntries", 3, unsupported),
                                       G("GetEnhMetaFilePixelFormat", 3, unsupported),
                                       G("SetEnhMetaFileBits", 2, unsupported),
                                       G("SetWinMetaFileBits", 4, unsupported),
                                       G("GetWinMetaFileBits", 5, unsupported),
                                       G("PlayEnhMetaFile", 3, unsupported),
                                       G("CopyEnhMetaFileW", 2, unsupported),
                                       G("CreateDCW", 4, unsupported),
                                       G("CreateICW", 4, unsupported),
                                       G("StartDocW", 2, unsupported),
                                       G("EndDoc", 1, unsupported),
                                       G("StartPage", 1, unsupported),
                                       G("EndPage", 1, unsupported),
                                       G("AbortDoc", 1, unsupported),
                                       G("SetAbortProc", 2, unsupported)};
#undef G
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
} // namespace gdi
