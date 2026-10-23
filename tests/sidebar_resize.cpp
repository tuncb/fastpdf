// Exercise the real reader controls in-process so their paint DC stays valid.
// The test window remains hidden and does not send desktop input.
#include "../src/app.cpp"
#include <cstdio>
#include <vssym32.h>

static void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

static void check_bookmark_frame(App& app) {
    RECT bounds{};
    GetClientRect(app.bookmark_tree, &bounds);
    const int width = bounds.right, height = bounds.bottom;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check(dc && bitmap, "Cannot create bookmark paint surface");
    auto old = SelectObject(dc, bitmap);
    SendMessageW(app.bookmark_tree, WM_PRINT, WPARAM(dc), PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    const auto data = static_cast<uint32_t*>(pixels);
    const size_t count = size_t(width) * height;
    const std::vector<uint32_t> painted(data, data + count);
    const int row_height = TreeView_GetItemHeight(app.bookmark_tree);
    const auto color = app.search_colors.footer;
    const uint32_t background =
        (uint32_t(GetRValue(color)) << 16) | (uint32_t(GetGValue(color)) << 8) | GetBValue(color);
    bool rows_visible = true;
    // Check unselected rows so a selection fill cannot mask missing text.
    for (int row = 1; row < 6; ++row) {
        int ink = 0;
        for (int y = row * row_height; y < (row + 1) * row_height; ++y)
            for (int x = px(app, 24); x < width - px(app, 24); ++x)
                if ((painted[size_t(y) * width + x] & 0xffffff) != background)
                    ++ink;
        rows_visible = rows_visible && ink > 20;
    }
    RECT selected{};
    check(TreeView_GetItemRect(app.bookmark_tree, TreeView_GetSelection(app.bookmark_tree), &selected, TRUE),
          "Cannot locate the selected bookmark row");
    const int selected_y = (selected.top + selected.bottom) / 2;
    const auto selected_color = app.search_colors.pressed;
    const uint32_t selection = (uint32_t(GetRValue(selected_color)) << 16) |
                               (uint32_t(GetGValue(selected_color)) << 8) | GetBValue(selected_color);
    const bool selection_fits =
        (painted[size_t(selected_y) * width + width - px(app, 10)] & 0xffffff) == selection &&
        (painted[size_t(selected_y) * width + width - px(app, 4)] & 0xffffff) == background;
    // A resize can erase the background before the next paint is dispatched.
    // The current rows must survive that interval instead of becoming blank.
    SendMessageW(app.bookmark_tree, WM_ERASEBKGND, WPARAM(dc), 0);
    GdiFlush();
    const bool preserved = std::equal(painted.begin(), painted.end(), data);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    check(rows_visible, "Bookmark rows are blank after resizing");
    check(selection_fits, "Bookmark selection does not reach the current pane edge with its margin");
    check(preserved, "Resize erased the bookmark rows before repainting");
}

static std::vector<uint32_t> scrollbar_pixels(HWND window, int part, int state) {
    const auto theme = OpenThemeData(window, L"Scrollbar");
    if (!theme)
        return {}; // Windows can disable visual styles, including in high contrast.
    constexpr int width = 32, height = 32;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    HDC dc = CreateCompatibleDC(nullptr);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    check(dc && bitmap, "Cannot create scrollbar paint surface");
    auto old = SelectObject(dc, bitmap);
    const auto data = static_cast<uint32_t*>(pixels);
    std::fill_n(data, width * height, 0);
    RECT bounds{0, 0, width, height};
    const auto result = DrawThemeBackground(theme, dc, part, state, &bounds, nullptr);
    GdiFlush();
    std::vector<uint32_t> painted(data, data + width * height);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    CloseThemeData(theme);
    check(SUCCEEDED(result), "Cannot paint scrollbar theme");
    return painted;
}

static void check_document_scrollbars(App& app) {
    check(!(GetWindowLongPtrW(app.window, GWL_STYLE) & (WS_HSCROLL | WS_VSCROLL)),
          "PDF scrollbars are attached to the whole window");
    const auto bounds = document_bounds(app), vp = viewport(app);
    RECT vertical{}, horizontal{};
    GetWindowRect(app.document_vscroll, &vertical);
    GetWindowRect(app.document_hscroll, &horizontal);
    MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&vertical), 2);
    MapWindowPoints(nullptr, app.window, reinterpret_cast<POINT*>(&horizontal), 2);
    check(vertical.left == vp.right && vertical.right == bounds.right && vertical.top == bounds.top &&
              vertical.bottom == vp.bottom,
          "Vertical scrollbar extends outside the document pane");
    check(horizontal.left == bounds.left && horizontal.right == vp.right && horizontal.top == vp.bottom &&
              horizontal.bottom == bounds.bottom,
          "Horizontal scrollbar extends outside the document pane");
    for (int part : {SBP_THUMBBTNHORZ, SBP_THUMBBTNVERT, SBP_LOWERTRACKHORZ, SBP_LOWERTRACKVERT})
        for (int state : {SCRBS_NORMAL, SCRBS_HOT, SCRBS_PRESSED, SCRBS_DISABLED})
            check(scrollbar_pixels(app.document_vscroll, part, state) ==
                          scrollbar_pixels(app.bookmark_tree, part, state) &&
                      scrollbar_pixels(app.document_hscroll, part, state) ==
                          scrollbar_pixels(app.bookmark_tree, part, state),
                  "PDF scrollbars use a different theme from the sidebar scrollbars");
    const int x = app.scroll_x, y = app.scroll_y;
    SendMessageW(app.window, WM_HSCROLL, SB_LINEDOWN, LPARAM(app.document_hscroll));
    SendMessageW(app.window, WM_VSCROLL, SB_LINEDOWN, LPARAM(app.document_vscroll));
    check(app.scroll_x > x && app.scroll_y > y, "PDF scrollbars did not move the document");
    SendMessageW(app.window, WM_HSCROLL, SB_BOTTOM, LPARAM(app.document_hscroll));
    SendMessageW(app.window, WM_VSCROLL, SB_BOTTOM, LPARAM(app.document_vscroll));
    const auto page = page_rect(app);
    check(page.right == vp.right - px(app, 24) && page.bottom == vp.bottom - px(app, 24),
          "Scrollbars cannot reach the bottom-right page margin");
    SendMessageW(app.window, WM_HSCROLL, SB_TOP, LPARAM(app.document_hscroll));
    SendMessageW(app.window, WM_VSCROLL, SB_TOP, LPARAM(app.document_vscroll));
    check(app.scroll_x == 0 && app.scroll_y == 0, "PDF scrollbars did not return to the start");
}

