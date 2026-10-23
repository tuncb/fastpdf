// Exercise presentation in our own window. No desktop input is sent.
#include "../src/app.cpp"
#include <cstdio>

static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
static std::shared_ptr<RenderedPage> test_page() {
    auto page = std::make_shared<RenderedPage>();
    page->width = page->height = 64;
    page->pixels.resize(64 * 64 * 4, 255);
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) {
            const auto i = (y * 64 + x) * 4;
            const uint8_t value = ((x + y) & 1) ? 255 : 0;
            page->pixels[i] = page->pixels[i + 1] = page->pixels[i + 2] = value;
        }
    page->pixels[0] = 0;
    page->pixels[1] = 0;
    page->pixels[2] = 255;
    return page;
}
static std::vector<uint32_t> read_frame(ui::PageView& view, const ui::PageFrame& frame) {
    const UINT width = frame.viewport.right - frame.viewport.left;
    const UINT height = frame.viewport.bottom - frame.viewport.top;
    ID2D1Bitmap1 *target = nullptr, *readback = nullptr;
    try {
        auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        hr(view.upload->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &target),
           "Create test target");
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        hr(view.upload->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, &properties, &readback),
           "Create test readback");
        view.upload->SetTarget(target);
        view.upload->SetDpi(96, 96);
        view.upload->SetTransform(D2D1::Matrix3x2F::Identity());
        view.upload->BeginDraw();
        ui::draw_page_view(view, view.upload, frame);
        hr(view.upload->EndDraw(), "Draw test frame");
        view.upload->SetTarget(nullptr);
        hr(readback->CopyFromBitmap(nullptr, target, nullptr), "Copy test frame");
        D2D1_MAPPED_RECT mapped{};
        hr(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped), "Map test frame");
        std::vector<uint32_t> pixels(size_t(width) * height);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(pixels.data() + size_t(y) * width, mapped.bits + size_t(y) * mapped.pitch,
                        size_t(width) * 4);
        hr(readback->Unmap(), "Unmap test frame");
        release(readback);
        release(target);
        return pixels;
    } catch (...) {
        view.upload->SetTarget(nullptr);
        release(readback);
        release(target);
        throw;
    }
}
static void check_pixels(ui::PageView& view, HWND window, const std::shared_ptr<RenderedPage>& source) {
    ui::PageFrame frame{{100, 80, 196, 176}, {112, 90, 176, 154}, {3, 5}, RGB(32, 48, 64), RGB(8, 9, 10), {}};
    check(ui::present_page_view(view, window, source, frame), "Cannot present exact-size page");
    const auto exact = read_frame(view, frame);
    const auto original = reinterpret_cast<const uint32_t*>(source->pixels.data());
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            check((exact[(y + 10) * 96 + x + 12] & 0xffffff) == (original[y * 64 + x] & 0xffffff),
                  "GPU changed the page pixels at 1:1 scale");
    check((exact[0] & 0xffffff) == 0x203040, "GPU used the wrong theme background");
    check((exact[74 * 96 + 76] & 0xffffff) == 0x08090a, "Page shadow is not in its expected position");
    frame.page.right = frame.page.left + 31;
    frame.page.bottom = frame.page.top + 31;
    check(ui::present_page_view(view, window, source, frame), "Cannot present a scaled page");
    const auto scaled = read_frame(view, frame);
    int intermediate = 0;
    for (int y = 12; y < 39; ++y)
        for (int x = 14; x < 41; ++x) {
            const auto value = scaled[size_t(y) * 96 + x] & 255;
            if (value > 50 && value < 205)
                ++intermediate;
        }
    check(intermediate > 600, "Shrinking dropped pixels instead of filtering the page");
    // Scroll past the top and left of the viewport, then compare to the source.
    frame.page = {90, 70, 154, 134};
    check(ui::present_page_view(view, window, source, frame), "Cannot present a clipped page");
    const auto clipped = read_frame(view, frame);
    check((clipped[0] & 0xffffff) == (original[10 * 64 + 10] & 0xffffff),
          "Scrolled page does not align with the viewport clip");
}
static void check_overlays(App& app) {
    app.bitmap = std::make_shared<RenderedPage>();
    app.bitmap->width = app.bitmap->height = 64;
    app.bitmap->pixels.assign(64 * 64 * 4, 255);
    app.bitmap->text = {{L"PDF", {8, 8, 32, 20}, {}}, {L"PDF", {8, 28, 32, 40}, {}}};
    app.selected = {0};
    app.find_open = true;
    app.active_match = 0;
    append_search_results(app.matches, search_matches(search_text(*app.bitmap), L"PDF", {}));
    check(app.matches.count == 2, "Cannot prepare search overlay fixture");
    for (UINT dpi : {96u, 144u, 192u}) {
        app.dpi = dpi;
        app.zoom = 72.0 / dpi;
        app.fit = Fit::custom;
        app.bitmap_scale = 1;
        update_scale(app);
        for (bool dark : {false, true}) {
            app.search_colors.field = dark ? RGB(20, 21, 22) : RGB(240, 241, 242);
            app.search_colors.border = dark ? RGB(80, 81, 82) : RGB(170, 171, 172);
            app.search_colors.accent = dark ? RGB(80, 160, 240) : RGB(30, 90, 180);
            const auto marks = page_marks(app);
            check(marks.size() == 7, "Selection/search overlay geometry is incomplete");
            const auto vp = viewport(app);
            const ui::PageFrame frame{vp,
                                      page_rect(app),
                                      {px(app, 3), px(app, 5)},
                                      app.search_colors.field,
                                      app.search_colors.border,
                                      marks};
            check(ui::present_page_view(app.page_view, app.window, app.bitmap, frame),
                  "Cannot present selection/search overlays");
            const auto gpu = read_frame(app.page_view, frame);
            ui::Painter cpu;
            HDC dc = GetDC(app.window);
            const auto bounds = client(app);
            check(ui::prepare(cpu, dc, bounds.right, bounds.bottom), "Cannot create CPU comparison surface");
            paint_app(app, cpu.dc);
            GdiFlush();
            std::vector<uint32_t> data(size_t(cpu.width) * cpu.height);
            BITMAPINFO info{};
            info.bmiHeader = {sizeof(BITMAPINFOHEADER), cpu.width, -cpu.height, 1, 32, BI_RGB};
            SelectObject(cpu.dc, cpu.original);
            check(GetDIBits(cpu.dc, cpu.bitmap, 0, UINT(cpu.height), data.data(), &info, DIB_RGB_COLORS) != 0,
                  "Cannot read the CPU comparison surface");
            SelectObject(cpu.dc, cpu.bitmap);
            int max_difference = 0;
            for (LONG y = vp.top; y < vp.bottom; ++y)
                for (LONG x = vp.left; x < vp.right; ++x) {
                    const auto a = data[size_t(y) * cpu.width + x];
                    const auto b = gpu[size_t(y - vp.top) * (vp.right - vp.left) + x - vp.left];
                    for (unsigned shift : {0u, 8u, 16u})
                        max_difference = std::max(
                            max_difference, std::abs(int((a >> shift) & 255) - int((b >> shift) & 255)));
                }
            ui::close(cpu);
            ReleaseDC(app.window, dc);
            check(max_difference <= 1, "GPU overlay pixels differ from selection/search CPU painting");
        }
    }
    app.matches = {};
    app.find_open = false;
    app.selected.clear();
}
int main() {
    App app;
    HWND window = nullptr;
    try {
        hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Initialize presentation test");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ui::enable_system_dialog_theme();
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                                            ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                            ICC_LISTVIEW_CLASSES};
        check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize controls");
        app.instance = GetModuleHandleW(nullptr);
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc;
        type.hInstance = app.instance;
        type.lpszClassName = L"FastPDF.PageViewTest";
        check(RegisterClassW(&type) != 0, "Cannot register GPU test window");
        window = CreateWindowExW(WS_EX_NOACTIVATE, type.lpszClassName, L"GPU page presentation test",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 20, 20, 700, 600, nullptr, nullptr,
                                 app.instance, &app);
        check(window != nullptr, "Cannot create GPU test window");
        app.worker.stop = true;
        app.worker.ready.notify_all();
        app.worker.thread.join();
        app.pages = {{64, 64, 0}};
        auto source = test_page();
        const ui::PageFrame first{{100, 80, 196, 176}, {112, 90, 176, 154}, {3, 5}, 0, 0, {}};
        if (!ui::present_page_view(app.page_view, window, source, first)) {
            const auto error = app.page_view.error;
            if (error == DXGI_ERROR_UNSUPPORTED || error == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                DestroyWindow(window);
                CoUninitialize();
                std::printf("Hardware presentation unavailable: 0x%08lx\n", error);
                return 77;
            }
            hr(error, "Initialize hardware presentation");
        }
        check_pixels(app.page_view, window, source);
        check_overlays(app);
        app.dpi = 96;
        app.fit = Fit::width;
        app.bitmap = source;
        app.show_sidebar = true;
        update_sidebar(app);
        layout(app);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        check(app.page_view.visible && app.page_view.source == source, "Live window did not use the GPU");
        const auto texture = app.page_view.bitmap;
        app.resizing_sidebar = true;
        app.sidebar_width += 30;
        resize_layout(app);
        const auto surface = app.page_view.surface;
        app.sidebar_width += 20;
        resize_layout(app);
        const auto vp = viewport(app);
        check(app.page_view.bitmap == texture, "Resize uploaded the unchanged PDF again");
        check(app.page_view.surface == surface, "Shrinking allocated a new GPU surface");
        check(EqualRect(&vp, &app.page_view.bounds), "GPU viewport did not follow the splitter");
        const auto old = app.page_view.bounds;
        const auto bounds = client(app);
        const RECT status{0, bounds.bottom - px(app, 30), bounds.right, bounds.bottom};
        check(paint_gpu_page(app, status) && EqualRect(&old, &app.page_view.bounds),
              "Status-only paint changed the document view");
        app.resizing_sidebar = false;
        ui::close_page_view(app.page_view);
        app.page_view.retry_after = GetTickCount64() + 5000;
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        check(!app.page_view.visible, "CPU fallback re-entered a failed graphics device");
        const auto page = page_rect(app);
        const auto fallback = GetPixel(app.frame.dc, page.left, page.top);
        check(fallback == RGB(255, 0, 0), "CPU fallback did not paint the current PDF");
        app.page_view.retry_after = 0;
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        check(app.page_view.visible, "GPU did not recover after the retry period");
        auto invalid = std::make_shared<RenderedPage>();
        invalid->width = invalid->height = 1;
        check(!ui::present_page_view(app.page_view, window, invalid, first) && !app.page_view.visible &&
                  !app.page_view.source && !app.page_view.device,
              "Failed GPU update left a stale page or device attached");
        app.page_view.retry_after = 0;
        check(ui::present_page_view(app.page_view, window, source, first), "Cannot recreate GPU resources");
        app.bitmap.reset();
        check(!paint_gpu_page(app, client(app)) && !app.page_view.visible && !app.page_view.source,
              "Unloading the PDF left its GPU page visible");
        DestroyWindow(window);
        window = nullptr;
        CoUninitialize();
        std::puts("GPU page pixels, filtering, overlays, clipping, resize, fallback and recovery passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        if (window)
            DestroyWindow(window);
        return 1;
    }
}
