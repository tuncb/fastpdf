#include "page_view.h"

namespace ui {
using pdf::release;

void hide_page_view(PageView& view) {
    if (view.visible && view.target) {
        view.target->SetRoot(nullptr);
        view.composition->Commit();
    }
    view.visible = false;
    view.source.reset();
    release(view.bitmap);
}
void close_page_view(PageView& view) {
    hide_page_view(view);
    release(view.brush);
    release(view.surface);
    release(view.visual);
    release(view.target);
    release(view.composition);
    release(view.upload);
    release(view.d2d);
    release(view.factory);
    release(view.dxgi);
    release(view.device);
    view = {};
}
static void checked(PageView& view, HRESULT result) {
    if (FAILED(result)) {
        view.error = result;
        pdf::hr(result, "Present GPU page");
    }
}
static void initialize(PageView& view, HWND window) {
    checked(view,
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                              nullptr, 0, D3D11_SDK_VERSION, &view.device, nullptr, nullptr));
    checked(view, view.device->QueryInterface(IID_PPV_ARGS(&view.dxgi)));
    checked(view, D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                    reinterpret_cast<void**>(&view.factory)));
    checked(view, view.factory->CreateDevice(view.dxgi, &view.d2d));
    checked(view, view.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &view.upload));
    checked(view, view.upload->CreateSolidColorBrush(D2D1::ColorF(0, 0), &view.brush));
    checked(view, DCompositionCreateDevice2(view.d2d, __uuidof(IDCompositionDesktopDevice),
                                            reinterpret_cast<void**>(&view.composition)));
    // Keep native controls above the document. Input continues to go to the
    // parent window; the composition visual does not add a child HWND.
    checked(view, view.composition->CreateTargetForHwnd(window, FALSE, &view.target));
    checked(view, view.composition->CreateVisual(&view.visual));
}
static void upload_page(PageView& view, const std::shared_ptr<const pdf::RenderedPage>& source) {
    if (view.source == source)
        return;
    const auto limit = view.upload->GetMaximumBitmapSize();
    if (!source->width || !source->height || source->width > limit || source->height > limit ||
        size_t(source->width) * source->height * 4 != source->pixels.size())
        checked(view, E_INVALIDARG);
    release(view.bitmap);
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96,
        96);
    checked(view, view.upload->CreateBitmap(D2D1::SizeU(source->width, source->height), source->pixels.data(),
                                            source->width * 4, &properties, &view.bitmap));
    view.source = source;
}
static D2D1_COLOR_F color(COLORREF value) {
    return D2D1::ColorF(GetRValue(value) / 255.0f, GetGValue(value) / 255.0f, GetBValue(value) / 255.0f);
}
static D2D1_RECT_F relative(RECT rect, const RECT& viewport) {
    return D2D1::RectF(float(rect.left - viewport.left), float(rect.top - viewport.top),
                       float(rect.right - viewport.left), float(rect.bottom - viewport.top));
}
void draw_page_view(PageView& view, ID2D1DeviceContext* draw, const PageFrame& frame) {
    const auto& vp = frame.viewport;
    draw->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    draw->PushAxisAlignedClip(D2D1::RectF(0, 0, float(vp.right - vp.left), float(vp.bottom - vp.top)),
                              D2D1_ANTIALIAS_MODE_ALIASED);
    draw->Clear(color(frame.background));
    RECT shadow = frame.page;
    OffsetRect(&shadow, frame.shadow.x, frame.shadow.y);
    view.brush->SetColor(color(frame.shadow_color));
    draw->FillRectangle(relative(shadow, vp), view.brush);
    const auto page = relative(frame.page, vp);
    view.brush->SetColor(D2D1::ColorF(D2D1::ColorF::White));
    draw->FillRectangle(page, view.brush);
    const bool exact = frame.page.right - frame.page.left == LONG(view.source->width) &&
                       frame.page.bottom - frame.page.top == LONG(view.source->height);
    // Every resampled preview uses the high-quality kernel, during and after resize.
    draw->DrawBitmap(view.bitmap, &page, 1,
                     exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                           : D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                     nullptr, nullptr);
    for (const auto& mark : frame.marks) {
        const auto alpha = mark.color >> 24;
        if (!alpha || IsRectEmpty(&mark.bounds))
            continue;
        // Brushes take straight color; the GDI overlay colors are premultiplied.
        view.brush->SetColor(D2D1::ColorF(float((mark.color >> 16) & 255) / float(alpha),
                                          float((mark.color >> 8) & 255) / float(alpha),
                                          float(mark.color & 255) / float(alpha), float(alpha) / 255.0f));
        draw->FillRectangle(relative(mark.bounds, vp), view.brush);
    }
    draw->PopAxisAlignedClip();
}
bool present_page_view(PageView& view, HWND window, const std::shared_ptr<const pdf::RenderedPage>& source,
                       const PageFrame& frame) {
    if (!source || IsRectEmpty(&frame.viewport)) {
        hide_page_view(view);
        return false;
    }
    if (GetTickCount64() < view.retry_after)
        return false;
    ID2D1DeviceContext* draw = nullptr;
    bool drawing = false;
    try {
        if (!view.device)
            initialize(view, window);
        upload_page(view, source);
        const LONG width = frame.viewport.right - frame.viewport.left;
        const LONG height = frame.viewport.bottom - frame.viewport.top;
        if (!view.surface || width > view.capacity.cx || height > view.capacity.cy) {
            const LONG capacity_w = std::max(view.capacity.cx, (width + 127) / 128 * 128);
            const LONG capacity_h = std::max(view.capacity.cy, (height + 127) / 128 * 128);
            release(view.surface);
            checked(view, view.composition->CreateSurface(UINT(capacity_w), UINT(capacity_h),
                                                          DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_IGNORE,
                                                          &view.surface));
            checked(view, view.visual->SetContent(view.surface));
            view.capacity = {capacity_w, capacity_h};
            view.initialized = false;
        }
        // Contents, viewport position and clip are committed together. Reuse
        // the surface on shrink and within each 128-pixel growth block.
        const RECT update{0, 0, view.initialized ? width : view.capacity.cx,
                          view.initialized ? height : view.capacity.cy};
        POINT offset{};
        checked(view, view.surface->BeginDraw(&update, __uuidof(ID2D1DeviceContext),
                                              reinterpret_cast<void**>(&draw), &offset));
        drawing = true;
        draw->SetDpi(96, 96);
        draw->SetTransform(D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y)));
        draw->PushAxisAlignedClip(D2D1::RectF(0, 0, float(update.right), float(update.bottom)),
                                  D2D1_ANTIALIAS_MODE_ALIASED);
        // The first update must initialize every pixel of the allocated surface.
        draw->Clear(color(frame.background));
        draw_page_view(view, draw, frame);
        draw->PopAxisAlignedClip();
        // DirectComposition owns BeginDraw/EndDraw on the returned D2D context.
        const auto result = view.surface->EndDraw();
        drawing = false;
        release(draw);
        checked(view, result);
        checked(view, view.visual->SetOffsetX(float(frame.viewport.left)));
        checked(view, view.visual->SetOffsetY(float(frame.viewport.top)));
        const auto clip = D2D1::RectF(0, 0, float(width), float(height));
        checked(view, view.visual->SetClip(clip));
        if (!view.visible)
            checked(view, view.target->SetRoot(view.visual));
        checked(view, view.composition->Commit());
        view.bounds = frame.viewport;
        view.initialized = view.visible = true;
        view.error = S_OK;
        return true;
    } catch (const std::exception& error) {
        if (drawing)
            view.surface->EndDraw();
        release(draw);
        const auto result = FAILED(view.error) ? view.error : E_FAIL;
        close_page_view(view);
        view.error = result;
        // Do not leave an old page visible or retry a lost device on every move.
        view.retry_after = GetTickCount64() + 5000;
        OutputDebugStringA(error.what());
        return false;
    }
}
} // namespace ui