static void check_document_fit(App& app) {
    const auto visible = [](HWND bar) { return (GetWindowLongPtrW(bar, GWL_STYLE) & WS_VISIBLE) != 0; };
    app.fit = Fit::page;
    request_render(app);
    check(!visible(app.document_hscroll) && !visible(app.document_vscroll),
          "Fit page left document scrollbars visible");
    app.fit = Fit::width;
    request_render(app);
    check(!visible(app.document_hscroll) && visible(app.document_vscroll),
          "Fit width did not reserve space for the vertical scrollbar");
    const auto scale = app.scale;
    request_render(app);
    check(app.scale == scale, "Fit width changes scale on repeated layout");
    app.fit = Fit::custom;
    app.scale = 4;
    app.zoom = app.scale * 72 / app.dpi;
    update_scroll(app);
    check(visible(app.document_hscroll) && visible(app.document_vscroll),
          "Zoom did not show both document scrollbars");
    check_document_scrollbars(app);
}

static void check_fitted_page(App& app) {
    const auto page = page_rect(app), vp = viewport(app);
    const int margin = px(app, 24);
    check(page.left >= vp.left + margin && page.right <= vp.right - margin,
          "Page width did not fit the current pane before the render timer");
    if (app.fit == Fit::page)
        check(page.top >= vp.top + margin && page.bottom <= vp.bottom - margin,
              "Page height did not fit the current window before the render timer");
    else
        // Near the height limit, reserving the vertical bar can make the shorter page fit without it.
        check(vp.right - vp.left - 2 * margin - (page.right - page.left) <=
                  GetSystemMetricsForDpi(SM_CXVSCROLL, app.dpi) + 1,
              "Fit width did not fill the current pane before the render timer");
    wchar_t zoom[32]{};
    GetWindowTextW(app.zoom_edit, zoom, int(std::size(zoom)));
    check(zoom == zoom_percent(app), "Zoom entry is stale during resize");
}

