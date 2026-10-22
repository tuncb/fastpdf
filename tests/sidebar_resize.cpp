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
    const uint32_t background = (uint32_t(GetRValue(color)) << 16) |
                                (uint32_t(GetGValue(color)) << 8) | GetBValue(color);
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
    // A resize can erase the background before the next paint is dispatched.
    // The current rows must survive that interval instead of becoming blank.
    SendMessageW(app.bookmark_tree, WM_ERASEBKGND, WPARAM(dc), 0);
    GdiFlush();
    const bool preserved = std::equal(painted.begin(), painted.end(), data);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    check(rows_visible, "Bookmark rows are blank after resizing");
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
    for (int part : {SBP_THUMBBTNHORZ, SBP_THUMBBTNVERT, SBP_LOWERTRACKHORZ, SBP_LOWERTRACKVERT})
        for (int state : {SCRBS_NORMAL, SCRBS_HOT, SCRBS_PRESSED, SCRBS_DISABLED})
            check(scrollbar_pixels(app.window, part, state) ==
                      scrollbar_pixels(app.bookmark_tree, part, state),
                  "PDF scrollbars use a different theme from the sidebar scrollbars");
    const int x = app.scroll_x, y = app.scroll_y;
    SendMessageW(app.window, WM_HSCROLL, SB_LINEDOWN, 0);
    SendMessageW(app.window, WM_VSCROLL, SB_LINEDOWN, 0);
    check(app.scroll_x > x && app.scroll_y > y, "PDF scrollbars did not move the document");
    SendMessageW(app.window, WM_HSCROLL, SB_TOP, 0);
    SendMessageW(app.window, WM_VSCROLL, SB_TOP, 0);
    check(app.scroll_x == 0 && app.scroll_y == 0, "PDF scrollbars did not return to the start");
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
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL,
                                 0, 0, 1200, 900, nullptr, nullptr, app.instance, &app);
        check(window != nullptr, "Cannot create test window");
        app.pages.resize(3);
        app.show_sidebar = true;
        for (int i = 0; i < 40; ++i)
            app.outline.push_back({L"Section " + std::to_wstring(i + 1) +
                                       L" - A long bookmark title for the resize check", 0, 0});
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
                check(TreeView_GetFirstVisible(app.bookmark_tree) == first,
                      "Resize changed the bookmark scroll position");
                check(TreeView_GetSelection(app.bookmark_tree) == selected,
                      "Resize changed the bookmark selection");
                check_bookmark_frame(app);
            }
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(px(app, 317), y));
        }
        DestroyWindow(window);
        std::puts("Reader UI: sidebar resize, PDF scrollbar themes and navigation, and DPI passed");
        return 0;
    } catch (const std::exception& error) {
        if (window)
            DestroyWindow(window);
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
