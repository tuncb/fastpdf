#pragma once
#include "render.h"
#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>

namespace ui {
// All objects belong to the GUI thread. The PDF raster remains immutable.
struct PageView {
    ID3D11Device* device = nullptr;
    IDXGIDevice* dxgi = nullptr;
    ID2D1Factory1* factory = nullptr;
    ID2D1Device* d2d = nullptr;
    ID2D1DeviceContext* upload = nullptr;
    IDCompositionDesktopDevice* composition = nullptr;
    IDCompositionTarget* target = nullptr;
    IDCompositionVisual2* visual = nullptr;
    IDCompositionSurface* surface = nullptr;
    ID2D1Bitmap1* bitmap = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::shared_ptr<const pdf::RenderedPage> source;
    SIZE capacity{};
    RECT bounds{};
    bool initialized = false, visible = false;
    ULONGLONG retry_after = 0;
    HRESULT error = S_OK;
};
struct PageMark {
    RECT bounds{};
    uint32_t color = 0; // Premultiplied BGRA, as used by the GDI selection painter.
};
struct PageFrame {
    RECT viewport{}, page{}; // Physical pixels in the parent window.
    POINT shadow{};
    COLORREF background = 0, shadow_color = 0;
    std::span<const PageMark> marks;
};
void close_page_view(PageView& view);
void hide_page_view(PageView& view);
// Draw into an already active context from the same D2D device. Coordinates are
// relative to the viewport; preserve the caller's surface-offset transform.
void draw_page_view(PageView& view, ID2D1DeviceContext* draw, const PageFrame& frame);
bool present_page_view(PageView& view, HWND window, const std::shared_ptr<const pdf::RenderedPage>& source,
                       const PageFrame& frame);
} // namespace ui