static void check_live_resize(App& app) {
    app.sidebar_width = 260;
    for (auto fit : {Fit::page, Fit::width}) {
        app.fit = fit;
        MoveWindow(app.window, 0, 0, px(app, 1100), px(app, 700), FALSE);
        request_render(app);
        const auto ticket = app.render_ticket;
        const double initial_scale = app.scale;
        SendMessageW(app.window, WM_ENTERSIZEMOVE, 0, 0);
        check(app.resizing_window, "Native resize loop did not enable the fast preview");
        for (int width : {700, 1000, 800, 1100}) {
            MoveWindow(app.window, 0, 0, px(app, width), px(app, 700), FALSE);
            check_fitted_page(app);
            if (width == 700)
                check(app.scale < initial_scale, "Native resize kept the old page scale");
            check(app.render_ticket == ticket, "Native resize started a render on each size message");
        }
        check(app.resize_render_pending, "Native resize did not schedule the final render");
        SendMessageW(app.window, WM_EXITSIZEMOVE, 0, 0);
        check(!live_resize(app) && !app.resize_render_pending && app.render_ticket == ticket + 1,
              "Native resize did not finish with one full-quality render");
        const auto final_ticket = app.render_ticket;
        SendMessageW(app.window, WM_TIMER, 1, 0);
        check(app.render_ticket == final_ticket, "A stale resize timer started a duplicate render");
        SendMessageW(app.window, WM_ENTERSIZEMOVE, 0, 0);
        SendMessageW(app.window, WM_EXITSIZEMOVE, 0, 0);
        check(app.render_ticket == final_ticket, "Moving the window started an unnecessary PDF render");
    }

    for (bool lose_capture : {false, true}) {
        app.fit = Fit::width;
        app.sidebar_width = 260;
        layout(app);
        request_render(app);
        const auto ticket = app.render_ticket;
        const auto splitter = sidebar_splitter(app);
        const int y = splitter.top + px(app, 100);
        SendMessageW(app.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitter.left + px(app, 2), y));
        double previous_scale = app.scale;
        int previous_width = app.sidebar_width;
        for (int width : {460, 180, 600, 320}) {
            SendMessageW(app.window, WM_MOUSEMOVE, MK_LBUTTON,
                         MAKELPARAM(px(app, width) - app.splitter_offset, y));
            check_fitted_page(app);
            check(width > previous_width ? app.scale < previous_scale : app.scale > previous_scale,
                  "Splitter preview did not track the current width");
            check(app.render_ticket == ticket, "Splitter started a render on each mouse move");
            previous_scale = app.scale;
            previous_width = width;
        }
        if (lose_capture)
            ReleaseCapture();
        else
            SendMessageW(app.window, WM_LBUTTONUP, 0, 0);
        check(!live_resize(app) && !app.resize_render_pending && app.render_ticket == ticket + 1,
              "Splitter release or capture loss did not finish the render");
        SendMessageW(app.window, WM_TIMER, 1, 0);
        check(app.render_ticket == ticket + 1, "Splitter left a duplicate render timer");
    }

    app.fit = Fit::custom;
    app.zoom = 2;
    request_render(app);
    const auto ticket = app.render_ticket;
    const auto scale = app.scale;
    SendMessageW(app.window, WM_ENTERSIZEMOVE, 0, 0);
    MoveWindow(app.window, 0, 0, px(app, 900), px(app, 650), FALSE);
    SendMessageW(app.window, WM_EXITSIZEMOVE, 0, 0);
    const auto splitter = sidebar_splitter(app);
    SendMessageW(app.window, WM_LBUTTONDOWN, MK_LBUTTON,
                 MAKELPARAM(splitter.left + px(app, 2), splitter.top + px(app, 100)));
    // Search progress can change the find-bar height while the splitter still has capture.
    app.find_open = true;
    app.show_find_status = true;
    layout(app);
    RECT find_bounds{};
    GetClientRect(app.find_bar, &find_bounds);
    check(find_bounds.bottom == find_height(app) && (GetWindowLongPtrW(app.find_bar, GWL_STYLE) & WS_VISIBLE),
          "Splitter drag blocked a find-bar layout change");
    app.find_open = app.show_find_status = false;
    layout(app);
    SendMessageW(app.window, WM_MOUSEMOVE, MK_LBUTTON,
                 MAKELPARAM(px(app, 460) - app.splitter_offset, splitter.top + px(app, 100)));
    SendMessageW(app.window, WM_LBUTTONUP, 0, 0);
    SendMessageW(app.window, WM_TIMER, 1, 0);
    check(app.scale == scale && app.render_ticket == ticket && !app.resize_render_pending,
          "Resize rerendered or rescaled a page at a fixed zoom");
}

