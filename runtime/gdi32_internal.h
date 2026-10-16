// Shared canvas state. Access only under the guest scheduler baton. Guest DIB
// addresses stay in the arena; window surfaces own top-down ARGB host pixels.
#pragma once
#include "gdi_image.h"
#include "imports.h"
#include <array>
#include <map>
#include <string>
namespace gdi {
struct Surface {
    int w = 0, h = 0;
    bool dirty = false;
    std::vector<uint32_t> argb;
};
struct Rect {
    int32_t l = 0, t = 0, r = 0, b = 0;
};
struct DcState {
    uint32_t surface = 0;
    int32_t org_x = 0, org_y = 0, viewport_x = 0, viewport_y = 0;
    int32_t brush_x = 0, brush_y = 0;
    uint32_t pen = 0x4f107, brush = 0x4f100, font = 0x4f10d;
    uint32_t bitmap = 0, palette = 0, region = 0;
    uint32_t text_color = 0, bk_color = 0xffffff;
    int bk_mode = 2, rop2 = 13, stretch_mode = 1, map_mode = 1;
    int32_t pos_x = 0, pos_y = 0;
    bool clipped = false;
    std::vector<Rect> clip;
};
struct DeviceContext : DcState {
    bool memory = true;
    uint32_t window = 0; // The acquiring window, possibly a child of surface.
    // WS_CLIPCHILDREN: the visible children's areas, in surface coordinates,
    // taken when the DC is made. Writes there are dropped.
    std::vector<Rect> excluded;
    std::vector<DcState> saved;
};
struct Object {
    enum Kind { Brush, Pen, Font, Region } kind = Brush;
    uint32_t color = 0, style = 0;
    int32_t width = 1;
    std::array<uint8_t, 92> logfont{};
    Rect rect{};
};
// A DC prepared for many pixel reads and writes. Resolving a DC's storage
// (borrowed DIB or window surface), the origin delta and the client bounds is
// the same for every pixel of a blit, so it is done once here instead of per
// pixel - the per-pixel walk of the window hierarchy was what made a software
// blit cost as much as the whole frame. `dib` stays opaque: it points at
// gdi32.cpp's guest DIB storage. Valid only while no window or DC is created
// or moved, which holds for the duration of a blit under the baton.
struct PixelTarget {
    DeviceContext *dc = nullptr;
    void *dib = nullptr; // guest DIB storage, or null when the DC names a window
    Surface *surface = nullptr;
    int64_t w = 0, h = 0;
    int64_t off_x = 0, off_y = 0; // added to a DC coordinate to reach storage
    Rect bounds{};                // client bounds in DC coordinates (no DIB)
};
bool prepare_target(uint32_t dc, PixelTarget *target);
bool read_target(const PixelTarget &target, int64_t x, int64_t y, uint32_t *pixel,
                 bool preserve_alpha = false);
bool write_target(const PixelTarget &target, int64_t x, int64_t y, uint32_t pixel,
                  bool blend = false);

std::map<uint32_t, DeviceContext> &dcs();
DeviceContext *dc_of(uint32_t dc);
std::map<uint32_t, Object> &objects();
uint32_t make_object(const Object &object);
uint32_t colorref(uint32_t argb); // Swapping R and B is its own inverse.
uint32_t argb(uint32_t color);
bool brush_color(uint32_t brush, uint32_t *pixel);
bool dc_size(uint32_t dc, int *w, int *h);
// Preserve alpha only for 32-bit BI_RGB DIB storage; normal GDI reads are opaque.
bool dc_has_alpha(uint32_t dc);
// A 1-bit DIB is selected into this DC, so blits to or from it convert between
// colour and monochrome rather than matching the nearest palette entry.
bool dc_is_monochrome(uint32_t dc);
bool read_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t *pixel, bool preserve_alpha = false);
// MaskBlt's mask: true with *set written when `bitmap` is a 1-bit bitmap and
// (x, y) is inside it. A set bit selects the foreground raster operation.
bool mask_bit(uint32_t bitmap, int64_t x, int64_t y, bool *set);
bool write_pixel(uint32_t dc, int64_t x, int64_t y, uint32_t pixel, bool blend = false);
Rect clip_box(uint32_t dc);
bool drawable(uint32_t dc, int64_t x, int64_t y);
void fill(uint32_t dc, Rect r, uint32_t pixel);
void line(uint32_t dc, int32_t x, int32_t y, int32_t x1, int32_t y1);
void register_draw();
void register_text();
// The dialog base units of a DS_SETFONT face: the font's average character
// width (a dialog unit is a quarter of it across) and cell height (an eighth of
// it down), measured from the same font the runtime draws with. False when the
// face has no measurable font, so the caller keeps the 8x16 bitmap cell.
bool font_dialog_units(const std::string &face, int32_t point_size, int32_t weight,
                       int32_t *ave_width, int32_t *cell_height);
uint32_t draw_text(uint32_t dc, uint32_t text, uint32_t count, uint32_t rect, uint32_t flags);
} // namespace gdi
uint32_t gdi_window_dc(uint32_t window);
bool gdi_release_window_dc(uint32_t window, uint32_t dc);
void gdi_destroy_window(uint32_t window);
// Borrowed DirectDraw memory is never freed by GDI. The owning surface pairs
// bind/unbind around its DC lifetime and publishes changes on ReleaseDC.
void gdi_bind_surface_dc(uint32_t dc, int w, int h, int bpp, uint32_t pitch, uint32_t bits,
                         const uint32_t *palette);
void gdi_unbind_surface_dc(uint32_t dc);
namespace gdi {
Rect to_device(uint32_t dc, Rect rect);
}

uint32_t gdi_new_dc();

// Called only with the guest baton. A present owns the composed pixel copy.
void gdi_present_windows(bool refresh = false);
void gdi_composite_windows(uint32_t *argb, int w, int h);
