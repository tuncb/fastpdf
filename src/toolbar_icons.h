#pragma once

// Scalable outlines, also drawn into the supersampled UI surface for smooth small icons.
enum class ToolbarIcon {
    open,
    print,
    previous,
    next,
    minus,
    plus,
    fit,
    width,
    rotate,
    find,
    pages,
    bookmarks
};

static void draw_toolbar_icon(HDC dc, RECT rect, ToolbarIcon icon, COLORREF color, UINT dpi) {
    const int size = MulDiv(16, int(dpi), 96);
    const int left = (rect.left + rect.right - size) / 2;
    const int top = (rect.top + rect.bottom - size) / 2;
    auto point = [&](int x, int y) -> POINT {
        return {left + MulDiv(x, size, 16), top + MulDiv(y, size, 16)};
    };
    auto line = [&](std::initializer_list<POINT> points) {
        std::vector<POINT> scaled;
        for (auto p : points)
            scaled.push_back(point(p.x, p.y));
        Polyline(dc, scaled.data(), int(scaled.size()));
    };
    auto box = [&](int x, int y, int right, int bottom) {
        auto a = point(x, y), b = point(right, bottom);
        Rectangle(dc, a.x, a.y, b.x, b.y);
    };
    LOGBRUSH brush{BS_SOLID, color, 0};
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND,
                            DWORD(std::max(1, MulDiv(3, int(dpi), 192))), &brush, 0, nullptr);
    auto old_pen = SelectObject(dc, pen);
    auto old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (icon) {
    case ToolbarIcon::open:
        line({{1, 13}, {1, 3}, {6, 3}, {8, 5}, {14, 5}, {14, 7}});
        line({{1, 13}, {4, 7}, {15, 7}, {12, 13}, {1, 13}});
        break;
    case ToolbarIcon::print:
        box(4, 1, 12, 6);
        line({{4, 12}, {1, 12}, {1, 6}, {15, 6}, {15, 12}, {12, 12}});
        box(4, 10, 12, 15);
        line({{12, 8}, {13, 8}});
        break;
    case ToolbarIcon::previous:
        line({{10, 3}, {5, 8}, {10, 13}});
        break;
    case ToolbarIcon::next:
        line({{6, 3}, {11, 8}, {6, 13}});
        break;
    case ToolbarIcon::plus:
        line({{8, 3}, {8, 13}});
        [[fallthrough]];
    case ToolbarIcon::minus:
        line({{3, 8}, {13, 8}});
        break;
    case ToolbarIcon::fit:
        line({{5, 1}, {1, 1}, {1, 5}});
        line({{11, 1}, {15, 1}, {15, 5}});
        line({{1, 11}, {1, 15}, {5, 15}});
        line({{11, 15}, {15, 15}, {15, 11}});
        box(5, 4, 11, 12);
        break;
    case ToolbarIcon::width:
        line({{4, 5}, {1, 8}, {4, 11}});
        line({{12, 5}, {15, 8}, {12, 11}});
        line({{1, 8}, {15, 8}});
        break;
    case ToolbarIcon::rotate: {
        auto a = point(2, 2), b = point(14, 14), start = point(13, 5), end = point(14, 10);
        Arc(dc, a.x, a.y, b.x, b.y, start.x, start.y, end.x, end.y);
        line({{10, 5}, {14, 5}, {14, 1}});
        break;
    }
    case ToolbarIcon::find: {
        auto a = point(1, 1), b = point(12, 12);
        Ellipse(dc, a.x, a.y, b.x, b.y);
        line({{10, 10}, {15, 15}});
        break;
    }
    case ToolbarIcon::pages:
        box(1, 1, 15, 15);
        line({{6, 1}, {6, 14}});
        break;
    case ToolbarIcon::bookmarks:
        line({{4, 2}, {12, 2}, {12, 14}, {8, 11}, {4, 14}, {4, 2}});
        break;
    }
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
}