static void check_paint_buffer(App& app) {
    app.fit = Fit::page;
    update_scale(app);
    layout(app);
    app.bitmap = std::make_shared<RenderedPage>();
    app.bitmap->width = app.bitmap->height = 16;
    app.bitmap->pixels.assign(16 * 16 * 4, 255);
    const auto bounds = client(app), page = page_rect(app);
    const int x = (page.left + page.right) / 2, y = (page.top + page.bottom) / 2;
    HDC dc = GetDC(app.window);
    ui::Painter surface;
    check(ui::prepare(surface, dc, bounds.right, bounds.bottom), "Cannot create paint test surface");
    paint_window(app, surface.dc, bounds);
    const auto buffer = app.frame.bitmap;
    const auto before = GetPixel(app.frame.dc, x, y);
    check(before == RGB(255, 255, 255), "Buffered paint did not draw the cached PDF");
    std::fill(app.bitmap->pixels.begin(), app.bitmap->pixels.end(), uint8_t(0));
    const RECT status{0, bounds.bottom - px(app, 30), bounds.right, bounds.bottom};
    paint_window(app, surface.dc, status);
    check(GetPixel(app.frame.dc, x, y) == before, "Status update repainted the PDF outside the dirty area");
    paint_window(app, surface.dc, bounds);
    check(GetPixel(app.frame.dc, x, y) == RGB(0, 0, 0), "Full repaint retained stale PDF pixels");
    check(app.frame.bitmap == buffer, "Same-size paint allocated a new window buffer");
    RECT smaller = bounds;
    smaller.right -= px(app, 100);
    smaller.bottom -= px(app, 100);
    MoveWindow(app.window, 0, 0, smaller.right, smaller.bottom, FALSE);
    paint_window(app, surface.dc, client(app));
    check(app.frame.bitmap == buffer, "Shrinking the window allocated a new paint buffer");
    ui::close(surface);
    ReleaseDC(app.window, dc);
    app.bitmap.reset();
}

