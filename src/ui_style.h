#pragma once
#include "toolbar_icons.h"
#include <optional>

namespace ui {
inline constexpr COLORREF chrome = RGB(17, 17, 17);
inline constexpr COLORREF canvas = RGB(23, 23, 23);
inline constexpr COLORREF surface = RGB(28, 28, 28);
inline constexpr COLORREF hover = RGB(36, 36, 36);
inline constexpr COLORREF selected = RGB(41, 41, 41);
inline constexpr COLORREF border = RGB(48, 48, 48);
inline constexpr COLORREF text = RGB(237, 237, 237);
inline constexpr COLORREF muted = RGB(163, 163, 163);
inline constexpr COLORREF disabled = RGB(100, 100, 100);
inline constexpr COLORREF focus = RGB(52, 107, 241);
inline constexpr COLORREF shadow = RGB(12, 12, 12);
inline constexpr int radius = 6, sidebar_width = 160, sidebar_header = 32, row_height = 32;

// One reusable scratch surface for small UI shapes. Text stays at native resolution for ClearType.
struct Painter {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    int width = 0, height = 0;
};
inline void close(Painter& painter) {
    if (painter.dc) {
        SelectObject(painter.dc, painter.original);
        DeleteObject(painter.bitmap);
        DeleteDC(painter.dc);
    }
    painter = {};
}
inline bool prepare(Painter& painter, HDC destination, int width, int height) {
    if (painter.dc && width <= painter.width && height <= painter.height)
        return true;
    width = std::max(width, painter.width);
    height = std::max(height, painter.height);
    HDC dc = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, width, height);
    if (!dc || !bitmap) {
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        return false;
    }
    close(painter);
    painter = {dc, bitmap, SelectObject(dc, bitmap), width, height};
    return true;
}
inline void control(Painter& painter, HDC destination, RECT rect, UINT dpi, COLORREF background,
                    COLORREF fill, COLORREF outline, int outline_width = 0,
                    std::optional<ToolbarIcon> icon = {}, COLORREF icon_color = text) {
    int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;
    int sampling = prepare(painter, destination, width * 4, height * 4) ? 4 : 1;
    HDC dc = sampling == 4 ? painter.dc : destination;
    int saved = SaveDC(dc);
    if (sampling == 1)
        OffsetViewportOrgEx(dc, rect.left, rect.top, nullptr);
    RECT bounds{0, 0, width * sampling, height * sampling};
    SetDCBrushColor(dc, background);
    FillRect(dc, &bounds, HBRUSH(GetStockObject(DC_BRUSH)));
    int stroke = outline_width ? std::max(1, MulDiv(outline_width * sampling, int(dpi), 96)) : 0;
    HPEN pen = stroke ? CreatePen(PS_SOLID, stroke, outline) : nullptr;
    SelectObject(dc, pen ? pen : GetStockObject(NULL_PEN));
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, fill);
    int inset = (stroke + 1) / 2;
    int diameter = MulDiv(radius * 2 * sampling, int(dpi), 96);
    RoundRect(dc, inset, inset, bounds.right - inset, bounds.bottom - inset, diameter, diameter);
    if (icon)
        draw_toolbar_icon(dc, bounds, *icon, icon_color, dpi * UINT(sampling));
    RestoreDC(dc, saved);
    if (pen)
        DeleteObject(pen);
    if (sampling == 4) {
        saved = SaveDC(destination);
        SetStretchBltMode(destination, HALFTONE);
        SetBrushOrgEx(destination, 0, 0, nullptr);
        StretchBlt(destination, rect.left, rect.top, width, height, dc, 0, 0, bounds.right, bounds.bottom,
                   SRCCOPY);
        RestoreDC(destination, saved);
    }
}
} // namespace ui