static void check_search_updates(App& app) {
    app.find_open = app.show_results = true;
    app.query = L"word";
    app.search_complete = app.search_has_text = true;
    append_search_results(app.matches, {{0, 0, L"word", {}}});
    ListView_SetItemCountEx(app.search_results, 1, LVSICF_NOSCROLL);
    app.active_match = 0;
    sync_search(app);
    layout(app);
    update_scale(app);
    const auto bounds = document_bounds(app);
    const auto ticket = app.render_ticket;
    const auto scale = app.scale;
    const int height = app.find_layout_height;
    const auto visible = [](HWND control) {
        return (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
    };
    const auto stable = [&] {
        const auto current = document_bounds(app);
        check(EqualRect(&bounds, &current) && app.find_layout_height == height,
              "Editing a fast search moved the document pane");
        check(app.render_ticket == ticket && app.scale == scale,
              "Editing a fast search rerendered or rescaled the PDF");
        check(!visible(GetDlgItem(app.find_bar, IDC_FIND_STATUS)) &&
                  !visible(GetDlgItem(app.find_bar, IDC_FIND_STOP)),
              "A fast search flashed the progress row");
    };
    // Exercise the edit notification, debounce, search start, and completion separately.
    // The message loop does not run here, so worker timing cannot hide an intermediate resize.
    for (const auto query : {L"wordzzz", L"word", L""}) {
        SetWindowTextW(app.search, query);
        stable();
        SendMessageW(app.window, WM_TIMER, 3, 0);
        stable();
        cancel_search(app.search_worker);
        app.search_running = false;
        app.search_complete = app.search_has_text = true;
        if (app.query == L"word") {
            append_search_results(app.matches, {{0, 0, L"word", {}}});
            ListView_SetItemCountEx(app.search_results, 1, LVSICF_NOSCROLL);
            app.active_match = 0;
        }
        sync_search(app);
        stable();
        SendMessageW(app.window, WM_TIMER, search_progress_timer, 0);
        stable();
    }
    // A new query must ignore an old timer event, but a slow search still needs Stop.
    SetWindowTextW(app.search, L"slow search");
    start_search(app);
    app.search_progress_after = GetTickCount64() + 60'000;
    SendMessageW(app.window, WM_TIMER, search_progress_timer, 0);
    stable();
    app.search_progress_after = 0;
    SendMessageW(app.window, WM_TIMER, search_progress_timer, 0);
    check(visible(GetDlgItem(app.find_bar, IDC_FIND_STATUS)) &&
              visible(GetDlgItem(app.find_bar, IDC_FIND_STOP)) && app.find_layout_height > height,
          "A slow search did not show progress and Stop");
    SendMessageW(app.find_bar, WM_COMMAND, IDC_FIND_STOP, 0);
    check(!app.search_running && app.search_stopped && visible(GetDlgItem(app.find_bar, IDC_FIND_STATUS)) &&
              !visible(GetDlgItem(app.find_bar, IDC_FIND_STOP)),
          "Stop did not preserve the search status");
    SetWindowTextW(app.search, L"[");
    start_search(app);
    cancel_search(app.search_worker);
    app.search_running = false;
    app.search_failed = true;
    app.search_notice = L"Invalid regex: missing closing bracket";
    sync_search(app);
    check(visible(GetDlgItem(app.find_bar, IDC_FIND_STATUS)), "Search errors were delayed");
    SetWindowTextW(app.search, L"");
    start_search(app);
    command(app, cmd_search);
    SendMessageW(app.window, WM_TIMER, search_progress_timer, 0);
    check(!visible(app.find_bar), "A stale progress timer reopened Find");
}

int main() {
    App app;
    app.instance = GetModuleHandleW(nullptr);
    HWND window = nullptr;
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ui::enable_system_dialog_theme();
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES |
                                                            ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                            ICC_LISTVIEW_CLASSES};
        check(InitCommonControlsEx(&controls) != FALSE, "Cannot initialize controls");
        WNDCLASSW type{};
        type.lpfnWndProc = window_proc;
        type.hInstance = app.instance;
        type.lpszClassName = L"FastPDF.SidebarResizeTest";
        check(RegisterClassW(&type) != 0, "Cannot register test window");
        window = CreateWindowExW(0, type.lpszClassName, L"Sidebar resize test",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, 1200, 900, nullptr, nullptr,
                                 app.instance, &app);
        check(window != nullptr, "Cannot create test window");
        app.pages.resize(3);
        app.show_sidebar = true;
        for (int i = 0; i < 40; ++i)
            app.outline.push_back(
                {L"Section " + std::to_wstring(i + 1) + L" - A long bookmark title for the resize check", 0,
                 0});
        rebuild_bookmarks(app);
        update_sidebar(app);
        app.scale = 4;
        layout(app);
        check_document_scrollbars(app);
        for (UINT message : {WM_SETTINGCHANGE, WM_SYSCOLORCHANGE, WM_THEMECHANGED}) {
            SendMessageW(window, message, 0, 0);
            check_document_scrollbars(app);
        }
        for (UINT dpi : {96u, 144u, 192u}) {
            app.dpi = dpi;
            fonts(app);
            MoveWindow(window, 0, 0, px(app, 1100), px(app, 700), FALSE);
            layout(app);
            check_document_fit(app);
            app.find_open = true;
            layout(app);
            check_document_scrollbars(app);
            app.find_open = false;
            app.show_sidebar = false;
            layout(app);
            check_document_scrollbars(app);
            app.show_sidebar = true;
            layout(app);
            const auto first = TreeView_GetFirstVisible(app.bookmark_tree);
            const auto selected = TreeView_GetSelection(app.bookmark_tree);
            auto splitter = sidebar_splitter(app);
            const int y = splitter.top + px(app, 120);
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM((splitter.left + splitter.right) / 2, y));
            check(app.resizing_sidebar, "Splitter drag did not start");
            for (int width : {200, 460, 180, 600, 320, 500, 160, 320}) {
                SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON,
                             MAKELPARAM(px(app, width) - app.splitter_offset, y));
                RECT tree{}, tabs{};
                GetWindowRect(app.bookmark_tree, &tree);
                GetWindowRect(app.sidebar_tabs, &tabs);
                check(tree.right - tree.left == px(app, width) - px(app, ui::splitter_width),
                      "Bookmark width did not follow the splitter");
                check(tabs.bottom == tree.top, "Tabs and bookmarks no longer align");
                SCROLLBARINFO scrollbar{sizeof(scrollbar)};
                check(GetScrollBarInfo(app.bookmark_tree, OBJID_VSCROLL, &scrollbar) &&
                          scrollbar.rcScrollBar.right == tree.right,
                      "Bookmark scrollbar did not follow the current pane edge");
                check(TreeView_GetFirstVisible(app.bookmark_tree) == first,
                      "Resize changed the bookmark scroll position");
                check(TreeView_GetSelection(app.bookmark_tree) == selected,
                      "Resize changed the bookmark selection");
                check_bookmark_frame(app);
                check_document_scrollbars(app);
            }
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(px(app, 317), y));
            check_live_resize(app);
            check_paint_buffer(app);
            for (int width : {1100, 640}) {
                MoveWindow(window, 0, 0, px(app, width), px(app, 700), FALSE);
                check_search_updates(app);
            }
        }
        DestroyWindow(window);
        check(!app.frame.dc && !app.frame.bitmap, "Window destruction leaked the paint buffer");
        std::puts("Reader UI: live resize, search updates, paint buffering, sidebar rows, PDF scrollbars, "
                  "and DPI passed");
        return 0;
    } catch (const std::exception& error) {
        if (window)
            DestroyWindow(window);
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
