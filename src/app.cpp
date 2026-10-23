#include "render.h"
#include "resource.h"
#include "deployment.h"
#include "installation_dialog.h"
#include "dialog_style.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cwctype>
#include <future>
#include <exception>
#include <optional>
#include "ui_style.h"
#include "search.h"
#include "search_accessibility.h"
#include "page_view.h"

using namespace pdf;
constexpr UINT worker_message = WM_APP + 1;
constexpr UINT search_message = WM_APP + 3;
constexpr UINT_PTR search_progress_timer = 5;
constexpr UINT search_progress_delay = 300;
constexpr int id_sidebar = 200, id_toolbar = 202, id_page = 203, id_zoom = 204;
constexpr int id_bookmark_tree = 205;
constexpr int id_find_bar = 206, id_results = 207, id_sidebar_tabs = 208;
constexpr int id_results_summary = 209;
constexpr int id_document_vscroll = 210, id_document_hscroll = 211;
constexpr int page_slot = 100, zoom_slot = 101, toolbar_height = 36;
constexpr const wchar_t* menu_titles[] = {L"&File", L"&View", L"&Help"};
enum class Job { open, render, preview, print };
enum class Fit { page, width, custom };
struct PageInfo {
    double width = 612, height = 792;
    int rotation = 0;
};
struct PrintJob {
    HDC dc = nullptr;
    int first = 0, last = 0;
};
static void destroy_print_job(PrintJob* job) {
    if (job) {
        if (job->dc)
            DeleteDC(job->dc);
        delete job;
    }
}
struct Request {
    Job kind = Job::render;
    uint64_t document = 0, ticket = 0;
    std::wstring path, password;
    int page = 0, rotation = 0;
    double scale = 1;
    std::shared_ptr<PrintJob> print;
};
struct Result {
    Job kind = Job::render;
    uint64_t document = 0, ticket = 0;
    int page = 0;
    double scale = 1, open_ms = 0;
    std::wstring error, message;
    std::vector<PageInfo> pages;
    std::vector<Outline> outline;
    std::shared_ptr<RenderedPage> bitmap;
};
struct Cache {
    int page = 0, rotation = 0;
    double scale = 1;
    std::shared_ptr<RenderedPage> bitmap;
};
struct Worker {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Request> jobs;
    std::deque<Result> results;
    std::atomic<bool> cancel = false, stop = false;
    bool io_active = false;
    std::thread thread;
    HWND window = nullptr;
};
enum Command {
    cmd_open = 1,
    cmd_prev,
    cmd_next,
    cmd_zoom_out,
    cmd_zoom_in,
    cmd_fit,
    cmd_width,
    cmd_rotate,
    cmd_sidebar,
    cmd_bookmarks,
    cmd_search,
    cmd_print = 13,
    cmd_goto = 15,
    cmd_about,
    cmd_install,
    cmd_updates,
    cmd_portable = 20,
    cmd_exit
};
struct UpdateResult {
    deployment::Release release;
    deployment::Download download;
    std::wstring error;
    bool downloading = false;
};
struct ToolbarTool {
    int command;
    ToolbarIcon icon;
    const wchar_t* name;
};
struct MenuItem {
    std::wstring title, shortcut;
    bool bar = false, separator = false;
};
constexpr ToolbarTool toolbar_tools[] = {
    {cmd_open, ToolbarIcon::open, L"Open PDF (Ctrl+O)"},
    {cmd_print, ToolbarIcon::print, L"Print (Ctrl+P)"},
    {cmd_prev, ToolbarIcon::previous, L"Previous page (Page Up)"},
    {cmd_next, ToolbarIcon::next, L"Next page (Page Down)"},
    {cmd_zoom_out, ToolbarIcon::minus, L"Zoom out (-)"},
    {cmd_zoom_in, ToolbarIcon::plus, L"Zoom in (+)"},
    {cmd_fit, ToolbarIcon::fit, L"Fit page (F)"},
    {cmd_width, ToolbarIcon::width, L"Fit width (W)"},
    {cmd_rotate, ToolbarIcon::rotate, L"Rotate clockwise (R)"},
    {cmd_search, ToolbarIcon::find, L"Find text (Ctrl+F)"},
    {cmd_sidebar, ToolbarIcon::pages, L"Page list"},
    {cmd_bookmarks, ToolbarIcon::bookmarks, L"Bookmarks (B)"},
};
struct App {
    HWND window = nullptr, sidebar = nullptr, search = nullptr;
    HWND document_vscroll = nullptr, document_hscroll = nullptr;
    HWND bookmark_tree = nullptr, sidebar_tabs = nullptr;
    std::vector<HTREEITEM> bookmark_items;
    int sidebar_width = ui::sidebar_width, splitter_offset = 0;
    int sidebar_header_height = ui::sidebar_header, tab_hover = -1;
    bool resizing_sidebar = false, resizing_window = false, resize_render_pending = false;
    bool syncing_bookmarks = false;
    HWND toolbar = nullptr, page_edit = nullptr, zoom_edit = nullptr;
    HWND welcome_install = nullptr, welcome_portable = nullptr;
    HINSTANCE instance = nullptr;
    HICON icon = nullptr, small_icon = nullptr;
    HFONT font = nullptr, title_font = nullptr, small_font = nullptr, selected_font = nullptr;
    HBRUSH edit_background = nullptr, chrome_background = nullptr;
    std::deque<MenuItem> menu_items;
    ui::Painter painter, frame;
    ui::PageView page_view;
    HWND input_hover = nullptr;
    int sidebar_hover = -1;
    Worker worker;
    SearchWorker search_worker;
    HWND find_bar = nullptr, search_results = nullptr, results_summary = nullptr;
    SearchOptions search_options;
    SearchResults matches;
    bool syncing_results = false;
    int active_match = -1, search_start_page = 0, search_checked = 0, search_total = 0;
    bool search_running = false, search_complete = false, search_has_text = false, search_failed = false;
    bool show_results = false, reveal_match = false, search_stopped = false;
    bool search_enter_pending = true, show_find_status = false;
    ULONGLONG search_progress_after = 0;
    bool sidebar_before_find = false, bookmarks_before_find = true;
    int find_layout_height = 0, summary_layout_height = 0;
    std::wstring search_notice, search_summary;
    ui::DialogColors search_colors{};
    bool setting_window_theme = false;
    HBRUSH results_background = nullptr;
    HFONT search_font = nullptr;
    UINT dpi = 96;
    uint64_t document = 0, ticket = 0, render_ticket = 0, search_ticket = 0;
    std::wstring path, password, status = L"Open a PDF to begin", query;
    std::vector<PageInfo> pages;
    std::vector<Outline> outline;
    std::shared_ptr<RenderedPage> bitmap;
    HWND print_preview = nullptr;
    std::shared_ptr<RenderedPage> preview_bitmap;
    std::wstring preview_error;
    int preview_page = 0;
    uint64_t preview_ticket = 0;
    int page = 0, rotation = 0, scroll_x = 0, scroll_y = 0;
    double scale = 1, bitmap_scale = 1, zoom = 1;
    Fit fit = Fit::page;
    bool show_sidebar = false, bookmarks = true, find_open = false, busy = false, dragging = false,
         panning = false, selected_all = false;
    POINT drag_start{}, drag_end{}, pan_start{};
    int pan_x = 0, pan_y = 0;
    std::vector<size_t> selected;
    std::wstring smoke_output;
    bool smoke = false;
    int exit_code = 0;
    bool installed = false, portable_choice = false, update_on_exit = false;
    std::optional<deployment::Session> restore;
    std::atomic<bool> update_cancel = false;
    std::future<UpdateResult> update_job;
    deployment::Download pending_update;
    std::wstring update_status;
};
static void sync_toolbar(App& a);
static void layout_toolbar(App& a);
static void create_toolbar(App& a);
static void paint_toolbar(App& a, HDC dc);
static void paint_toolbar_button(App& a, HDC dc, RECT rect, int id, UINT state);
static void command(App& a, int id);
static void paint_input_frame(App& a, HDC dc, HWND parent, HWND input);
static void paint_sidebar_item(App& a, HDC dc, RECT rect, int index, bool selected, bool focused);
static void label(HDC dc, RECT rect, std::wstring_view text, COLORREF color, HFONT font, UINT flags);
static void find(App& a, int direction = 1);
static void start_search(App& a);
static void sync_search(App& a);
static void fit_search_results(HWND window) {
    RECT bounds{};
    GetClientRect(window, &bounds);
    // The native list needs one pixel beyond the column to avoid a horizontal scrollbar.
    const int width = std::max(0, int(bounds.right) - 1);
    const int current = ListView_GetColumnWidth(window, 0);
    const bool overflow = (GetWindowLongPtrW(window, GWL_STYLE) & WS_HSCROLL) != 0;
    // Showing a hidden list can restore stale scroll ranges. A same-width assignment is a no-op.
    if (overflow && current == width)
        ListView_SetColumnWidth(window, 0, std::max(0, width - 1));
    if (current != width || overflow)
        ListView_SetColumnWidth(window, 0, width);
}
static void clear_search_results(App& a) {
    KillTimer(a.window, 4);
    KillTimer(a.window, search_progress_timer);
    a.syncing_results = true;
    a.active_match = -1;
    if (ListView_GetItemCount(a.search_results) != 0)
        ListView_SetItemCountEx(a.search_results, 0, LVSICF_NOSCROLL);
    fit_search_results(a.search_results);
    a.syncing_results = false;
    retire_search_results(a.search_worker, a.matches);
}
static void select_match(App& a, int index);
static void reveal_search_match(App& a);
static INT_PTR CALLBACK find_bar_proc(HWND window, UINT message, WPARAM wp, LPARAM lp);
static std::wstring lower(std::wstring s) {
    for (auto& c : s)
        c = wchar_t(std::towlower(c));
    return s;
}
static void post(Worker& w, Result result) {
    if (w.stop)
        return;
    {
        std::lock_guard lock(w.mutex);
        w.results.push_back(std::move(result));
    }
    PostMessageW(w.window, worker_message, 0, 0);
}
static void worker_run(Worker& w) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Document document;
    document.cancel = &w.cancel;
    Renderer renderer;
    uint64_t generation = 0;
    std::deque<Cache> cache;
    size_t cache_bytes = 0;
    int last_page = -1;
    try {
        hr(com, "Initialize render thread");
        init_renderer(renderer);
        auto render = [&](int page, double scale, int rotation) {
            for (auto it = cache.begin(); it != cache.end(); ++it)
                if (it->page == page && it->rotation == rotation && std::abs(it->scale - scale) < 0.00001) {
                    auto hit = *it;
                    cache.erase(it);
                    cache.push_front(hit);
                    return hit.bitmap;
                }
            auto image = std::make_shared<RenderedPage>(
                render_page(renderer, document, size_t(page), scale, rotation));
            while (!cache.empty() && cache_bytes + image->pixels.size() > 192 * 1024 * 1024) {
                cache_bytes -= cache.back().bitmap->pixels.size();
                cache.pop_back();
            }
            if (image->pixels.size() <= 192 * 1024 * 1024) {
                cache.push_front({page, rotation, scale, image});
                cache_bytes += image->pixels.size();
            }
            return image;
        };
        while (!w.stop) {
            Request request;
            {
                std::unique_lock lock(w.mutex);
                w.ready.wait(lock, [&] { return w.stop || !w.jobs.empty(); });
                if (w.stop)
                    break;
                request = std::move(w.jobs.front());
                w.jobs.pop_front();
                w.cancel = false;
                w.io_active = request.kind == Job::print;
            }
            Result result;
            result.kind = request.kind;
            result.document = request.document;
            result.ticket = request.ticket;
            result.page = request.page;
            result.scale = request.scale;
            try {
                if (request.kind == Job::open) {
                    cache.clear();
                    cache_bytes = 0;
                    last_page = -1;
                    clear_resources(renderer);
                    auto start = std::chrono::steady_clock::now();
                    open(document, request.path, utf8(request.password));
                    generation = request.document;
                    result.open_ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                            .count();
                    for (auto& p : document.pages)
                        result.pages.push_back({(p.box.x1 - p.box.x0) * p.user_unit,
                                                (p.box.y1 - p.box.y0) * p.user_unit, p.rotation});
                    result.outline = document.outline;
                    result.message = L"Opened in " + std::to_wstring(int(result.open_ms)) + L" ms";
                } else {
                    require(generation == request.document && !document.pages.empty(),
                            "Document is no longer open");
                    if (request.kind == Job::render || request.kind == Job::preview)
                        result.bitmap = render(request.page, request.scale, request.rotation);
                    else if (request.kind == Job::print) {
                        HDC dc = request.print->dc;
                        DOCINFOW info{sizeof(info)};
                        info.lpszDocName = document.path.c_str();
                        require(StartDocW(dc, &info) > 0, "Printer could not start the document");
                        try {
                            for (int p = request.print->first; p <= request.print->last; ++p) {
                                check_cancel(document);
                                auto& page = document.pages[size_t(p)];
                                double width = (page.box.x1 - page.box.x0) * page.user_unit,
                                       height = (page.box.y1 - page.box.y0) * page.user_unit;
                                if (page.rotation % 180)
                                    std::swap(width, height);
                                double scale =
                                    std::min({double(GetDeviceCaps(dc, HORZRES)) / width,
                                              double(GetDeviceCaps(dc, VERTRES)) / height, 300.0 / 72});
                                auto image = render(p, scale, 0);
                                require(StartPage(dc) > 0, "Printer could not start a page");
                                int pw = GetDeviceCaps(dc, HORZRES), ph = GetDeviceCaps(dc, VERTRES);
                                double fit = std::min(double(pw) / image->width, double(ph) / image->height);
                                int dw = int(image->width * fit), dh = int(image->height * fit);
                                BITMAPINFO bmi{};
                                bmi.bmiHeader = {sizeof(BITMAPINFOHEADER),
                                                 LONG(image->width),
                                                 -LONG(image->height),
                                                 1,
                                                 32,
                                                 BI_RGB,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 0};
                                SetStretchBltMode(dc, HALFTONE);
                                int copied = StretchDIBits(
                                    dc, (pw - dw) / 2, (ph - dh) / 2, dw, dh, 0, 0, int(image->width),
                                    int(image->height), image->pixels.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
                                require(copied != 0 && copied != GDI_ERROR,
                                        "Printer could not draw the page");
                                require(EndPage(dc) > 0, "Printer could not finish a page");
                            }
                            require(EndDoc(dc) > 0, "Printer could not finish the document");
                        } catch (...) {
                            AbortDoc(dc);
                            throw;
                        }
                        result.message = L"Document sent to the printer";
                    }
                }
            } catch (const std::exception& e) {
                if (w.cancel || w.stop)
                    continue;
                result.error = utf8(e.what());
            }
            {
                std::lock_guard lock(w.mutex);
                w.io_active = false;
            }
            post(w, std::move(result));
            // Decode neighboring pages only while the interactive queue is empty.
            if (request.kind == Job::render && !w.cancel && !w.stop && generation == request.document) {
                int direction = last_page >= 0 && request.page < last_page ? -1 : 1;
                last_page = request.page;
                for (int neighbor :
                     {request.page + direction, request.page + 2 * direction, request.page - direction}) {
                    {
                        std::lock_guard lock(w.mutex);
                        if (!w.jobs.empty() || w.stop)
                            break;
                    }
                    if (neighbor >= 0 && size_t(neighbor) < document.pages.size())
                        try {
                            render(neighbor, request.scale, request.rotation);
                        } catch (const std::exception&) {
                            if (w.cancel)
                                break;
                        }
                }
            }
        }
    } catch (const std::exception& e) {
        Result result;
        result.error = utf8(e.what());
        post(w, std::move(result));
    }
    close_renderer(renderer);
    close(document);
    if (SUCCEEDED(com))
        CoUninitialize();
}
static void enqueue(App& a, Request request) {
    request.document = a.document;
    request.ticket = ++a.ticket;
    if (request.kind == Job::render)
        a.render_ticket = request.ticket;
    if (request.kind == Job::preview)
        a.preview_ticket = request.ticket;
    {
        std::lock_guard lock(a.worker.mutex);
        // A preview must not cancel the reader's pending render or search.
        if (!a.worker.io_active && request.kind != Job::preview)
            a.worker.cancel = true;
        if (request.kind == Job::open)
            a.worker.jobs.clear();
        else
            std::erase_if(a.worker.jobs, [&](auto& job) { return job.kind == request.kind; });
        a.worker.jobs.push_back(std::move(request));
    }
    a.worker.ready.notify_one();
}
static int px(const App& a, int value) {
    return MulDiv(value, int(a.dpi), 96);
}
static RECT client(App& a) {
    RECT r{};
    GetClientRect(a.window, &r);
    return r;
}
static bool live_resize(const App& a) {
    return a.resizing_sidebar || a.resizing_window;
}
static void position_control(HWND window, int x, int y, int width, int height) {
    RECT current{};
    GetWindowRect(window, &current);
    MapWindowPoints(nullptr, GetParent(window), reinterpret_cast<POINT*>(&current), 2);
    if (current.left == x && current.top == y && current.right - current.left == width &&
        current.bottom - current.top == height)
        return;
    // Layout paints once, after every control has its final bounds.
    SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
}
static void show_control(HWND window, bool visible) {
    if (((GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0) != visible)
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW |
                         (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}
static int find_width_units(App& a) {
    const auto unit = ui::dialog_rect(a.find_bar, {0, 0, 4, 8});
    return MulDiv(client(a).right, 4, std::max(1L, unit.right));
}
static int find_height(App& a) {
    if (!a.find_open || !a.find_bar)
        return 0;
    const auto units = ui::dialog_rect(
        a.find_bar, {0, 0, 0, (find_width_units(a) < 350 ? 64 : 44) + (a.show_find_status ? 18 : 0)});
    return units.bottom;
}
static bool results_visible(const App& a) {
    return a.find_open && a.show_results;
}
static bool sidebar_visible(const App& a) {
    return (a.show_sidebar || results_visible(a)) && !a.pages.empty();
}
static RECT document_bounds(App& a) {
    RECT r = client(a);
    r.top = px(a, toolbar_height) + find_height(a);
    r.bottom -= px(a, 30);
    if (sidebar_visible(a)) {
        int maximum = std::max(0, int(r.right) - px(a, ui::document_min_width));
        r.left = std::clamp(px(a, a.sidebar_width), std::min(px(a, ui::sidebar_min_width), maximum), maximum);
    }
    r.right = std::max(r.left, r.right);
    r.bottom = std::max(r.top, r.bottom);
    return r;
}
static RECT sidebar_splitter(App& a) {
    auto r = document_bounds(a);
    r.right = r.left;
    r.left = std::max(0L, r.right - px(a, ui::splitter_width));
    return r;
}
static std::pair<double, double> dimensions(App& a) {
    if (a.pages.empty())
        return {0, 0};
    auto p = a.pages[size_t(a.page)];
    double w = p.width, h = p.height;
    if ((p.rotation + a.rotation) % 180)
        std::swap(w, h);
    return {w, h};
}
static RECT viewport(App& a) {
    auto r = document_bounds(a);
    if (a.pages.empty())
        return r;
    const auto [w, h] = dimensions(a);
    const int total_w = int(std::ceil(w * a.scale)) + 2 * px(a, 24),
              total_h = int(std::ceil(h * a.scale)) + 2 * px(a, 24);
    const int bar_w = GetSystemMetricsForDpi(SM_CXVSCROLL, a.dpi),
              bar_h = GetSystemMetricsForDpi(SM_CYHSCROLL, a.dpi);
    bool vertical = false, horizontal = false;
    // Each scrollbar can make the other axis overflow. Start with the full pane
    // so bars disappear as soon as the page fits again.
    for (int pass = 0; pass < 2; ++pass) {
        vertical = total_h > r.bottom - r.top - (horizontal ? bar_h : 0);
        horizontal = total_w > r.right - r.left - (vertical ? bar_w : 0);
    }
    if (vertical)
        r.right = std::max(r.left, r.right - bar_w);
    if (horizontal)
        r.bottom = std::max(r.top, r.bottom - bar_h);
    return r;
}
static void update_scroll(App& a, bool repaint = true) {
    const auto bounds = document_bounds(a);
    auto r = viewport(a);
    auto [w, h] = dimensions(a);
    int width = std::max<LONG>(1, r.right - r.left), height = std::max<LONG>(1, r.bottom - r.top),
        margin = px(a, 24);
    int total_w = a.pages.empty() ? 0 : int(std::ceil(w * a.scale)) + 2 * margin,
        total_h = a.pages.empty() ? 0 : int(std::ceil(h * a.scale)) + 2 * margin;
    a.scroll_x = std::clamp(a.scroll_x, 0, std::max(0, total_w - width));
    a.scroll_y = std::clamp(a.scroll_y, 0, std::max(0, total_h - height));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMin = 0;
    si.nMax = std::max(0, total_h - 1);
    si.nPage = UINT(height);
    si.nPos = a.scroll_y;
    SetScrollInfo(a.document_vscroll, SB_CTL, &si, repaint);
    si.nMax = std::max(0, total_w - 1);
    si.nPage = UINT(width);
    si.nPos = a.scroll_x;
    SetScrollInfo(a.document_hscroll, SB_CTL, &si, repaint);
    position_control(a.document_vscroll, r.right, r.top, bounds.right - r.right, r.bottom - r.top);
    position_control(a.document_hscroll, r.left, r.bottom, r.right - r.left, bounds.bottom - r.bottom);
    show_control(a.document_vscroll, r.right < bounds.right);
    show_control(a.document_hscroll, r.bottom < bounds.bottom);
    if (repaint)
        for (auto control : {a.document_vscroll, a.document_hscroll})
            InvalidateRect(control, nullptr, FALSE);
}
static void update_scale(App& a) {
    if (a.pages.empty())
        return;
    auto r = document_bounds(a);
    auto [w, h] = dimensions(a);
    double available_w = std::max<LONG>(32, r.right - r.left - px(a, 48)),
           available_h = std::max<LONG>(32, r.bottom - r.top - px(a, 48));
    if (a.fit == Fit::page)
        a.scale = std::min(available_w / w, available_h / h);
    else if (a.fit == Fit::width) {
        if (h * (available_w / w) > available_h)
            available_w = std::max(32.0, available_w - GetSystemMetricsForDpi(SM_CXVSCROLL, a.dpi));
        a.scale = available_w / w;
    } else
        a.scale = a.zoom * double(a.dpi) / 72;
    // Keep rounding to whole pixels from making a fitted page overflow by one pixel.
    if (a.fit != Fit::custom)
        a.scale = std::nextafter(a.scale, 0.0);
    a.scale = std::clamp(a.scale, 0.05, 12.0);
    double pixels = w * h * a.scale * a.scale;
    if (pixels > 60'000'000)
        a.scale *= std::sqrt(60'000'000 / pixels);
}
static void request_render(App& a, bool reset_scroll = false) {
    KillTimer(a.window, 1);
    a.resize_render_pending = false;
    if (a.pages.empty()) {
        sync_toolbar(a);
        return;
    }
    update_scale(a);
    if (reset_scroll)
        a.scroll_x = a.scroll_y = 0;
    update_scroll(a);
    a.busy = true;
    a.status = L"Rendering page " + std::to_wstring(a.page + 1) + L"…";
    Request request;
    request.kind = Job::render;
    request.page = a.page;
    request.scale = a.scale;
    request.rotation = a.rotation;
    enqueue(a, std::move(request));
    sync_toolbar(a);
    InvalidateRect(a.window, nullptr, FALSE);
}
static void style_menu(App& a, HMENU menu, bool bar) {
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = bar ? a.chrome_background : a.edit_background;
    SetMenuInfo(menu, &info);
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_SUBMENU | MIIM_STRING;
        if (!GetMenuItemInfoW(menu, UINT(index), TRUE, &item))
            continue;
        if (!(item.fType & MFT_OWNERDRAW)) {
            std::wstring title(size_t(item.cch) + 1, L'\0');
            item.dwTypeData = title.data();
            ++item.cch;
            GetMenuItemInfoW(menu, UINT(index), TRUE, &item);
            title.resize(item.cch);
            std::wstring shortcut;
            if (auto tab = title.find(L'\t'); tab != std::wstring::npos) {
                shortcut = title.substr(tab + 1);
                title.resize(tab);
            }
            a.menu_items.push_back(
                {std::move(title), std::move(shortcut), bar, (item.fType & MFT_SEPARATOR) != 0});
            item.dwItemData = reinterpret_cast<ULONG_PTR>(&a.menu_items.back());
        }
        // Keep the native strings and submenus for accessibility and menu navigation.
        item.fMask = MIIM_FTYPE | MIIM_DATA;
        item.fType |= MFT_OWNERDRAW;
        SetMenuItemInfoW(menu, UINT(index), TRUE, &item);
        if (item.hSubMenu)
            style_menu(a, item.hSubMenu, false);
    }
}
static void fonts(App& a) {
    for (auto f : {a.font, a.title_font, a.small_font, a.selected_font})
        if (f)
            DeleteObject(f);
    a.font =
        CreateFontW(-px(a, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    a.title_font =
        CreateFontW(-px(a, 28), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    a.small_font =
        CreateFontW(-px(a, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    a.selected_font =
        CreateFontW(-px(a, 13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(a.sidebar, WM_SETFONT, WPARAM(a.font), TRUE);
    SendMessageW(a.sidebar, LB_SETITEMHEIGHT, 0, px(a, ui::row_height));
    SendMessageW(a.bookmark_tree, WM_SETFONT, WPARAM(a.font), TRUE);
    TreeView_SetItemHeight(a.bookmark_tree, px(a, ui::row_height));
    TreeView_SetIndent(a.bookmark_tree, px(a, 20));
    if (a.search_font)
        DeleteObject(a.search_font);
    a.search_font =
        CreateFontW(-MulDiv(10, int(a.dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (a.find_bar) {
        SendMessageW(a.find_bar, WM_SETFONT, WPARAM(a.search_font), FALSE);
        for (auto child = GetWindow(a.find_bar, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, WPARAM(a.search_font), TRUE);
    }
    if (a.search_results) {
        SendMessageW(a.search_results, WM_SETFONT, WPARAM(a.search_font), TRUE);
    }
    SendMessageW(a.results_summary, WM_SETFONT, WPARAM(a.search_font), TRUE);
    SendMessageW(a.sidebar_tabs, WM_SETFONT, WPARAM(a.search_font), TRUE);
    TabCtrl_SetPadding(a.sidebar_tabs, px(a, 12), 0);
    TabCtrl_SetItemSize(a.sidebar_tabs, 0, px(a, ui::sidebar_header));
    for (auto control : {a.welcome_install, a.welcome_portable})
        SendMessageW(control, WM_SETFONT, WPARAM(a.font), TRUE);
    for (auto control : {a.toolbar, a.page_edit, a.zoom_edit})
        SendMessageW(control, WM_SETFONT, WPARAM(a.small_font), TRUE);
    style_menu(a, GetMenu(a.window), true);
    DrawMenuBar(a.window);
}
static int search_summary_height(App& a) {
    RECT text{0, 0, std::max(px(a, 24), int(sidebar_splitter(a).left) - px(a, 24)), 0};
    const auto dc = GetDC(a.window);
    const auto old = SelectObject(dc, a.search_font);
    DrawTextW(dc, a.search_summary.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(a.window, dc);
    return std::max(px(a, 18), int(text.bottom)) + px(a, 16);
}
static void layout_search_results(App& a) {
    const auto r = client(a);
    const int sidebar_width = sidebar_splitter(a).left;
    const int list_top = px(a, toolbar_height) + a.find_layout_height + a.sidebar_header_height;
    a.summary_layout_height = search_summary_height(a);
    position_control(a.results_summary, px(a, 12), list_top + px(a, 8),
                     std::max(0, sidebar_width - px(a, 24)), a.summary_layout_height - px(a, 16));
    show_control(a.results_summary, results_visible(a));
    const int results_top = list_top + a.summary_layout_height;
    position_control(a.search_results, 0, results_top, sidebar_width,
                     std::max<LONG>(0, r.bottom - results_top - px(a, 30)));
    if (!results_visible(a) && GetFocus() == a.search_results)
        SetFocus(a.window);
    show_control(a.search_results, results_visible(a));
    fit_search_results(a.search_results);
}
static void layout(App& a) {
    auto r = client(a);
    const int next_find_height = find_height(a);
    const bool layout_find = !a.resizing_sidebar || next_find_height != a.find_layout_height;
    a.find_layout_height = next_find_height;
    int top = px(a, toolbar_height) + a.find_layout_height;
    const int tab_count = a.find_open ? 3 : 2;
    while (TabCtrl_GetItemCount(a.sidebar_tabs) > tab_count)
        TabCtrl_DeleteItem(a.sidebar_tabs, tab_count);
    constexpr const wchar_t* titles[] = {L"Pages", L"Bookmarks", L"Results"};
    for (int i = TabCtrl_GetItemCount(a.sidebar_tabs); i < tab_count; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(titles[i]);
        TabCtrl_InsertItem(a.sidebar_tabs, i, &item);
    }
    const int tab = results_visible(a) ? 2 : a.bookmarks ? 1 : 0;
    if (TabCtrl_GetCurSel(a.sidebar_tabs) != tab)
        TabCtrl_SetCurSel(a.sidebar_tabs, tab);
    const int sidebar_width = sidebar_splitter(a).left;
    position_control(a.sidebar_tabs, 0, top, sidebar_width, a.sidebar_header_height);
    RECT tab_area{0, 0, sidebar_width, px(a, ui::sidebar_header)};
    TabCtrl_AdjustRect(a.sidebar_tabs, FALSE, &tab_area);
    a.sidebar_header_height = std::max(px(a, ui::sidebar_header), int(tab_area.top));
    position_control(a.sidebar_tabs, 0, top, sidebar_width, a.sidebar_header_height);
    show_control(a.sidebar_tabs, sidebar_visible(a));
    int list_top = top + a.sidebar_header_height;
    for (auto control : {a.sidebar, a.bookmark_tree}) {
        position_control(control, 0, list_top, sidebar_width,
                         std::max<LONG>(0, r.bottom - list_top - px(a, 30)));
        bool visible = a.show_sidebar && !results_visible(a) && !a.pages.empty() &&
                       (control == a.bookmark_tree ? a.bookmarks : !a.bookmarks);
        if (!visible && GetFocus() == control)
            SetFocus(a.window);
        show_control(control, visible);
    }
    if (a.find_bar && layout_find) {
        position_control(a.find_bar, 0, px(a, toolbar_height), r.right, find_height(a));
        if (a.find_open) {
            const int width = find_width_units(a);
            const bool wrapped = width < 350;
            const int button_y = wrapped ? 43 : 23, status_y = button_y + 20;
            int right = width - 12;
            for (int id : {IDC_FIND_NEXT, IDC_FIND_PREVIOUS}) {
                ui::position_dialog_control(a.find_bar, id, {right - 18, button_y, right, button_y + 14});
                right -= 20;
            }
            ui::position_dialog_control(a.find_bar, IDC_FIND_COUNT,
                                        {right - 68, button_y + 1, right - 4, button_y + 13});
            ui::position_dialog_control(a.find_bar, IDC_FIND_SCOPE, {12, 23, 112, 87});
            // A native combo keeps its own closed height; center it within the control row.
            const auto row = ui::dialog_rect(a.find_bar, {12, 23, 112, 37});
            RECT combo{};
            const auto scope = GetDlgItem(a.find_bar, IDC_FIND_SCOPE);
            GetWindowRect(scope, &combo);
            SetWindowPos(scope, nullptr, row.left,
                         row.top + (row.bottom - row.top - (combo.bottom - combo.top)) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            int left = 120;
            for (int id : {IDC_FIND_CASE, IDC_FIND_WORDS, IDC_FIND_REGEX}) {
                ui::position_dialog_control(a.find_bar, id, {left, 23, left + 18, 37});
                left += 20;
            }
            ui::position_dialog_control(a.find_bar, IDC_FIND_TEXT, {12, 6, width - 36, 18});
            ui::position_dialog_control(a.find_bar, IDC_FIND_CLOSE, {width - 30, 5, width - 12, 19});
            ui::position_dialog_control(a.find_bar, IDC_FIND_STATUS,
                                        {12, status_y, width - 50, status_y + 10});
            ui::position_dialog_control(a.find_bar, IDC_FIND_STOP,
                                        {width - 42, status_y - 1, width - 12, status_y + 13});
            ShowWindow(GetDlgItem(a.find_bar, IDC_FIND_STATUS), a.show_find_status ? SW_SHOW : SW_HIDE);
        }
        show_control(a.find_bar, a.find_open);
    }
    layout_search_results(a);
    const auto vp = viewport(a);
    const int midpoint = (vp.left + vp.right) / 2, center = (vp.top + vp.bottom) / 2;
    const bool welcome = !a.installed && !a.portable_choice && a.path.empty() && a.pages.empty();
    if (welcome) {
        position_control(a.welcome_install, midpoint - px(a, 188), center + px(a, 12), px(a, 180), px(a, 38));
        position_control(a.welcome_portable, midpoint + px(a, 8), center + px(a, 12), px(a, 180), px(a, 38));
    }
    show_control(a.welcome_install, welcome);
    show_control(a.welcome_portable, welcome);
    update_scroll(a, false);
    if (!a.resizing_sidebar)
        layout_toolbar(a);
    sync_toolbar(a);
    // Paint complete rows and scrollbars only after all pane bounds are final.
    // During a drag, queued mouse moves must not delay the child paints.
    RECT redraw = r;
    if (a.resizing_sidebar) {
        redraw.top = top;
        redraw.bottom = std::max(top, int(r.bottom) - px(a, 30));
    }
    RedrawWindow(a.window, &redraw, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    if (live_resize(a)) {
        // Finish the pane's client area and native scrollbars before the document paint.
        // Do not invalidate again: each child should paint only once for this layout.
        for (auto control : {a.bookmark_tree, a.sidebar, a.search_results, a.sidebar_tabs, a.results_summary})
            if (IsWindowVisible(control))
                RedrawWindow(control, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
        RedrawWindow(a.window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
}
static void resize_layout(App& a) {
    const double previous_scale = a.scale;
    // Fit the cached page before painting. Only PDF rasterization waits for the timer.
    update_scale(a);
    if (a.scale != previous_scale) {
        a.resize_render_pending = true;
        SetTimer(a.window, 1, 120, nullptr);
    }
    layout(a);
}
static void finish_resize(App& a) {
    KillTimer(a.window, 1);
    if (a.resize_render_pending)
        request_render(a);
    // Restore filtered painting even when the PDF scale did not change.
    RedrawWindow(a.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
static void set_sidebar_hover(App& a, int hover) {
    if (hover == a.sidebar_hover)
        return;
    int selected = int(SendMessageW(a.sidebar, LB_GETCURSEL, 0, 0));
    for (int index : {a.sidebar_hover, hover}) {
        RECT rect{};
        if (index >= 0 && index != selected &&
            SendMessageW(a.sidebar, LB_GETITEMRECT, WPARAM(index), LPARAM(&rect)) != LB_ERR)
            InvalidateRect(a.sidebar, &rect, FALSE);
    }
    a.sidebar_hover = hover;
}
static void rebuild_bookmarks(App& a) {
    a.syncing_bookmarks = true;
    SendMessageW(a.bookmark_tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(a.bookmark_tree);
    a.bookmark_items.clear();
    std::vector<HTREEITEM> parents;
    for (size_t i = 0; i < a.outline.size(); ++i) {
        const auto& entry = a.outline[i];
        size_t depth = std::min(size_t(std::max(0, entry.depth)), parents.size());
        parents.resize(depth);
        std::wstring title = entry.title;
        std::replace(title.begin(), title.end(), L'\t', L' ');
        TVINSERTSTRUCTW item{};
        item.hParent = parents.empty() ? TVI_ROOT : parents.back();
        item.hInsertAfter = TVI_LAST;
        item.item.mask = TVIF_TEXT | TVIF_PARAM;
        item.item.pszText = title.data();
        item.item.lParam = LPARAM(i);
        auto handle = TreeView_InsertItem(a.bookmark_tree, &item);
        a.bookmark_items.push_back(handle);
        parents.push_back(handle);
    }
    SendMessageW(a.bookmark_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(a.bookmark_tree, nullptr, FALSE);
    a.syncing_bookmarks = false;
}
static void update_sidebar(App& a, bool reset = false) {
    int selected = a.pages.empty() ? -1 : a.page;
    if (a.bookmarks) {
        // Search navigation must not move or expand the user's bookmark tree.
        if (a.find_open)
            return;
        auto current = TreeView_GetSelection(a.bookmark_tree);
        selected = -1;
        int best_page = -1;
        for (size_t i = 0; i < a.outline.size(); ++i) {
            int page = a.outline[i].page;
            if (page > best_page && page <= a.page) {
                best_page = page;
                selected = int(i);
            }
        }
        // Keep the user's choice when several bookmarks have the same destination.
        for (size_t i = 0; best_page >= 0 && i < a.bookmark_items.size(); ++i)
            if (a.bookmark_items[i] == current && a.outline[i].page == best_page)
                selected = int(i);
        HTREEITEM item = selected >= 0 ? a.bookmark_items[size_t(selected)] : nullptr;
        // Select a visible ancestor without opening a branch the user collapsed.
        for (auto parent = TreeView_GetParent(a.bookmark_tree, item); parent;
             parent = TreeView_GetParent(a.bookmark_tree, parent))
            if (!(TreeView_GetItemState(a.bookmark_tree, parent, TVIS_EXPANDED) & TVIS_EXPANDED))
                item = parent;
        a.syncing_bookmarks = true;
        if (current != item)
            TreeView_SelectItem(a.bookmark_tree, item);
        a.syncing_bookmarks = false;
        return;
    }
    int count = int(a.pages.size());
    if (reset || SendMessageW(a.sidebar, LB_GETCOUNT, 0, 0) != count) {
        // LB_SETCOUNT resets the selection and caret, even when the count is unchanged.
        SendMessageW(a.sidebar, LB_SETCOUNT, WPARAM(count), 0);
        a.sidebar_hover = -1;
        InvalidateRect(a.sidebar, nullptr, FALSE);
    }
    if (SendMessageW(a.sidebar, LB_GETCURSEL, 0, 0) != selected) {
        set_sidebar_hover(a, -1);
        SendMessageW(a.sidebar, LB_SETCURSEL, WPARAM(selected), 0);
    }
}
static void select_sidebar_tab(App& a, int index) {
    if (index < 0 || index > (a.find_open ? 2 : 1))
        return;
    a.show_results = index == 2;
    if (a.show_results)
        a.search_enter_pending = false;
    if (!a.show_results) {
        a.show_sidebar = true;
        a.bookmarks = index == 1;
        update_sidebar(a);
    }
    layout(a);
    request_render(a);
}
static void load_file(App& a, const std::wstring& path, const std::wstring& password = {}) {
    if (a.restore && (a.restore->document.empty() ||
                      std::filesystem::absolute(path) != std::filesystem::absolute(a.restore->document)))
        a.restore.reset();
    a.path = path;
    a.password = password;
    ++a.document;
    a.pages.clear();
    a.outline.clear();
    rebuild_bookmarks(a);
    a.bitmap.reset();
    a.selected.clear();
    a.find_open = false;
    cancel_search(a.search_worker);
    KillTimer(a.window, 3);
    ++a.search_ticket;
    clear_search_results(a);
    a.active_match = -1;
    a.search_running = a.search_complete = a.search_stopped = a.search_has_text = false;
    a.search_failed = false;
    a.show_results = a.reveal_match = false;
    a.search_notice.clear();
    SetWindowTextW(a.search, L"");
    a.query.clear();
    a.page = 0;
    a.rotation = 0;
    a.busy = true;
    a.status = L"Opening " + std::filesystem::path(path).filename().wstring() + L"…";
    SetWindowTextW(a.window, (std::filesystem::path(path).filename().wstring() + L" — FastPDF").c_str());
    layout(a);
    Request request;
    request.kind = Job::open;
    request.path = path;
    request.password = password;
    enqueue(a, std::move(request));
    InvalidateRect(a.window, nullptr, FALSE);
}
struct FileDialog {
    IFileOpenDialog* dialog = nullptr;
    IShellItem* item = nullptr;
    PWSTR path = nullptr;
    ~FileDialog() {
        CoTaskMemFree(path);
        release(item);
        release(dialog);
    }
};
static void choose_file(App& a) {
    ui::enable_system_dialog_theme();
    FileDialog file;
    hr(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&file.dialog)),
       "Create Open dialog");
    DWORD options = 0;
    hr(file.dialog->GetOptions(&options), "Get Open dialog options");
    hr(file.dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                               FOS_NOCHANGEDIR),
       "Set Open dialog options");
    const COMDLG_FILTERSPEC filters[] = {{L"PDF documents (*.pdf)", L"*.pdf"}, {L"All files", L"*.*"}};
    hr(file.dialog->SetFileTypes(UINT(std::size(filters)), filters), "Set Open dialog file types");
    const auto result = file.dialog->Show(a.window);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return;
    hr(result, "Show Open dialog");
    hr(file.dialog->GetResult(&file.item), "Get selected file");
    hr(file.item->GetDisplayName(SIGDN_FILESYSPATH, &file.path), "Get selected file path");
    load_file(a, file.path);
}
static void goto_page(App& a, int page) {
    if (a.pages.empty())
        return;
    page = std::clamp(page, 0, int(a.pages.size() - 1));
    if (page == a.page && a.bitmap)
        return;
    a.page = page;
    a.bitmap.reset();
    a.selected.clear();
    a.selected_all = false;
    update_sidebar(a);
    request_render(a, true);
}
static INT_PTR CALLBACK password_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        ui::style_dialog(window, 45);
        SendDlgItemMessageW(window, IDC_PASSWORD, EM_LIMITTEXT, 1024, 0);
        SetFocus(GetDlgItem(window, IDC_PASSWORD));
        return FALSE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            wchar_t pass[1025]{};
            GetDlgItemTextW(window, IDC_PASSWORD, pass, 1025);
            *reinterpret_cast<std::wstring*>(GetWindowLongPtrW(window, DWLP_USER)) = pass;
            SecureZeroMemory(pass, sizeof(pass));
            EndDialog(window, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
static void request_print_preview(App& a) {
    if (!a.print_preview || a.pages.empty())
        return;
    a.preview_bitmap.reset();
    a.preview_error.clear();
    const auto caption =
        L"Page " + std::to_wstring(a.preview_page + 1) + L" of " + std::to_wstring(a.pages.size());
    SetDlgItemTextW(a.print_preview, IDC_PREVIEW_PAGE, caption.c_str());
    EnableWindow(GetDlgItem(a.print_preview, IDC_PREVIEW_PREVIOUS), a.preview_page > 0);
    EnableWindow(GetDlgItem(a.print_preview, IDC_PREVIEW_NEXT), a.preview_page + 1 < int(a.pages.size()));
    RECT bounds{};
    GetClientRect(GetDlgItem(a.print_preview, IDC_PREVIEW_IMAGE), &bounds);
    auto page = a.pages[size_t(a.preview_page)];
    if (page.rotation % 180)
        std::swap(page.width, page.height);
    Request request;
    request.kind = Job::preview;
    request.page = a.preview_page;
    // Printing uses the PDF's rotation, not the reader's temporary view rotation.
    request.scale = std::min(
        {std::max(1L, bounds.right - 24) / page.width, std::max(1L, bounds.bottom - 24) / page.height, 2.0});
    enqueue(a, std::move(request));
    InvalidateRect(GetDlgItem(a.print_preview, IDC_PREVIEW_IMAGE), nullptr, TRUE);
}
static void paint_print_preview(App& a, const DRAWITEMSTRUCT& draw) {
    const int saved = SaveDC(draw.hDC);
    const auto colors = ui::system_dialog_colors();
    SetDCBrushColor(draw.hDC, colors.dark ? colors.field : colors.footer);
    FillRect(draw.hDC, &draw.rcItem, HBRUSH(GetStockObject(DC_BRUSH)));
    if (a.preview_bitmap) {
        const auto& image = *a.preview_bitmap;
        RECT bounds = draw.rcItem;
        InflateRect(&bounds, -12, -12);
        const double scale = std::min(double(std::max(1L, bounds.right - bounds.left)) / image.width,
                                      double(std::max(1L, bounds.bottom - bounds.top)) / image.height);
        const int width = std::max(1, int(image.width * scale));
        const int height = std::max(1, int(image.height * scale));
        BITMAPINFO info{};
        info.bmiHeader = {
            sizeof(BITMAPINFOHEADER), LONG(image.width), -LONG(image.height), 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        SetStretchBltMode(draw.hDC, HALFTONE);
        SetBrushOrgEx(draw.hDC, 0, 0, nullptr);
        StretchDIBits(draw.hDC, (bounds.left + bounds.right - width) / 2,
                      (bounds.top + bounds.bottom - height) / 2, width, height, 0, 0, int(image.width),
                      int(image.height), image.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        RECT bounds = draw.rcItem;
        InflateRect(&bounds, -12, -12);
        label(draw.hDC, bounds, a.preview_error.empty() ? L"Loading preview…" : a.preview_error,
              ui::system_dialog_colors().muted, HFONT(SendMessageW(a.print_preview, WM_GETFONT, 0, 0)),
              DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    }
    RestoreDC(draw.hDC, saved);
}
static INT_PTR CALLBACK print_preview_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto a = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        a = reinterpret_cast<App*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        a->print_preview = window;
        ui::style_dialog(window, -1, 0, false);
        // The final panel can match the template size, so WM_SIZE is not guaranteed.
        PostMessageW(window, WM_SIZE, 0, 0);
        return TRUE;
    }
    if (!a)
        return FALSE;
    switch (message) {
    case WM_SIZE: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        RECT units{0, 0, 4, 8};
        MapDialogRect(window, &units);
        const int width = MulDiv(bounds.right, 4, units.right);
        const int footer = MulDiv(bounds.bottom, 8, units.bottom) - 26;
        ui::dialog_footer(window, footer);
        ui::position_dialog_control(window, IDC_PREVIEW_TITLE, {12, 9, width - 12, 21});
        ui::position_dialog_control(window, IDC_PREVIEW_IMAGE, {12, 25, width - 12, footer - 9});
        ui::position_dialog_control(window, IDC_PREVIEW_PREVIOUS, {12, footer + 6, 62, footer + 20});
        ui::position_dialog_control(window, IDC_PREVIEW_NEXT,
                                    {width - 62, footer + 6, width - 12, footer + 20});
        ui::position_dialog_control(window, IDC_PREVIEW_PAGE, {68, footer + 6, width - 68, footer + 20});
        PostMessageW(window, WM_APP, 0, 0);
        return TRUE;
    }
    case WM_APP:
        request_print_preview(*a);
        return TRUE;
    case WM_DRAWITEM:
        if (wp == IDC_PREVIEW_IMAGE) {
            paint_print_preview(*a, *reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
            return TRUE;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PREVIEW_PREVIOUS || LOWORD(wp) == IDC_PREVIEW_NEXT) {
            const int page = std::clamp(a->preview_page + (LOWORD(wp) == IDC_PREVIEW_NEXT ? 1 : -1), 0,
                                        int(a->pages.size()) - 1);
            if (page != a->preview_page) {
                a->preview_page = page;
                request_print_preview(*a);
            }
            return TRUE;
        }
        break;
    case WM_NCDESTROY: {
        a->print_preview = nullptr;
        a->preview_bitmap.reset();
        a->preview_error.clear();
        std::lock_guard lock(a->worker.mutex);
        std::erase_if(a->worker.jobs, [](const auto& job) { return job.kind == Job::preview; });
        break;
    }
    }
    return FALSE;
}
static UINT_PTR CALLBACK print_dialog_hook(HWND window, UINT message, WPARAM, LPARAM lp) {
    if (message != WM_INITDIALOG)
        return FALSE;
    const auto dialog = reinterpret_cast<PRINTDLGW*>(lp);
    auto& a = *reinterpret_cast<App*>(dialog->lCustData);
    ui::style_common_dialog(window);
    a.preview_page = a.page;
    const auto pane = CreateDialogParamW(a.instance, MAKEINTRESOURCEW(IDD_PRINT_PREVIEW), window,
                                         print_preview_dialog, LPARAM(&a));
    if (!pane)
        return FALSE;
    RECT bounds{}, client_bounds{}, pane_bounds{};
    GetWindowRect(window, &bounds);
    GetClientRect(window, &client_bounds);
    GetWindowRect(pane, &pane_bounds);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    const int frame_width = bounds.right - bounds.left - client_bounds.right;
    const int frame_height = bounds.bottom - bounds.top - client_bounds.bottom;
    const int pane_width = pane_bounds.right - pane_bounds.left;
    const int pane_height = pane_bounds.bottom - pane_bounds.top;
    const bool beside =
        client_bounds.right + frame_width + pane_width <= monitor.rcWork.right - monitor.rcWork.left;
    const int width =
        beside ? client_bounds.right + pane_width : std::max<int>(client_bounds.right, pane_width);
    const int height = std::min(beside ? std::max(client_bounds.bottom, LONG(pane_height))
                                       : client_bounds.bottom + pane_height,
                                monitor.rcWork.bottom - monitor.rcWork.top - frame_height);
    const int x = std::clamp(bounds.left - (width - client_bounds.right) / 2, monitor.rcWork.left,
                             std::max(monitor.rcWork.left, monitor.rcWork.right - width - frame_width));
    const int y = std::clamp(bounds.top, monitor.rcWork.top,
                             std::max(monitor.rcWork.top, monitor.rcWork.bottom - height - frame_height));
    SetWindowPos(window, nullptr, x, y, width + frame_width, height + frame_height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    MoveWindow(pane, beside ? client_bounds.right : 0, beside ? 0 : client_bounds.bottom,
               beside ? pane_width : width, beside ? height : height - client_bounds.bottom, TRUE);
    if (beside) {
        RECT units{0, 0, 4, 8}, native_units{0, 0, 4, 8};
        MapDialogRect(pane, &units);
        MapDialogRect(window, &native_units);
        const int footer = height - MulDiv(26, units.bottom, 8);
        ui::dialog_footer(window, MulDiv(footer, 8, native_units.bottom));
        const int right = client_bounds.right - MulDiv(12, units.right, 4);
        const int y_button = footer + MulDiv(6, units.bottom, 8);
        const int button_height = MulDiv(14, units.bottom, 8);
        const int cancel_width = MulDiv(44, units.right, 4);
        const int primary_width = MulDiv(50, units.right, 4);
        MoveWindow(GetDlgItem(window, IDCANCEL), right - cancel_width, y_button, cancel_width, button_height,
                   TRUE);
        MoveWindow(GetDlgItem(window, IDOK), right - cancel_width - MulDiv(6, units.right, 4) - primary_width,
                   y_button, primary_width, button_height, TRUE);
    }
    return FALSE;
}
static INT_PTR CALLBACK licenses_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        ui::style_dialog(window, 302);
        const auto instance = reinterpret_cast<App*>(lp)->instance;
        std::wstring notices;
        for (int id : {IDR_FONT_LICENSE, IDR_UNICODE_LICENSE}) {
            const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
            const auto loaded = resource ? LoadResource(instance, resource) : nullptr;
            const auto bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
            if (!bytes) {
                notices += L"Unable to load embedded notices.\r\n";
                continue;
            }
            const auto text = utf8(std::string_view(bytes, SizeofResource(instance, resource)));
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r'))
                    notices += L'\r';
                notices += text[i];
            }
            notices += L"\r\n\r\n";
        }
        SetDlgItemTextW(window, IDC_LICENSES, notices.c_str());
        SetFocus(GetDlgItem(window, IDOK));
        return FALSE;
    }
    if (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(window, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}
static INT_PTR CALLBACK about_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        ui::style_dialog(window, 43);
        SetDlgItemTextW(window, IDC_ABOUT_VERSION, L"FastPDF " FASTPDF_VERSION_W);
        LITEM link{};
        link.mask = LIF_ITEMINDEX | LIF_STATE;
        link.state = link.stateMask = LIS_DEFAULTCOLORS;
        SendDlgItemMessageW(window, IDC_REPOSITORY, LM_SETITEM, 0, reinterpret_cast<LPARAM>(&link));
        SetFocus(GetDlgItem(window, IDOK));
        return FALSE;
    }
    if (message == WM_NOTIFY) {
        const auto notification = reinterpret_cast<const NMHDR*>(lp);
        if (notification->idFrom == IDC_REPOSITORY &&
            (notification->code == NM_CLICK || notification->code == NM_RETURN)) {
            ShellExecuteW(window, L"open", L"https://github.com/tuncb/fastpdf", nullptr, nullptr,
                          SW_SHOWNORMAL);
            return TRUE;
        }
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDC_NOTICES) {
            const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
            DialogBoxParamW(app->instance, MAKEINTRESOURCEW(IDD_LICENSES), window, licenses_dialog,
                            reinterpret_cast<LPARAM>(app));
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(window, LOWORD(wp));
            return TRUE;
        }
    }
    return FALSE;
}
static INT_PTR CALLBACK goto_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        auto a = reinterpret_cast<App*>(lp);
        ui::style_dialog(window, 45);
        SetDlgItemInt(window, IDC_PAGE, UINT(a->page + 1), FALSE);
        SendDlgItemMessageW(window, IDC_PAGE, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDOK) {
            auto a = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
            BOOL valid = FALSE;
            UINT page = GetDlgItemInt(window, IDC_PAGE, &valid, FALSE);
            if (valid && page > 0 && page <= a->pages.size()) {
                goto_page(*a, int(page - 1));
                EndDialog(window, IDOK);
            } else
                MessageBeep(MB_ICONWARNING);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
    }
    return FALSE;
}
static std::wstring search_query(App& a) {
    wchar_t text[4096]{};
    GetWindowTextW(a.search, text, int(std::size(text)));
    std::wstring value = text;
    if (a.search_options.regex)
        return value;
    const auto first = value.find_first_not_of(L" \t\r\n");
    return first == std::wstring::npos ? L""
                                       : value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}
static void sync_search(App& a) {
    if (!a.find_bar)
        return;
    std::wstring count, status;
    if (a.query.empty())
        status = L"Enter text to find.";
    else if (a.search_failed)
        count = a.search_notice.starts_with(L"Invalid regex:") ? L"Invalid regex" : L"Search error";
    else if (a.search_running) {
        count = std::to_wstring(a.matches.count) + L" found";
        status = L"Searching… " + std::to_wstring(a.search_checked) + L" of " +
                 std::to_wstring(a.search_total) + L" pages checked";
    } else if (a.search_stopped) {
        count = std::to_wstring(a.matches.count) + L" found";
        status = L"Search stopped. " + std::to_wstring(a.search_checked) + L" of " +
                 std::to_wstring(a.search_total) + L" pages checked";
    } else if (a.matches.count != 0) {
        count = std::to_wstring(a.active_match + 1) + L" of " + std::to_wstring(a.matches.count);
        const size_t pages = a.matches.batches.size();
        status = std::to_wstring(a.matches.count) + (a.matches.count == 1 ? L" match on " : L" matches on ") +
                 std::to_wstring(pages) + (pages == 1 ? L" page" : L" pages");
    } else if (a.search_complete && !a.search_has_text) {
        count = L"No text";
        status = L"No searchable text detected. These pages may contain scanned images.";
    } else if (a.search_complete) {
        count = L"0 matches";
        status = L"No matches. Check the spelling or change the search options.";
    }
    a.search_summary = status;
    if (!a.search_notice.empty()) {
        if (!a.search_summary.empty())
            a.search_summary += L"\n";
        a.search_summary += a.search_notice;
        status = a.search_notice;
    }
    // Fast searches must not briefly grow the bar and rescale the document.
    const bool show_progress = a.search_running && GetTickCount64() >= a.search_progress_after;
    if (!a.search_running)
        KillTimer(a.window, search_progress_timer);
    a.show_find_status = show_progress || a.search_stopped || a.search_failed || !a.search_notice.empty();
    SetDlgItemTextW(a.find_bar, IDC_FIND_COUNT, count.c_str());
    SetDlgItemTextW(a.find_bar, IDC_FIND_STATUS, a.show_find_status ? status.c_str() : L"");
    SetWindowTextW(a.results_summary, a.search_summary.c_str());
    CheckDlgButton(a.find_bar, IDC_FIND_CASE, a.search_options.match_case ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(a.find_bar, IDC_FIND_WORDS, a.search_options.whole_words ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(a.find_bar, IDC_FIND_REGEX, a.search_options.regex ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(a.find_bar, IDC_FIND_PREVIOUS), a.matches.count != 0);
    EnableWindow(GetDlgItem(a.find_bar, IDC_FIND_NEXT), a.matches.count != 0);
    ShowWindow(GetDlgItem(a.find_bar, IDC_FIND_STOP),
               a.search_running && a.show_find_status ? SW_SHOW : SW_HIDE);
    const bool resized = a.find_layout_height != find_height(a);
    if (resized) {
        layout(a);
        request_render(a);
    } else if (a.summary_layout_height != search_summary_height(a)) {
        // A wrapped summary changes only the Results pane, not the find bar or PDF.
        layout_search_results(a);
        if (results_visible(a)) {
            RECT redraw{0, px(a, toolbar_height) + a.find_layout_height + a.sidebar_header_height,
                        sidebar_splitter(a).left, client(a).bottom - px(a, 30)};
            RedrawWindow(a.window, &redraw, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        }
    }
    const int selected = ListView_GetNextItem(a.search_results, -1, LVNI_SELECTED);
    if (selected != a.active_match) {
        a.syncing_results = true;
        if (selected >= 0)
            ListView_SetItemState(a.search_results, selected, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (a.active_match >= 0) {
            ListView_SetItemState(a.search_results, a.active_match, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(a.search_results, a.active_match, FALSE);
        }
        a.syncing_results = false;
    }
    fit_search_results(a.search_results);
    const auto bounds = document_bounds(a);
    InvalidateRect(a.window, &bounds, FALSE);
}
static void start_search(App& a) {
    KillTimer(a.window, 3);
    cancel_search(a.search_worker);
    ++a.search_ticket;
    a.query = search_query(a);
    if (a.search_options.page >= 0)
        a.search_options.page = a.page;
    clear_search_results(a);
    a.active_match = -1;
    a.search_checked = 0;
    a.search_start_page = a.page;
    a.search_total = a.search_options.page < 0 ? int(a.pages.size()) : 1;
    a.search_running = !a.query.empty() && !a.pages.empty();
    a.search_complete = a.search_stopped = a.search_has_text = a.reveal_match = false;
    a.search_failed = false;
    a.search_enter_pending = true;
    a.search_notice.clear();
    if (a.search_running) {
        a.search_progress_after = GetTickCount64() + search_progress_delay;
        SetTimer(a.window, search_progress_timer, search_progress_delay, nullptr);
        submit_search(a.search_worker,
                      {a.document, a.search_ticket, a.path, a.password, a.query, a.search_options});
    }
    sync_search(a);
}
static void select_match(App& a, int index) {
    if (index < 0 || size_t(index) >= a.matches.count)
        return;
    a.active_match = index;
    a.reveal_match = true;
    const int page = search_result(a.matches, size_t(index)).page;
    if (a.page != page || !a.bitmap)
        goto_page(a, page);
    else
        reveal_search_match(a);
    sync_search(a);
}
static void find(App& a, int direction) {
    if (search_query(a) != a.query || (!a.search_running && !a.search_complete && !a.search_stopped)) {
        start_search(a);
        return;
    }
    if (a.matches.count == 0)
        return;
    a.search_enter_pending = false;
    int next = a.active_match + direction;
    a.search_notice.clear();
    if (next < 0 || size_t(next) >= a.matches.count) {
        if (a.search_running || a.search_stopped) {
            a.search_notice = a.search_running ? L"Searching for more matches…"
                                               : L"Search stopped. Press Enter to search again.";
            if (a.search_stopped) {
                start_search(a);
                return;
            }
            sync_search(a);
            return;
        }
        a.search_notice = next < 0 ? L"Continued from the end of the document."
                                   : L"Continued from the start of the document.";
        next = next < 0 ? int(a.matches.count) - 1 : 0;
    }
    select_match(a, next);
}
static void submit_find(App& a, int direction) {
    if (search_query(a) != a.query || (a.search_stopped && !a.search_failed) ||
        (!a.search_running && !a.search_complete && !a.search_stopped))
        start_search(a);
    const bool reveal = a.search_enter_pending || !results_visible(a);
    if (!results_visible(a))
        select_sidebar_tab(a, 2);
    a.search_enter_pending = false;
    // The first Enter submits the query or reveals a hidden result, without advancing it.
    if (reveal) {
        select_match(a, a.active_match);
        if (a.active_match >= 0)
            ListView_EnsureVisible(a.search_results, a.active_match, FALSE);
        return;
    }
    find(a, direction);
}
static void receive_search(App& a) {
    KillTimer(a.window, 4);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    int select = -1;
    bool changed = false;
    // Transfer complete page allocations. Even a page with many matches takes
    // constant time here; no row strings or match records are copied into the UI.
    for (int pages = 0; a.find_open && pages < 64; ++pages) {
        SearchUpdate update;
        if (!take_search_update(a.search_worker, a.document, a.search_ticket, update))
            break;
        changed = true;
        a.search_checked = update.checked;
        a.search_total = update.total;
        a.search_has_text = a.search_has_text || update.has_text;
        a.search_complete = update.complete;
        a.search_running = !update.complete;
        if (!update.error.empty()) {
            a.search_notice = update.error;
            a.search_failed = true;
            a.search_complete = false;
            a.search_stopped = true;
            cancel_search(a.search_worker);
            clear_search_results(a);
            a.active_match = select = -1;
            break;
        }
        if (!update.matches.empty()) {
            if (a.active_match < 0 && select < 0 && update.matches.front().page >= a.search_start_page)
                select = int(a.matches.count);
            append_search_results(a.matches, std::move(update.matches));
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }
    if (finish_search_updates(a.search_worker))
        SetTimer(a.window, 4, 16, nullptr);
    if (!changed)
        return;
    if (ListView_GetItemCount(a.search_results) != int(a.matches.count))
        ListView_SetItemCountEx(a.search_results, int(a.matches.count),
                                LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    fit_search_results(a.search_results);
    InvalidateRect(a.search_results, nullptr, FALSE);
    if (a.active_match < 0 && select < 0 && (a.search_complete || a.search_stopped) && a.matches.count != 0)
        select = 0;
    if (select >= 0)
        select_match(a, select);
    else
        sync_search(a);
}
static INT_PTR CALLBACK find_bar_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto a = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        ui::style_dialog(window, -1, 0, false);
        ui::compact_button(GetDlgItem(window, IDC_FIND_CASE), L"Aa");
        ui::compact_button(GetDlgItem(window, IDC_FIND_WORDS), L"ab");
        ui::compact_button(GetDlgItem(window, IDC_FIND_REGEX), L".*");
        ui::icon_button(GetDlgItem(window, IDC_FIND_PREVIOUS), ToolbarIcon::up);
        ui::icon_button(GetDlgItem(window, IDC_FIND_NEXT), ToolbarIcon::down);
        ui::icon_button(GetDlgItem(window, IDC_FIND_CLOSE), ToolbarIcon::close);
        SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_ADDSTRING, 0, LPARAM(L"Entire document"));
        SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_ADDSTRING, 0, LPARAM(L"Current page"));
        SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_SETCURSEL, 0, 0);
        ShowWindow(GetDlgItem(window, IDC_FIND_STATUS), SW_HIDE);
        ShowWindow(GetDlgItem(window, IDC_FIND_STOP), SW_HIDE);
        return TRUE;
    }
    if (!a)
        return FALSE;
    if (message == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case IDC_FIND_TEXT:
            if (HIWORD(wp) == EN_CHANGE && a->find_open) {
                cancel_search(a->search_worker);
                ++a->search_ticket;
                clear_search_results(*a);
                a->active_match = -1;
                a->search_running = a->search_complete = a->search_stopped = false;
                a->search_failed = false;
                a->search_enter_pending = true;
                a->search_notice = L"";
                SetTimer(a->window, 3, 180, nullptr);
                sync_search(*a);
            }
            return TRUE;
        case IDC_FIND_CASE:
        case IDC_FIND_WORDS:
        case IDC_FIND_REGEX:
            a->search_options.match_case = IsDlgButtonChecked(window, IDC_FIND_CASE) == BST_CHECKED;
            a->search_options.whole_words = IsDlgButtonChecked(window, IDC_FIND_WORDS) == BST_CHECKED;
            a->search_options.regex = IsDlgButtonChecked(window, IDC_FIND_REGEX) == BST_CHECKED;
            start_search(*a);
            return TRUE;
        case IDC_FIND_PREVIOUS:
            find(*a, -1);
            return TRUE;
        case IDC_FIND_NEXT:
            find(*a);
            return TRUE;
        case IDC_FIND_SCOPE:
            if (HIWORD(wp) == CBN_SELENDCANCEL) {
                SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_SETCURSEL, a->search_options.page < 0 ? 0 : 1,
                                    0);
                return TRUE;
            }
            if (HIWORD(wp) == CBN_SELENDOK ||
                (HIWORD(wp) == CBN_SELCHANGE &&
                 !SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_GETDROPPEDSTATE, 0, 0))) {
                const bool current_page =
                    SendDlgItemMessageW(window, IDC_FIND_SCOPE, CB_GETCURSEL, 0, 0) == 1;
                if (current_page == (a->search_options.page >= 0))
                    return TRUE;
                a->search_options.page = current_page ? a->page : -1;
                start_search(*a);
            }
            return TRUE;
        case IDC_FIND_STOP:
            KillTimer(a->window, 4);
            cancel_search(a->search_worker);
            ++a->search_ticket;
            a->search_running = false;
            a->search_stopped = true;
            sync_search(*a);
            return TRUE;
        case IDC_FIND_CLOSE:
            command(*a, cmd_search);
            return TRUE;
        }
    }
    return FALSE;
}
static void copy(App& a) {
    if (!a.bitmap)
        return;
    std::wstring text;
    if (a.selected_all || a.selected.empty())
        text = page_text(*a.bitmap);
    else {
        RenderedPage selected;
        for (auto i : a.selected)
            if (i < a.bitmap->text.size())
                selected.text.push_back(a.bitmap->text[i]);
        text = page_text(selected);
    }
    if (text.empty() || !OpenClipboard(a.window))
        return;
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block) {
        if (auto ptr = GlobalLock(block)) {
            std::memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(block);
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, block))
                GlobalFree(block);
        } else
            GlobalFree(block);
    }
    CloseClipboard();
    a.status = L"Copied text to clipboard";
    InvalidateRect(a.window, nullptr, FALSE);
}

static deployment::Session current_session(const App& a) {
    return {a.path.empty() ? L"" : std::filesystem::absolute(a.path).wstring(), a.page, a.rotation,
            int(a.fit), a.zoom};
}
static int task_dialog(App& a, const wchar_t* title, const std::wstring& content,
                       const std::vector<TASKDIALOG_BUTTON>& buttons, const wchar_t* checkbox = nullptr,
                       BOOL* checked = nullptr) {
    auto choices = buttons;
    choices.push_back({IDCANCEL, L"Cancel"});
    return ui::show_prompt(a.window, title, content, choices, IDCANCEL, checkbox, checked);
}
static void installation_dialog(App& a) {
    if (a.update_job.valid() || !a.pending_update.directory.empty())
        throw std::runtime_error("Finish the current update before changing the installation.");
    const bool present = deployment::installation_present();
    ui::InstallationOptions options{deployment::install_directory().wstring(), a.installed, present,
                                    !present || deployment::shortcut_present(),
                                    present && deployment::default_pdf_reader()};
    const auto result = ui::show_installation_dialog(a.instance, a.window, options);
    if (result == ui::InstallationAction::apply) {
        deployment::install(options.shortcut);
        if (!a.installed) {
            deployment::open_installed(current_session(a));
            PostMessageW(a.window, WM_CLOSE, 0, 0);
        }
        if (options.default_pdf && !deployment::default_pdf_reader())
            deployment::open_pdf_defaults(a.window);
        else if (a.installed)
            ui::message_box(a.window, L"Installation options saved.", L"FastPDF", MB_OK | MB_ICONINFORMATION);
    } else if (result == ui::InstallationAction::open) {
        deployment::open_installed(current_session(a));
        PostMessageW(a.window, WM_CLOSE, 0, 0);
    } else if (result == ui::InstallationAction::uninstall) {
        if (ui::message_box(a.window,
                            L"Uninstall FastPDF for your Windows account? Your PDF files will be kept.",
                            L"Uninstall FastPDF", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            deployment::start_uninstall();
            PostMessageW(a.window, WM_CLOSE, 0, 0);
        }
    }
}
static void ready_update(App& a) {
    const int result = task_dialog(
        a, L"Your Update Is Ready",
        L"FastPDF " + utf8(a.pending_update.version) +
            L" has been downloaded and verified.\n\nClose other FastPDF windows before installing. "
            L"Restarting will reopen this document at the same page. Protected PDFs may ask for their "
            L"password again.",
        {{100, L"Restart now"}, {101, L"Install when I close FastPDF"}, {102, L"Later"}});
    if (result == 100) {
        if (a.busy)
            throw std::runtime_error("Wait for the current render, print, or export to finish, then choose "
                                     "Check for updates again.");
        deployment::start_update(a.pending_update, current_session(a), true);
        a.pending_update = {};
        a.update_on_exit = false;
        PostMessageW(a.window, WM_CLOSE, 0, 0);
    } else if (result == 101) {
        deployment::require_no_other_instances();
        a.update_on_exit = true;
        a.update_status = L"Update will install when you close FastPDF.";
    } else {
        a.update_on_exit = false;
        a.update_status = L"Update ready. Help → Check for updates to install.";
    }
}
static void start_update_job(App& a, std::optional<deployment::Release> release = {}) {
    a.update_cancel = false;
    a.update_status = release ? L"Downloading update…" : L"Checking for updates…";
    a.update_job = std::async(std::launch::async, [&a, release] {
        UpdateResult result;
        result.downloading = release.has_value();
        try {
            if (release)
                result.download = deployment::download_release(*release, a.update_cancel);
            else
                result.release = deployment::check_release(a.update_cancel);
        } catch (const std::exception& error) {
            result.error = utf8(error.what());
        }
        return result;
    });
    SetTimer(a.window, 2, 200, nullptr);
}
static void poll_update(App& a) {
    if (!a.update_job.valid() || a.update_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    KillTimer(a.window, 2);
    auto result = a.update_job.get();
    a.update_status.clear();
    if (a.update_cancel) {
        deployment::cleanup_download(result.download);
        a.update_status = L"Update canceled.";
    } else if (!result.error.empty()) {
        ui::message_box(a.window, result.error.c_str(), L"FastPDF Update", MB_OK | MB_ICONERROR);
    } else if (result.downloading) {
        a.pending_update = std::move(result.download);
        a.update_status = L"Update ready. Help → Check for updates to install.";
        ready_update(a);
    } else if (result.release.version <= deployment::parse_version(FASTPDF_VERSION)) {
        ui::message_box(a.window, L"FastPDF " FASTPDF_VERSION_W L" is up to date.", L"FastPDF Update",
                        MB_OK | MB_ICONINFORMATION);
    } else if (task_dialog(a, L"A FastPDF Update Is Available",
                           L"Version " + utf8(result.release.tag) + L" is available. Download it now?",
                           {{100, L"Download update"}}) == 100) {
        start_update_job(a, result.release);
    }
    InvalidateRect(a.window, nullptr, FALSE);
}
static void command(App& a, int id) {
    if (a.pages.empty() && id >= cmd_prev && id <= cmd_goto)
        return;
    if (id == cmd_exit)
        PostMessageW(a.window, WM_CLOSE, 0, 0);
    else if (id == cmd_install)
        installation_dialog(a);
    else if (id == cmd_portable) {
        a.portable_choice = true;
        SetFocus(a.window);
        layout(a);
    } else if (id == cmd_updates) {
        if (!a.installed) {
            ui::message_box(
                a.window,
                L"Install FastPDF to update it here. You can also download a new portable copy from "
                L"GitHub Releases.",
                L"FastPDF Updates", MB_OK | MB_ICONINFORMATION);
            installation_dialog(a);
        } else if (!a.pending_update.directory.empty())
            ready_update(a);
        else if (!a.update_job.valid())
            start_update_job(a);
    } else if (id == cmd_open)
        choose_file(a);
    else if (id == cmd_prev)
        goto_page(a, a.page - 1);
    else if (id == cmd_next)
        goto_page(a, a.page + 1);
    else if (id == cmd_zoom_out || id == cmd_zoom_in) {
        a.fit = Fit::custom;
        a.zoom = std::clamp(a.scale * 72 / a.dpi * (id == cmd_zoom_in ? 1.2 : 1 / 1.2), 0.05, 8.0);
        request_render(a);
    } else if (id == cmd_fit || id == cmd_width) {
        a.fit = id == cmd_fit ? Fit::page : Fit::width;
        request_render(a, true);
    } else if (id == cmd_rotate) {
        a.rotation = (a.rotation + 90) % 360;
        a.bitmap.reset();
        request_render(a, true);
    } else if (id == cmd_sidebar || id == cmd_bookmarks) {
        bool bookmarks = id == cmd_bookmarks;
        a.show_sidebar = results_visible(a) || !(a.show_sidebar && a.bookmarks == bookmarks);
        a.show_results = false;
        a.bookmarks = bookmarks;
        update_sidebar(a, true);
        layout(a);
        request_render(a);
    } else if (id == cmd_search) {
        if (!a.find_open) {
            a.sidebar_before_find = a.show_sidebar;
            a.bookmarks_before_find = a.bookmarks;
        }
        a.find_open = !a.find_open;
        if (!a.find_open) {
            a.show_results = false;
            a.show_sidebar = a.sidebar_before_find;
            a.bookmarks = a.bookmarks_before_find;
        }
        layout(a);
        if (a.find_open) {
            SetFocus(a.search);
            SendMessageW(a.search, EM_SETSEL, 0, -1);
            start_search(a);
        } else {
            KillTimer(a.window, 3);
            cancel_search(a.search_worker);
            ++a.search_ticket;
            a.search_running = false;
            clear_search_results(a);
            a.active_match = -1;
            a.reveal_match = false;
            SetFocus(a.window);
            a.query.clear();
        }
        request_render(a);
    } else if (id == cmd_goto && !a.pages.empty())
        DialogBoxParamW(a.instance, MAKEINTRESOURCEW(IDD_GOTO), a.window, goto_dialog, LPARAM(&a));
    else if (id == cmd_print && !a.pages.empty()) {
        ui::enable_system_dialog_theme();
        PRINTDLGW dialog{sizeof(dialog)};
        dialog.hwndOwner = a.window;
        dialog.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION | PD_ENABLEPRINTHOOK;
        dialog.lpfnPrintHook = print_dialog_hook;
        dialog.lCustData = LPARAM(&a);
        dialog.nMinPage = 1;
        dialog.nMaxPage = WORD(std::min<size_t>(a.pages.size(), 65535));
        dialog.nFromPage = dialog.nToPage = WORD(a.page + 1);
        dialog.nCopies = 1;
        if (PrintDlgW(&dialog)) {
            auto job = std::shared_ptr<PrintJob>(new PrintJob{}, destroy_print_job);
            job->dc = dialog.hDC;
            job->first = (dialog.Flags & PD_PAGENUMS) ? dialog.nFromPage - 1 : 0;
            job->last = (dialog.Flags & PD_PAGENUMS) ? dialog.nToPage - 1 : int(a.pages.size() - 1);
            Request request;
            request.kind = Job::print;
            request.print = job;
            enqueue(a, std::move(request));
            a.status = L"Printing…";
            a.busy = true;
        }
        if (dialog.hDevMode)
            GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames)
            GlobalFree(dialog.hDevNames);
    } else if (id == cmd_about) {
        DialogBoxParamW(a.instance, MAKEINTRESOURCEW(IDD_ABOUT), a.window, about_dialog,
                        reinterpret_cast<LPARAM>(&a));
    }
    sync_toolbar(a);
    InvalidateRect(a.window, nullptr, FALSE);
}
static void fill_rect(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
static void label(HDC dc, RECT rect, std::wstring_view text, COLORREF color, HFONT font,
                  UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.data(), int(text.size()), &rect, flags);
    SelectObject(dc, old);
}
static void paint_sidebar_tabs(App& a, HDC dc) {
    RECT bounds{};
    GetClientRect(a.sidebar_tabs, &bounds);
    const auto& colors = a.search_colors;
    fill_rect(dc, bounds, colors.footer);
    fill_rect(dc, {0, bounds.bottom - 1, bounds.right, bounds.bottom}, colors.border);
    const int selected = TabCtrl_GetCurSel(a.sidebar_tabs);
    for (int i = 0; i < TabCtrl_GetItemCount(a.sidebar_tabs); ++i) {
        RECT tab{};
        TabCtrl_GetItemRect(a.sidebar_tabs, i, &tab);
        wchar_t title[32]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = title;
        item.cchTextMax = int(std::size(title));
        TabCtrl_GetItem(a.sidebar_tabs, i, &item);
        if (i == selected || i == a.tab_hover)
            fill_rect(dc, tab, i == selected ? colors.body : colors.hover);
        label(dc, tab, title, i == selected ? colors.text : colors.muted, a.search_font,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (i == selected) {
            fill_rect(dc, {tab.left + px(a, 10), tab.bottom - px(a, 2), tab.right - px(a, 10), tab.bottom},
                      colors.accent);
            if (GetFocus() == a.sidebar_tabs &&
                !(SendMessageW(a.sidebar_tabs, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                InflateRect(&tab, -px(a, 4), -px(a, 4));
                DrawFocusRect(dc, &tab);
            }
        }
    }
}

static std::wstring page_number(const App& a) {
    return a.pages.empty() ? L"—" : std::to_wstring(a.page + 1);
}
static std::wstring zoom_percent(const App& a) {
    return a.pages.empty() ? L"—" : std::to_wstring(int(std::lround(a.scale * 72 / a.dpi * 100))) + L"%";
}
static void sync_entry(HWND window, const std::wstring& value) {
    if (GetFocus() == window)
        return;
    wchar_t current[32]{};
    GetWindowTextW(window, current, int(std::size(current)));
    if (value != current)
        SetWindowTextW(window, value.c_str());
}
static void sync_toolbar(App& a) {
    if (!a.toolbar)
        return;
    bool loaded = !a.pages.empty();
    for (const auto& tool : toolbar_tools) {
        bool enabled = loaded || tool.command == cmd_open;
        if (tool.command == cmd_prev)
            enabled = loaded && a.page > 0;
        if (tool.command == cmd_next)
            enabled = loaded && a.page + 1 < int(a.pages.size());
        if ((SendMessageW(a.toolbar, TB_ISBUTTONENABLED, WPARAM(tool.command), 0) != 0) != enabled)
            SendMessageW(a.toolbar, TB_ENABLEBUTTON, WPARAM(tool.command), MAKELPARAM(enabled, 0));
    }
    for (auto [id, checked] :
         {std::pair{cmd_fit, loaded && a.fit == Fit::page},
          {cmd_width, loaded && a.fit == Fit::width},
          {cmd_search, loaded && a.find_open},
          {cmd_sidebar, loaded && a.show_sidebar && !results_visible(a) && !a.bookmarks},
          {cmd_bookmarks, loaded && a.show_sidebar && !results_visible(a) && a.bookmarks}}) {
        if ((SendMessageW(a.toolbar, TB_ISBUTTONCHECKED, WPARAM(id), 0) != 0) != checked)
            SendMessageW(a.toolbar, TB_CHECKBUTTON, WPARAM(id), MAKELPARAM(checked, 0));
    }
    for (auto control : {a.page_edit, a.zoom_edit})
        if ((IsWindowEnabled(control) != FALSE) != loaded)
            EnableWindow(control, loaded);
    sync_entry(a.page_edit, page_number(a));
    sync_entry(a.zoom_edit, zoom_percent(a));
}

static void layout_toolbar(App& a) {
    if (!a.toolbar)
        return;
    auto r = client(a);
    position_control(a.toolbar, 0, 0, r.right, px(a, toolbar_height));
    SendMessageW(a.toolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(px(a, 16), px(a, 16)));
    SendMessageW(a.toolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(px(a, 28), px(a, toolbar_height)));
    SendMessageW(a.toolbar, TB_SETINDENT, px(a, 4), 0);
    SendMessageW(a.toolbar, TB_SETPADDING, 0, MAKELPARAM(0, 0));
    // Six digits fit in the page entry; total-page width grows with the document.
    int total_width = 18 + 8 * int(std::to_wstring(std::max<size_t>(1, a.pages.size())).size());
    for (int index = 0; index < SendMessageW(a.toolbar, TB_BUTTONCOUNT, 0, 0); ++index) {
        TBBUTTON button{};
        SendMessageW(a.toolbar, TB_GETBUTTON, WPARAM(index), LPARAM(&button));
        TBBUTTONINFOW info{sizeof(info)};
        info.dwMask = TBIF_SIZE | TBIF_BYINDEX;
        info.cx = WORD(px(a, button.idCommand == page_slot   ? 54 + total_width
                             : button.idCommand == zoom_slot ? 60
                             : (button.fsStyle & BTNS_SEP)   ? 12
                                                             : 28));
        SendMessageW(a.toolbar, TB_SETBUTTONINFOW, WPARAM(index), LPARAM(&info));
    }
    // Keep essential navigation visible; every hidden action remains in the main menus.
    for (int id : {cmd_print, cmd_rotate, cmd_sidebar, cmd_bookmarks})
        SendMessageW(a.toolbar, TB_HIDEBUTTON, WPARAM(id), FALSE);
    for (int id : {cmd_bookmarks, cmd_sidebar, cmd_rotate, cmd_print}) {
        SIZE size{};
        SendMessageW(a.toolbar, TB_GETMAXSIZE, 0, LPARAM(&size));
        if (size.cx + px(a, 8) <= r.right)
            break;
        SendMessageW(a.toolbar, TB_HIDEBUTTON, WPARAM(id), TRUE);
    }
    RECT page{}, zoom{};
    SendMessageW(a.toolbar, TB_GETRECT, page_slot, LPARAM(&page));
    SendMessageW(a.toolbar, TB_GETRECT, zoom_slot, LPARAM(&zoom));
    // Edit controls are toolbar children, so the native control clips and paints them correctly.
    position_control(a.page_edit, page.left + px(a, 5), px(a, 9), px(a, 42), px(a, 18));
    position_control(a.zoom_edit, zoom.left + px(a, 5), px(a, 9), px(a, 50), px(a, 18));
}

static void paint_toolbar_button(App& a, HDC dc, RECT rect, int id, UINT state) {
    bool enabled = (state & CDIS_DISABLED) == 0;
    bool active = (state & (CDIS_CHECKED | CDIS_SELECTED)) != 0;
    bool hot = (state & CDIS_HOT) != 0;
    rect.top = px(a, 4);
    rect.bottom = px(a, 32);
    InflateRect(&rect, -px(a, 1), 0);
    for (const auto& tool : toolbar_tools)
        if (tool.command == id) {
            COLORREF background = active           ? a.search_colors.pressed
                                  : hot && enabled ? a.search_colors.hover
                                                   : a.search_colors.footer;
            COLORREF color = !enabled        ? a.search_colors.muted
                             : active || hot ? a.search_colors.text
                                             : a.search_colors.muted;
            ui::control(a.painter, dc, rect, a.dpi, a.search_colors.footer, background,
                        a.search_colors.accent, (state & CDIS_FOCUS) ? 2 : 0, tool.icon, color);
            break;
        }
}

static void paint_input_frame(App& a, HDC dc, HWND parent, HWND input) {
    if (!input)
        return;
    RECT rect{};
    GetWindowRect(input, &rect);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
    InflateRect(&rect, px(a, 3), px(a, 3));
    bool focused = GetFocus() == input;
    COLORREF outline = focused                  ? a.search_colors.accent
                       : a.input_hover == input ? a.search_colors.muted
                                                : a.search_colors.border;
    ui::control(a.painter, dc, rect, a.dpi, a.search_colors.footer, a.search_colors.field, outline,
                focused ? 2 : 1);
}

static void paint_sidebar_item(App& a, HDC dc, RECT rect, int index, bool selected, bool focused) {
    fill_rect(dc, rect, a.search_colors.footer);
    RECT row = rect;
    InflateRect(&row, -px(a, 8), -px(a, 2));
    COLORREF fill = selected                   ? a.search_colors.pressed
                    : index == a.sidebar_hover ? a.search_colors.hover
                                               : a.search_colors.footer;
    if (selected || index == a.sidebar_hover || focused)
        ui::control(a.painter, dc, row, a.dpi, a.search_colors.footer, fill, a.search_colors.accent,
                    focused ? 2 : 0);
    rect.left = row.left + px(a, 10);
    rect.right = row.right - px(a, 10);
    std::wstring text = L"Page " + std::to_wstring(index + 1);
    label(dc, rect, text, selected || index == a.sidebar_hover ? a.search_colors.text : a.search_colors.muted,
          selected ? a.selected_font : a.font);
}
static LRESULT paint_bookmark(App& a, NMTVCUSTOMDRAW& draw) {
    if (draw.nmcd.dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    if (draw.nmcd.dwDrawStage != CDDS_ITEMPREPAINT)
        return CDRF_DODEFAULT;
    auto item = HTREEITEM(draw.nmcd.dwItemSpec);
    size_t index = size_t(draw.nmcd.lItemlParam);
    if (index >= a.outline.size())
        return CDRF_SKIPDEFAULT;
    RECT text{}, row{};
    TreeView_GetItemRect(a.bookmark_tree, item, &text, TRUE);
    GetClientRect(a.bookmark_tree, &row);
    row.top = text.top;
    row.bottom = text.bottom;
    HDC dc = draw.nmcd.hdc;
    fill_rect(dc, row, a.search_colors.footer);
    InflateRect(&row, -px(a, 8), -px(a, 2));
    bool selected = TreeView_GetSelection(a.bookmark_tree) == item;
    bool focused = selected && GetFocus() == a.bookmark_tree;
    // The row background is already filled. Only a selected row needs a rounded shape.
    if (selected)
        ui::control(a.painter, dc, row, a.dpi, a.search_colors.footer, a.search_colors.pressed,
                    a.search_colors.accent, focused ? 2 : 0);
    if (TreeView_GetChild(a.bookmark_tree, item)) {
        int x = text.left - px(a, 10), y = (text.top + text.bottom) / 2, d = px(a, 3);
        bool expanded = (TreeView_GetItemState(a.bookmark_tree, item, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
        POINT points[3];
        if (expanded) {
            points[0] = {x - d, y - d / 2};
            points[1] = {x, y + d / 2};
            points[2] = {x + d, y - d / 2};
        } else {
            points[0] = {x - d / 2, y - d};
            points[1] = {x + d / 2, y};
            points[2] = {x - d / 2, y + d};
        }
        auto pen = CreatePen(PS_SOLID, std::max(1, px(a, 1)), a.search_colors.muted);
        auto old = SelectObject(dc, pen);
        Polyline(dc, points, 3);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    text.right = row.right - px(a, 10);
    std::wstring title = a.outline[index].title;
    std::replace(title.begin(), title.end(), L'\t', L' ');
    label(dc, text, title, selected ? a.search_colors.text : a.search_colors.muted,
          selected ? a.selected_font : a.font);
    return CDRF_SKIPDEFAULT;
}

static void paint_toolbar(App& a, HDC dc) {
    auto r = client(a);
    const RECT bounds{0, 0, r.right, px(a, toolbar_height)};
    if (!RectVisible(dc, &bounds))
        return;
    fill_rect(dc, {0, 0, r.right, px(a, toolbar_height)}, a.search_colors.footer);
    fill_rect(dc, {0, px(a, toolbar_height) - 1, r.right, px(a, toolbar_height)}, a.search_colors.border);
    int hot = int(SendMessageW(a.toolbar, TB_GETHOTITEM, 0, 0));
    for (int index = 0; index < SendMessageW(a.toolbar, TB_BUTTONCOUNT, 0, 0); ++index) {
        TBBUTTON button{};
        RECT rect{};
        SendMessageW(a.toolbar, TB_GETBUTTON, WPARAM(index), LPARAM(&button));
        SendMessageW(a.toolbar, TB_GETITEMRECT, WPARAM(index), LPARAM(&rect));
        if (button.fsState & TBSTATE_HIDDEN)
            continue;
        if (button.idCommand == page_slot) {
            rect.left += px(a, 54);
            rect.top = 0;
            rect.bottom = px(a, toolbar_height);
            label(dc, rect, a.pages.empty() ? L"/ —" : L"/ " + std::to_wstring(a.pages.size()),
                  a.search_colors.muted, a.small_font);
        } else if (button.idCommand == zoom_slot) {
            continue;
        } else if (button.fsStyle & BTNS_SEP) {
            int x = (rect.left + rect.right) / 2;
            fill_rect(dc, {x, px(a, 8), x + 1, px(a, 28)}, a.search_colors.border);
        } else {
            UINT state = (button.fsState & TBSTATE_ENABLED) ? 0 : CDIS_DISABLED;
            if (button.fsState & TBSTATE_CHECKED)
                state |= CDIS_CHECKED;
            if (button.fsState & TBSTATE_PRESSED)
                state |= CDIS_SELECTED;
            if (index == hot) {
                state |= CDIS_HOT;
                if (GetFocus() == a.toolbar)
                    state |= CDIS_FOCUS;
            }
            paint_toolbar_button(a, dc, rect, button.idCommand, state);
        }
    }
    paint_input_frame(a, dc, a.toolbar, a.page_edit);
    paint_input_frame(a, dc, a.toolbar, a.zoom_edit);
}
static RECT page_rect(App& a) {
    auto r = viewport(a);
    auto [w, h] = dimensions(a);
    int width = int(std::ceil(w * a.scale)), height = int(std::ceil(h * a.scale)), margin = px(a, 24);
    int left = r.left + std::max<LONG>(margin, (r.right - r.left - width) / 2) - a.scroll_x,
        top = r.top + std::max<LONG>(margin, (r.bottom - r.top - height) / 2) - a.scroll_y;
    return {left, top, left + width, top + height};
}
static RECT span_rect(App& a, Rect bounds) {
    auto rect = page_rect(a);
    double scale = a.scale / a.bitmap_scale;
    return {rect.left + LONG(std::floor(bounds.x0 * scale)), rect.top + LONG(std::floor(bounds.y0 * scale)),
            rect.left + LONG(std::ceil(bounds.x1 * scale)), rect.top + LONG(std::ceil(bounds.y1 * scale))};
}
static void reveal_search_match(App& a) {
    if (!a.reveal_match || !a.bitmap || a.active_match < 0 || size_t(a.active_match) >= a.matches.count ||
        search_result(a.matches, size_t(a.active_match)).page != a.page)
        return;
    const auto boxes = search_rects(*a.bitmap, search_result(a.matches, size_t(a.active_match)));
    if (boxes.empty())
        return;
    // Show the first fragment even when a phrase crosses several lines.
    const auto rect = span_rect(a, boxes.front()), vp = viewport(a);
    const int padding = px(a, 24);
    if (rect.left < vp.left + padding)
        a.scroll_x += rect.left - vp.left - padding;
    else if (rect.right > vp.right - padding)
        a.scroll_x += rect.right - vp.right + padding;
    if (rect.top < vp.top + padding || rect.bottom > vp.bottom - padding)
        a.scroll_y += (rect.top + rect.bottom - vp.top - vp.bottom) / 2;
    update_scroll(a);
    a.reveal_match = false;
}
static void highlight(HDC dc, RECT rect, uint32_t color) {
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), 1, -1, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* pixel = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixel, nullptr, 0);
    if (!bitmap)
        return;
    *static_cast<uint32_t*>(pixel) = color;
    HDC memory = CreateCompatibleDC(dc);
    auto old = SelectObject(memory, bitmap);
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, memory, 0, 0, 1, 1,
               blend);
    SelectObject(memory, old);
    DeleteDC(memory);
    DeleteObject(bitmap);
}
static std::vector<ui::PageMark> page_marks(App& a) {
    std::vector<ui::PageMark> marks;
    if (!a.bitmap)
        return marks;
    for (size_t i = 0; i < a.bitmap->text.size(); ++i)
        if (a.selected_all || std::find(a.selected.begin(), a.selected.end(), i) != a.selected.end())
            marks.push_back({span_rect(a, a.bitmap->text[i].bounds), 0x5010224b});
    if (a.find_open) {
        const auto batch = search_result_page(a.matches, a.page);
        for (size_t i = 0; batch && i < batch->matches.size(); ++i) {
            const bool active = int(batch->first + i) == a.active_match;
            for (const auto& bounds : search_rects(*a.bitmap, batch->matches[i])) {
                const auto rect = span_rect(a, bounds);
                marks.push_back({rect, active ? 0x60605000u : 0x40403800u});
                if (active) {
                    const int stroke = std::max(1, px(a, 2));
                    RECT inner = rect;
                    InflateRect(&inner, px(a, 1) - 1, px(a, 1) - 1);
                    const auto c = a.search_colors.accent;
                    const uint32_t color = 0xff000000u | (uint32_t(GetRValue(c)) << 16) |
                                           (uint32_t(GetGValue(c)) << 8) | GetBValue(c);
                    const LONG left = inner.left - stroke, right = inner.right + stroke;
                    marks.push_back({{left, inner.top - stroke, right, inner.top}, color});
                    marks.push_back({{left, inner.bottom, right, inner.bottom + stroke}, color});
                    marks.push_back({{left, inner.top, inner.left, inner.bottom}, color});
                    marks.push_back({{inner.right, inner.top, right, inner.bottom}, color});
                }
            }
        }
    }
    return marks;
}
static bool paint_gpu_page(App& a, const RECT& dirty) {
    if (!a.bitmap || a.pages.empty() || IsIconic(a.window) || !IsWindowVisible(a.window)) {
        ui::hide_page_view(a.page_view);
        return false;
    }
    const auto vp = viewport(a);
    RECT intersection{};
    if (a.page_view.visible && a.page_view.source == a.bitmap && EqualRect(&vp, &a.page_view.bounds) &&
        !IntersectRect(&intersection, &dirty, &vp))
        return true;
    const auto marks = page_marks(a);
    const ui::PageFrame frame{
        vp, page_rect(a), {px(a, 3), px(a, 5)}, a.search_colors.field, a.search_colors.border, marks};
    const bool was_visible = a.page_view.visible;
    const bool presented = ui::present_page_view(a.page_view, a.window, a.bitmap, frame);
    if (was_visible && !presented)
        InvalidateRect(a.window, &vp, FALSE);
    return presented;
}
static void paint_app(App& a, HDC dc) {
    auto r = client(a);
    auto vp = viewport(a);
    fill_rect(dc, r, a.search_colors.field);
    paint_toolbar(a, dc);
    if (sidebar_visible(a)) {
        const auto pane = document_bounds(a);
        fill_rect(dc, {0, pane.top, pane.left, pane.bottom}, a.search_colors.footer);
        if (results_visible(a))
            fill_rect(dc, {0, pane.top + a.sidebar_header_height, sidebar_splitter(a).left, pane.bottom},
                      a.search_colors.body);
        fill_rect(dc, {pane.left - 1, pane.top, pane.left, pane.bottom}, a.search_colors.border);
        auto splitter = sidebar_splitter(a);
        int center = (splitter.left + splitter.right) / 2;
        fill_rect(dc, {center, pane.top, center + 1, pane.bottom},
                  a.resizing_sidebar ? a.search_colors.accent : a.search_colors.border);
    }
    int saved = SaveDC(dc);
    IntersectClipRect(dc, vp.left, vp.top, vp.right, vp.bottom);
    if (RectVisible(dc, &vp)) {
        if (a.pages.empty()) {
            int center = (vp.top + vp.bottom) / 2;
            constexpr std::wstring_view prompt = L"Drop a PDF here, or press Ctrl+O to open one.";
            RECT heading{vp.left + px(a, 40), 0, vp.right - px(a, 40), 0};
            auto old_font = SelectObject(dc, a.title_font);
            DrawTextW(dc, prompt.data(), int(prompt.size()), &heading,
                      DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            SelectObject(dc, old_font);
            // Keep the original headline's center, allowing the prompt to wrap in narrow windows.
            int height = heading.bottom - heading.top;
            heading.left = vp.left + px(a, 40);
            heading.right = vp.right - px(a, 40);
            heading.top = center - px(a, 67) - height / 2;
            heading.bottom = heading.top + height;
            label(dc, heading, prompt, a.search_colors.text, a.title_font, DT_CENTER | DT_WORDBREAK);
        } else {
            RECT page = page_rect(a);
            RECT shadow = page;
            OffsetRect(&shadow, px(a, 3), px(a, 5));
            fill_rect(dc, shadow, a.search_colors.border);
            fill_rect(dc, page, RGB(255, 255, 255));
            if (a.bitmap) {
                BITMAPINFO bmi{};
                bmi.bmiHeader = {sizeof(BITMAPINFOHEADER),
                                 LONG(a.bitmap->width),
                                 -LONG(a.bitmap->height),
                                 1,
                                 32,
                                 BI_RGB,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0};
                // A filtered full-page resample can exceed a frame budget on the UI thread.
                // Use the cached raster for immediate feedback; restore filtering on release.
                SetStretchBltMode(dc, live_resize(a) ? COLORONCOLOR : HALFTONE);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                StretchDIBits(dc, page.left, page.top, page.right - page.left, page.bottom - page.top, 0, 0,
                              int(a.bitmap->width), int(a.bitmap->height), a.bitmap->pixels.data(), &bmi,
                              DIB_RGB_COLORS, SRCCOPY);
                for (const auto& mark : page_marks(a))
                    if ((mark.color >> 24) == 255)
                        fill_rect(dc, mark.bounds,
                                  RGB((mark.color >> 16) & 255, (mark.color >> 8) & 255, mark.color & 255));
                    else
                        highlight(dc, mark.bounds, mark.color);
            } else
                label(dc, page, L"Rendering…", a.search_colors.muted, a.font,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    RestoreDC(dc, saved);
    RECT status{0, r.bottom - px(a, 30), r.right, r.bottom};
    fill_rect(dc, status, a.search_colors.footer);
    label(dc, {px(a, 14), status.top, r.right - px(a, 250), status.bottom},
          a.update_status.empty() ? a.status : a.update_status, a.search_colors.muted, a.small_font);
    if (!a.pages.empty()) {
        std::wstring details =
            L"Page " + std::to_wstring(a.page + 1) + L" of " + std::to_wstring(a.pages.size());
        label(dc, {r.right - px(a, 240), status.top, r.right - px(a, 14), status.bottom}, details,
              a.search_colors.muted, a.small_font, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}
static void paint_window(App& a, HDC dc, const RECT& dirty) {
    if (IsRectEmpty(&dirty))
        return;
    const auto r = client(a);
    // Memory-DC captures and printing retain the complete CPU paint path.
    const bool gpu = WindowFromDC(dc) == a.window && paint_gpu_page(a, dirty);
    const auto vp = viewport(a);
    const int saved_dc = SaveDC(dc);
    if (gpu)
        ExcludeClipRect(dc, vp.left, vp.top, vp.right, vp.bottom);
    // Grow in blocks, so dragging an edge does not allocate a new bitmap for every pixel.
    const int width = (std::max(1L, r.right) + 127) / 128 * 128;
    const int height = (std::max(1L, r.bottom) + 127) / 128 * 128;
    if (!ui::prepare(a.frame, dc, width, height)) {
        paint_app(a, dc);
        RestoreDC(dc, saved_dc);
        return;
    }
    const int saved = SaveDC(a.frame.dc);
    IntersectClipRect(a.frame.dc, dirty.left, dirty.top, dirty.right, dirty.bottom);
    if (gpu)
        ExcludeClipRect(a.frame.dc, vp.left, vp.top, vp.right, vp.bottom);
    // A memory DC does not inherit WS_CLIPCHILDREN from the paint DC.
    for (auto child = GetWindow(a.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
            continue;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, a.window, reinterpret_cast<POINT*>(&bounds), 2);
        ExcludeClipRect(a.frame.dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
    }
    paint_app(a, a.frame.dc);
    BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, a.frame.dc,
           dirty.left, dirty.top, SRCCOPY);
    RestoreDC(a.frame.dc, saved);
    RestoreDC(dc, saved_dc);
}
static void screenshot(App& a) {
    auto r = client(a);
    HDC dc = GetDC(a.window), memory = CreateCompatibleDC(dc);
    BITMAPINFO bmi{};
    bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), r.right, -r.bottom, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void* data = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &data, nullptr, 0);
    require(bitmap != nullptr, "Cannot allocate UI capture");
    auto old = SelectObject(memory, bitmap);
    paint_app(a, memory);
    for (auto control : {a.page_edit, a.zoom_edit, a.search, a.bookmark_tree, a.sidebar_tabs,
                         a.document_vscroll, a.document_hscroll}) {
        if (!(GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE))
            continue;
        if ((control == a.search && !a.find_open) ||
            (control == a.bookmark_tree &&
             (!a.show_sidebar || !a.bookmarks || results_visible(a) || a.pages.empty())) ||
            (control == a.sidebar_tabs && !sidebar_visible(a)))
            continue;
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, a.window, reinterpret_cast<POINT*>(&rect), 2);
        int saved = SaveDC(memory);
        SetViewportOrgEx(memory, rect.left, rect.top, nullptr);
        SendMessageW(control, WM_PRINT, WPARAM(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        RestoreDC(memory, saved);
    }
    if (a.show_sidebar && !results_visible(a) && !a.pages.empty() && !a.bookmarks) {
        auto vp = document_bounds(a);
        int top = vp.top + a.sidebar_header_height;
        int first = int(SendMessageW(a.sidebar, LB_GETTOPINDEX, 0, 0));
        int count = int(SendMessageW(a.sidebar, LB_GETCOUNT, 0, 0));
        int selected = int(SendMessageW(a.sidebar, LB_GETCURSEL, 0, 0));
        int saved = SaveDC(memory);
        IntersectClipRect(memory, 0, top, sidebar_splitter(a).left, vp.bottom);
        for (int i = std::max(0, first); i < count && top < vp.bottom; ++i) {
            paint_sidebar_item(a, memory, {0, top, sidebar_splitter(a).left, top + px(a, ui::row_height)}, i,
                               i == selected, false);
            top += px(a, ui::row_height);
        }
        RestoreDC(memory, saved);
    }
    RenderedPage page;
    page.width = unsigned(r.right);
    page.height = unsigned(r.bottom);
    page.pixels.assign(static_cast<uint8_t*>(data),
                       static_cast<uint8_t*>(data) + size_t(r.right) * r.bottom * 4);
    for (size_t i = 3; i < page.pixels.size(); i += 4)
        page.pixels[i] = 255;
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(a.window, dc);
    Renderer renderer;
    try {
        init_renderer(renderer);
        save_png(renderer, page, a.smoke_output);
        close_renderer(renderer);
    } catch (...) {
        close_renderer(renderer);
        throw;
    }
}
static void receive(App& a) {
    std::deque<Result> results;
    {
        std::lock_guard lock(a.worker.mutex);
        results.swap(a.worker.results);
    }
    for (auto& result : results) {
        if (result.document != a.document && result.document != 0)
            continue;
        if (result.kind == Job::preview) {
            if (a.print_preview && result.ticket == a.preview_ticket) {
                a.preview_bitmap = std::move(result.bitmap);
                a.preview_error = std::move(result.error);
                InvalidateRect(GetDlgItem(a.print_preview, IDC_PREVIEW_IMAGE), nullptr, TRUE);
            }
            continue;
        }
        if (result.kind == Job::render && result.ticket != a.render_ticket)
            continue;
        a.busy = false;
        if (!result.error.empty()) {
            a.status = result.error;
            if (a.smoke) {
                a.exit_code = 2;
                PostMessageW(a.window, WM_CLOSE, 0, 0);
                continue;
            }
            if (result.kind == Job::open && result.error.find(L"Password") != std::wstring::npos) {
                std::wstring password;
                if (DialogBoxParamW(a.instance, MAKEINTRESOURCEW(IDD_PASSWORD), a.window, password_dialog,
                                    LPARAM(&password)) == IDOK)
                    load_file(a, a.path, password);
            } else
                ui::message_box(a.window, result.error.c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
            continue;
        }
        if (result.kind == Job::open) {
            a.pages = std::move(result.pages);
            a.outline = std::move(result.outline);
            rebuild_bookmarks(a);
            a.bookmarks = true;
            a.show_sidebar = !a.outline.empty();
            a.status = result.message;
            if (a.restore) {
                a.page = std::clamp(a.restore->page, 0, std::max(0, int(a.pages.size()) - 1));
                a.rotation = a.restore->rotation;
                a.fit = Fit(a.restore->fit);
                a.zoom = a.restore->zoom;
                a.restore.reset();
            }
            update_sidebar(a, true);
            layout(a);
            request_render(a, true);
        } else if (result.kind == Job::render) {
            a.bitmap = std::move(result.bitmap);
            a.bitmap_scale = result.scale;
            reveal_search_match(a);
            a.status = std::filesystem::path(a.path).filename().wstring();
            if (a.smoke) {
                try {
                    screenshot(a);
                } catch (...) {
                    a.exit_code = 3;
                }
                PostMessageW(a.window, WM_CLOSE, 0, 0);
            }
        } else {
            a.status = result.message;
            if (!a.bitmap && !a.pages.empty())
                request_render(a);
        }
    }
    InvalidateRect(a.window, nullptr, FALSE);
}
static void scroll(App& a, int bar, int code, int delta = 0) {
    auto vp = viewport(a);
    int& position = bar == SB_VERT ? a.scroll_y : a.scroll_x;
    int page = bar == SB_VERT ? vp.bottom - vp.top : vp.right - vp.left;
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(bar == SB_VERT ? a.document_vscroll : a.document_hscroll, SB_CTL, &info);
    if (code == SB_LINEUP)
        position -= px(a, 48);
    else if (code == SB_LINEDOWN)
        position += px(a, 48);
    else if (code == SB_PAGEUP)
        position -= std::max(1, page - px(a, 40));
    else if (code == SB_PAGEDOWN)
        position += std::max(1, page - px(a, 40));
    else if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION)
        position = info.nTrackPos;
    else if (code == SB_TOP)
        position = 0;
    else if (code == SB_BOTTOM)
        position = info.nMax;
    else
        position += delta;
    update_scroll(a);
    InvalidateRect(a.window, &vp, FALSE);
}
static const Link* hit_link(App& a, POINT p) {
    if (!a.bitmap)
        return nullptr;
    for (auto& link : a.bitmap->links) {
        auto rect = span_rect(a, link.bounds);
        if (PtInRect(&rect, p))
            return &link;
    }
    return nullptr;
}

static LRESULT CALLBACK input_frame_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                         DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    HWND parent = GetParent(window);
    auto invalidate_frame = [&] {
        RECT rect{};
        GetWindowRect(window, &rect);
        MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
        InflateRect(&rect, px(a, 4), px(a, 4));
        InvalidateRect(parent, &rect, FALSE);
    };
    if (message == WM_MOUSEMOVE && a.input_hover != window) {
        a.input_hover = window;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        invalidate_frame();
    } else if (message == WM_MOUSELEAVE) {
        if (a.input_hover == window)
            a.input_hover = nullptr;
        invalidate_frame();
    }
    LRESULT result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
        invalidate_frame();
    return result;
}

static LRESULT CALLBACK sidebar_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                     DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    if (window == a.sidebar && message == WM_MOUSEMOVE) {
        DWORD item = DWORD(SendMessageW(window, LB_ITEMFROMPOINT, 0, lp));
        int hover = HIWORD(item) ? -1 : int(LOWORD(item));
        set_sidebar_hover(a, hover);
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (window == a.sidebar &&
               (message == WM_MOUSELEAVE || message == WM_VSCROLL || message == WM_MOUSEWHEEL)) {
        set_sidebar_hover(a, -1);
    } else if (message == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            SetFocus(a.window);
            return 0;
        }
        if (wp == VK_F3 || wp == VK_F6 ||
            ((GetKeyState(VK_CONTROL) & 0x8000) &&
             (wp == 'O' || wp == 'F' || wp == 'G' || wp == 'P' || wp == 'S'))) {
            SendMessageW(a.window, message, wp, lp);
            return 0;
        }
    }
    LRESULT result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}
static LRESULT CALLBACK sidebar_tabs_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                          DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        paint_sidebar_tabs(a, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        paint_sidebar_tabs(a, HDC(wp));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        TCHITTESTINFO hit{{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, 0};
        const int hover = TabCtrl_HitTest(window, &hit);
        if (a.tab_hover != hover) {
            a.tab_hover = hover;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        break;
    }
    case WM_MOUSELEAVE:
        a.tab_hover = -1;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            SetFocus(a.window);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, sidebar_tabs_proc, id);
        break;
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_UPDATEUISTATE ||
        message == WM_KEYDOWN)
        InvalidateRect(window, nullptr, FALSE);
    return result;
}
static LRESULT CALLBACK search_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                    DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    if (message == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            submit_find(a, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            command(a, cmd_search);
            return 0;
        }
        if (wp == VK_F3) {
            find(a, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        }
    }
    return DefSubclassProc(window, message, wp, lp);
}

static LRESULT CALLBACK toolbar_control_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                             DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    bool edit = window == a.page_edit || window == a.zoom_edit;
    if (window == a.toolbar && (message == WM_SETFOCUS || message == WM_KILLFOCUS))
        InvalidateRect(window, nullptr, FALSE);
    if (window == a.toolbar &&
        (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC || message == WM_COMMAND))
        return SendMessageW(a.window, message, wp, lp);
    if (message == WM_KEYDOWN) {
        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && (wp == 'O' || wp == 'F' || wp == 'G' || wp == 'P' || wp == 'S')) {
            SendMessageW(a.window, message, wp, lp);
            return 0;
        }
        if (wp == VK_ESCAPE || wp == VK_F6) {
            SetFocus(a.window);
            sync_toolbar(a);
            return 0;
        }
        if (wp == VK_RETURN && edit) {
            wchar_t text[64]{};
            GetWindowTextW(window, text, int(std::size(text)));
            wchar_t* end = nullptr;
            if (window == a.page_edit) {
                long page = wcstol(text, &end, 10);
                while (iswspace(*end))
                    ++end;
                if (end == text || *end || page < 1 || size_t(page) > a.pages.size()) {
                    MessageBeep(MB_ICONWARNING);
                    SendMessageW(window, EM_SETSEL, 0, -1);
                    return 0;
                }
                SetFocus(a.window);
                goto_page(a, int(page - 1));
            } else {
                double percent = wcstod(text, &end);
                bool parsed = end != text;
                while (iswspace(*end))
                    ++end;
                if (*end == L'%')
                    ++end;
                while (iswspace(*end))
                    ++end;
                if (!parsed || *end || !std::isfinite(percent) || percent < 5 || percent > 800) {
                    MessageBeep(MB_ICONWARNING);
                    SendMessageW(window, EM_SETSEL, 0, -1);
                    return 0;
                }
                SetFocus(a.window);
                a.fit = Fit::custom;
                a.zoom = percent / 100;
                request_render(a);
            }
            sync_toolbar(a);
            return 0;
        }
        if (wp == VK_RETURN && window == a.toolbar) {
            int hot = int(SendMessageW(window, TB_GETHOTITEM, 0, 0));
            TBBUTTON button{};
            if (hot >= 0 && SendMessageW(window, TB_GETBUTTON, WPARAM(hot), LPARAM(&button)) &&
                (button.fsState & TBSTATE_ENABLED))
                command(a, button.idCommand);
            return 0;
        }
    }
    if (message == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE))
        return 0;
    if (message == WM_KILLFOCUS && edit) {
        SetWindowTextW(window, (window == a.page_edit ? page_number(a) : zoom_percent(a)).c_str());
    }
    return DefSubclassProc(window, message, wp, lp);
}

static void create_toolbar(App& a) {
    a.toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, L"Document toolbar",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | TBSTYLE_FLAT |
                                    TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN | CCS_NODIVIDER,
                                0, 0, 0, 0, a.window, HMENU(INT_PTR(id_toolbar)), a.instance, nullptr);
    require(a.toolbar != nullptr, "Cannot create document toolbar");
    SendMessageW(a.toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(a.toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_MIXEDBUTTONS);
    for (int id : std::initializer_list<int>{cmd_open, cmd_print, 0, cmd_prev, page_slot, cmd_next, 0,
                                             cmd_zoom_out, zoom_slot, cmd_zoom_in, 0, cmd_fit, cmd_width,
                                             cmd_rotate, 0, cmd_search, cmd_sidebar, cmd_bookmarks}) {
        TBBUTTON button{};
        button.idCommand = id;
        button.fsState = TBSTATE_ENABLED;
        button.iBitmap = I_IMAGENONE;
        button.fsStyle = BTNS_BUTTON;
        if (id == 0 || id == page_slot || id == zoom_slot) {
            button.fsStyle = BTNS_SEP;
            button.iBitmap = 12;
        } else {
            for (const auto& tool : toolbar_tools)
                if (tool.command == id)
                    button.iString = INT_PTR(tool.name);
        }
        SendMessageW(a.toolbar, TB_ADDBUTTONSW, 1, LPARAM(&button));
    }
    auto edit = [&](int id, const wchar_t* name) {
        HWND window =
            CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
                            0, 0, 0, 0, a.toolbar, HMENU(INT_PTR(id)), a.instance, nullptr);
        require(window != nullptr, "Cannot create toolbar entry");
        SendMessageW(window, EM_LIMITTEXT, 16, 0);
        SendMessageW(window, EM_SETCUEBANNER, TRUE, LPARAM(name));
        ui::center_edit_text(window);
        return window;
    };
    a.page_edit = edit(id_page, L"Page number");
    a.zoom_edit = edit(id_zoom, L"Zoom (%)");
    for (auto control : {a.toolbar, a.page_edit, a.zoom_edit})
        SetWindowSubclass(control, toolbar_control_proc, 1, DWORD_PTR(&a));
    for (auto control : {a.page_edit, a.zoom_edit})
        SetWindowSubclass(control, input_frame_proc, 2, DWORD_PTR(&a));
    // The toolbar's native tooltip window also describes its two text fields.
    HWND tips = HWND(SendMessageW(a.toolbar, TB_GETTOOLTIPS, 0, 0));
    require(tips != nullptr, "Cannot create toolbar hints");
    // Use the application colors instead of the system tooltip theme.
    SetWindowTheme(tips, L"", L"");
    SendMessageW(tips, TTM_SETTIPBKCOLOR, a.search_colors.field, 0);
    SendMessageW(tips, TTM_SETTIPTEXTCOLOR, a.search_colors.text, 0);
    SendMessageW(tips, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
    SendMessageW(tips, TTM_SETDELAYTIME, TTDT_RESHOW, 100);
    SendMessageW(tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);
    for (auto [window, name] : {std::pair{a.page_edit, L"Page number — Enter to go, Esc to cancel"},
                                {a.zoom_edit, L"Zoom percentage (5–800%) — Enter to apply"}}) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = a.toolbar;
        tool.uId = UINT_PTR(window);
        tool.lpszText = const_cast<wchar_t*>(name);
        SendMessageW(tips, TTM_ADDTOOLW, 0, LPARAM(&tool));
    }
}
static void window_icons(App& a) {
    for (auto [kind, current] : {std::pair{ICON_BIG, &a.icon}, std::pair{ICON_SMALL, &a.small_icon}}) {
        const bool large = kind == ICON_BIG;
        auto icon = static_cast<HICON>(
            LoadImageW(a.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                       GetSystemMetricsForDpi(large ? SM_CXICON : SM_CXSMICON, a.dpi),
                       GetSystemMetricsForDpi(large ? SM_CYICON : SM_CYSMICON, a.dpi), LR_DEFAULTCOLOR));
        require(icon != nullptr, "Cannot load application icon");
        SendMessageW(a.window, WM_SETICON, WPARAM(kind), LPARAM(icon));
        if (*current)
            DestroyIcon(*current);
        *current = icon;
    }
}
static void refresh_reader_theme(App& a) {
    a.search_colors = ui::system_dialog_colors();
    const auto& c = a.search_colors;
    for (auto brush : {a.edit_background, a.chrome_background, a.results_background})
        if (brush)
            DeleteObject(brush);
    a.edit_background = CreateSolidBrush(c.field);
    a.chrome_background = CreateSolidBrush(c.footer);
    a.results_background = CreateSolidBrush(c.body);
    BOOL dark = c.dark;
    DwmSetWindowAttribute(a.window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(a.window, DWMWA_CAPTION_COLOR, &c.footer, sizeof(COLORREF));
    DwmSetWindowAttribute(a.window, DWMWA_TEXT_COLOR, &c.text, sizeof(COLORREF));
    // SetWindowTheme sends WM_THEMECHANGED synchronously to this window.
    a.setting_window_theme = true;
    SetWindowTheme(a.window, c.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    a.setting_window_theme = false;
    for (auto control : {a.sidebar, a.bookmark_tree, a.search_results, a.page_edit, a.zoom_edit,
                         a.document_vscroll, a.document_hscroll})
        SetWindowTheme(control, c.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    ListView_SetBkColor(a.search_results, c.body);
    ListView_SetTextBkColor(a.search_results, c.body);
    ListView_SetTextColor(a.search_results, c.text);
    TreeView_SetBkColor(a.bookmark_tree, c.footer);
    TreeView_SetTextColor(a.bookmark_tree, c.text);
    TreeView_SetLineColor(a.bookmark_tree, c.border);
    ui::refresh_dialog_style(a.find_bar);
    if (GetMenu(a.window))
        style_menu(a, GetMenu(a.window), true);
    RedrawWindow(a.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
static void paint_search_result(App& a, const DRAWITEMSTRUCT& item) {
    const auto& colors = a.search_colors;
    fill_rect(item.hDC, item.rcItem, colors.body);
    if (item.itemID >= a.matches.count)
        return;
    const auto& match = search_result(a.matches, item.itemID);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    RECT row = item.rcItem;
    InflateRect(&row, -px(a, 8), -px(a, 2));
    if (selected) {
        ui::control(a.painter, item.hDC, row, a.dpi, colors.body, colors.pressed, colors.accent,
                    GetFocus() == a.search_results ? 1 : 0);
        RECT line{row.left, row.top + px(a, 4), row.left + px(a, 3), row.bottom - px(a, 4)};
        fill_rect(item.hDC, line, colors.accent);
    }
    row.left += px(a, 10);
    row.right -= px(a, 8);
    RECT heading = row;
    heading.bottom = heading.top + px(a, 21);
    label(item.hDC, heading, L"Page " + std::to_wstring(match.page + 1), colors.muted, a.search_font,
          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    row.top = heading.bottom;
    const auto font = SelectObject(item.hDC, a.search_font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(item.hDC, &metrics);
    row.bottom = std::min(row.bottom, row.top + 2 * metrics.tmHeight);
    std::wstring snippet = match.snippet;
    for (;;) {
        RECT size = row;
        DrawTextW(item.hDC, snippet.c_str(), int(snippet.size()), &size,
                  DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        if (size.bottom <= row.bottom || snippet.size() <= 1)
            break;
        if (snippet.back() == L'…')
            snippet.pop_back();
        snippet.pop_back();
        if (!snippet.empty() && snippet.back() >= 0xd800 && snippet.back() <= 0xdbff)
            snippet.pop_back();
        snippet += L'…';
    }
    SelectObject(item.hDC, font);
    label(item.hDC, row, snippet, colors.text, a.search_font, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* ptr = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        ptr = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        ptr->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(ptr));
    }
    if (!ptr)
        return DefWindowProcW(window, message, wp, lp);
    auto& a = *ptr;
    try {
        switch (message) {
        case WM_CREATE: {
            a.search_colors = ui::system_dialog_colors();
            a.dpi = GetDpiForWindow(window);
            window_icons(a);
            a.document_vscroll =
                CreateWindowExW(0, L"SCROLLBAR", L"Scroll document vertically", WS_CHILD | SBS_VERT, 0, 0, 0,
                                0, window, HMENU(INT_PTR(id_document_vscroll)), a.instance, nullptr);
            a.document_hscroll =
                CreateWindowExW(0, L"SCROLLBAR", L"Scroll document horizontally", WS_CHILD | SBS_HORZ, 0, 0,
                                0, 0, window, HMENU(INT_PTR(id_document_hscroll)), a.instance, nullptr);
            require(a.document_vscroll && a.document_hscroll, "Cannot create document scrollbars");
            a.sidebar_tabs =
                CreateWindowExW(0, WC_TABCONTROLW, L"Sidebar views",
                                WS_CHILD | WS_TABSTOP | TCS_MULTILINE | TCS_FOCUSONBUTTONDOWN, 0, 0, 0, 0,
                                window, HMENU(INT_PTR(id_sidebar_tabs)), a.instance, nullptr);
            require(a.sidebar_tabs != nullptr, "Cannot create sidebar tabs");
            SetWindowTheme(a.sidebar_tabs, L"", L"");
            SetWindowSubclass(a.sidebar_tabs, sidebar_tabs_proc, 1, DWORD_PTR(&a));
            a.sidebar = CreateWindowExW(0, L"LISTBOX", L"Pages",
                                        WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NODATA | LBS_OWNERDRAWFIXED |
                                            LBS_NOINTEGRALHEIGHT,
                                        0, 0, 0, 0, window, HMENU(INT_PTR(id_sidebar)), a.instance, nullptr);
            SetWindowSubclass(a.sidebar, sidebar_proc, 1, DWORD_PTR(&a));
            a.bookmark_tree =
                CreateWindowExW(0, WC_TREEVIEWW, L"Bookmarks",
                                WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                    TVS_FULLROWSELECT | TVS_NOTOOLTIPS | TVS_DISABLEDRAGDROP,
                                0, 0, 0, 0, window, HMENU(INT_PTR(id_bookmark_tree)), a.instance, nullptr);
            TreeView_SetExtendedStyle(a.bookmark_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
            TreeView_SetBkColor(a.bookmark_tree, a.search_colors.footer);
            TreeView_SetTextColor(a.bookmark_tree, a.search_colors.text);
            TreeView_SetLineColor(a.bookmark_tree, a.search_colors.border);
            SetWindowSubclass(a.bookmark_tree, sidebar_proc, 1, DWORD_PTR(&a));
            a.find_bar = CreateDialogParamW(a.instance, MAKEINTRESOURCEW(IDD_FIND_BAR), window, find_bar_proc,
                                            LPARAM(&a));
            require(a.find_bar != nullptr, "Cannot create search bar");
            SetWindowLongPtrW(a.find_bar, GWLP_ID, id_find_bar);
            a.search = GetDlgItem(a.find_bar, IDC_FIND_TEXT);
            SendMessageW(a.search, EM_SETCUEBANNER, TRUE, LPARAM(L"Find in document"));
            SendMessageW(a.search, EM_LIMITTEXT, 4095, 0);
            SetWindowSubclass(a.search, search_proc, 1, DWORD_PTR(&a));
            a.results_summary =
                CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window,
                                HMENU(INT_PTR(id_results_summary)), a.instance, nullptr);
            require(a.results_summary != nullptr, "Cannot create search summary");
            a.search_results =
                CreateWindowExW(0, WC_LISTVIEWW, L"Search Results",
                                WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA | LVS_NOCOLUMNHEADER |
                                    LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDRAWFIXED,
                                0, 0, 0, 0, window, HMENU(INT_PTR(id_results)), a.instance, nullptr);
            require(a.search_results != nullptr, "Cannot create search results");
            LVCOLUMNW column{};
            column.mask = LVCF_WIDTH;
            ListView_InsertColumn(a.search_results, 0, &column);
            ListView_SetExtendedListViewStyle(a.search_results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            SetWindowSubclass(a.search_results, sidebar_proc, 1, DWORD_PTR(&a));
            require(ui::enable_search_accessibility(a.search_results), "Cannot create search accessibility");
            create_toolbar(a);
            a.welcome_install =
                CreateWindowExW(0, L"BUTTON", L"Install FastPDF", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                0, 0, window, HMENU(INT_PTR(cmd_install)), a.instance, nullptr);
            a.welcome_portable =
                CreateWindowExW(0, L"BUTTON", L"Continue portable", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0,
                                0, 0, 0, window, HMENU(INT_PTR(cmd_portable)), a.instance, nullptr);
            require(a.welcome_install && a.welcome_portable, "Cannot create welcome controls");
            refresh_reader_theme(a);
            fonts(a);
            DragAcceptFiles(window, TRUE);
            a.worker.window = window;
            a.worker.thread = std::thread(worker_run, std::ref(a.worker));
            a.search_worker.window = window;
            a.search_worker.message = search_message;
            a.search_worker.thread = std::thread(run_search_worker, std::ref(a.search_worker));
            sync_search(a);
            layout(a);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            LRESULT result = DefWindowProcW(window, message, wp, lp);
            if (a.chrome_background && GetMenu(window) && !IsIconic(window)) {
                // Windows draws a light one-pixel menu divider outside the client area.
                RECT client_rect{}, window_rect{};
                GetClientRect(window, &client_rect);
                MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client_rect), 2);
                GetWindowRect(window, &window_rect);
                OffsetRect(&client_rect, -window_rect.left, -window_rect.top);
                RECT divider{client_rect.left, client_rect.top - 1, client_rect.right, client_rect.top};
                HDC dc = GetWindowDC(window);
                fill_rect(dc, divider, a.search_colors.border);
                ReleaseDC(window, dc);
            }
            return result;
        }
        case WM_NOTIFY: {
            auto header = reinterpret_cast<NMHDR*>(lp);
            if (header->hwndFrom == a.search_results) {
                if (header->code == LVN_GETDISPINFOW) {
                    auto info = reinterpret_cast<NMLVDISPINFOW*>(lp);
                    if (info->item.mask & LVIF_TEXT) {
                        std::wstring text;
                        if (info->item.iItem >= 0 && size_t(info->item.iItem) < a.matches.count) {
                            const auto& match = search_result(a.matches, size_t(info->item.iItem));
                            text = L"Page " + std::to_wstring(match.page + 1) + L": " + match.snippet;
                        }
                        if (info->item.pszText && info->item.cchTextMax > 0)
                            lstrcpynW(info->item.pszText, text.c_str(), info->item.cchTextMax);
                    }
                } else if (header->code == LVN_ITEMCHANGED && !a.syncing_results) {
                    const auto change = reinterpret_cast<NMLISTVIEW*>(lp);
                    if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) &&
                        change->iItem != a.active_match) {
                        a.search_notice.clear();
                        select_match(a, change->iItem);
                    }
                }
                return 0;
            }
            if (header->hwndFrom == a.sidebar_tabs && header->code == TCN_SELCHANGE) {
                select_sidebar_tab(a, TabCtrl_GetCurSel(a.sidebar_tabs));
                return 0;
            }
            if (header->hwndFrom == a.bookmark_tree) {
                if (header->code == NM_CUSTOMDRAW)
                    return paint_bookmark(a, *reinterpret_cast<NMTVCUSTOMDRAW*>(lp));
                if (header->code == TVN_SELCHANGEDW && !a.syncing_bookmarks) {
                    auto change = reinterpret_cast<NMTREEVIEWW*>(lp);
                    size_t index = size_t(change->itemNew.lParam);
                    if (change->action != TVC_UNKNOWN && index < a.outline.size() &&
                        a.outline[index].page >= 0)
                        goto_page(a, a.outline[index].page);
                    return 0;
                }
            }
            if (header->hwndFrom == a.toolbar && header->code == TBN_GETINFOTIPW) {
                auto tip = reinterpret_cast<NMTBGETINFOTIPW*>(lp);
                for (const auto& tool : toolbar_tools)
                    if (tool.command == tip->iItem) {
                        lstrcpynW(tip->pszText, tool.name, tip->cchTextMax);
                        break;
                    }
                return 0;
            }
            if (header->hwndFrom == a.toolbar && header->code == NM_CUSTOMDRAW) {
                auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
                if (draw->dwDrawStage == CDDS_PREPAINT) {
                    paint_toolbar(a, draw->hdc);
                    return CDRF_SKIPDEFAULT;
                }
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(window, &ps);
            paint_window(a, dc, ps.rcPaint);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                resize_layout(a);
            }
            return 0;
        case WM_DISPLAYCHANGE:
            // Reconnect to the graphics device after a display/session change.
            ui::close_page_view(a.page_view);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_ENTERSIZEMOVE:
            a.resizing_window = true;
            return 0;
        case WM_EXITSIZEMOVE:
            a.resizing_window = false;
            finish_resize(a);
            return 0;
        case WM_TIMER:
            if (wp == search_progress_timer) {
                if (!a.find_open || !a.search_running)
                    KillTimer(window, search_progress_timer);
                else if (GetTickCount64() >= a.search_progress_after) {
                    KillTimer(window, search_progress_timer);
                    sync_search(a);
                }
            } else if (wp == 4)
                receive_search(a);
            else if (wp == 3)
                start_search(a);
            else if (wp == 2)
                poll_update(a);
            else if (wp == 1) {
                KillTimer(window, 1);
                if (a.resize_render_pending)
                    request_render(a);
            }
            return 0;
        case WM_DPICHANGED: {
            a.dpi = HIWORD(wp);
            window_icons(a);
            fonts(a);
            auto r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            layout(a);
            request_render(a);
            return 0;
        }
        case WM_THEMECHANGED:
            if (a.setting_window_theme)
                return 0;
            [[fallthrough]];
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
            refresh_reader_theme(a);
            return 0;
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {px(a, 640), px(a, 480)};
            return 0;
        }
        case WM_CTLCOLORLISTBOX:
            if (HWND(lp) == a.search_results) {
                SetBkColor(HDC(wp), a.search_colors.body);
                SetTextColor(HDC(wp), a.search_colors.text);
                return LRESULT(a.results_background);
            }
            SetBkColor(HDC(wp), a.search_colors.footer);
            return LRESULT(a.chrome_background);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            if (HWND(lp) == a.results_summary) {
                SetBkColor(HDC(wp), a.search_colors.body);
                SetTextColor(HDC(wp), a.search_colors.muted);
                return LRESULT(a.results_background);
            }
            SetBkColor(HDC(wp), a.search_colors.field);
            SetTextColor(HDC(wp), IsWindowEnabled(HWND(lp)) ? a.search_colors.text : a.search_colors.muted);
            return LRESULT(a.edit_background);
        case WM_MEASUREITEM: {
            auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (item->CtlID == id_results) {
                item->itemHeight = UINT(px(a, 72));
                return TRUE;
            }
            if (item->CtlType != ODT_MENU || !item->itemData)
                break;
            const auto& data = *reinterpret_cast<const MenuItem*>(item->itemData);
            if (data.separator) {
                item->itemWidth = 0;
                item->itemHeight = UINT(px(a, 9));
                return TRUE;
            }
            HDC dc = GetDC(window);
            auto old = SelectObject(dc, a.font);
            RECT bounds{}, shortcut{};
            DrawTextW(dc, data.title.c_str(), -1, &bounds, DT_SINGLELINE | DT_CALCRECT);
            DrawTextW(dc, data.shortcut.c_str(), -1, &shortcut, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            SelectObject(dc, old);
            ReleaseDC(window, dc);
            item->itemWidth = UINT(bounds.right + shortcut.right + px(a, data.bar ? 16 : 64));
            item->itemHeight = UINT(
                data.bar ? std::max(bounds.bottom + px(a, 6), LONG(GetSystemMetricsForDpi(SM_CYMENU, a.dpi)))
                         : std::max(bounds.bottom + px(a, 12), LONG(px(a, 30))));
            return TRUE;
        }
        case WM_MENUCHAR: {
            HMENU menu = HMENU(lp);
            for (int index = 0; index < GetMenuItemCount(menu); ++index) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE;
                if (!GetMenuItemInfoW(menu, UINT(index), TRUE, &item) || !(item.fType & MFT_OWNERDRAW) ||
                    !item.dwItemData || (item.fState & (MFS_DISABLED | MFS_GRAYED)))
                    continue;
                const auto& title = reinterpret_cast<const MenuItem*>(item.dwItemData)->title;
                for (size_t i = 0; i + 1 < title.size(); ++i) {
                    if (title[i] != L'&')
                        continue;
                    if (title[++i] == L'&')
                        continue;
                    if (std::towupper(wchar_t(LOWORD(wp))) == std::towupper(title[i]))
                        return MAKELRESULT(index, MNC_EXECUTE);
                }
            }
            break;
        }
        case WM_DRAWITEM: {
            auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item->CtlID == id_results && item->CtlType == ODT_LISTVIEW) {
                paint_search_result(a, *item);
                return TRUE;
            }
            if (item->CtlType == ODT_MENU && item->itemData) {
                const auto& data = *reinterpret_cast<const MenuItem*>(item->itemData);
                int saved = SaveDC(item->hDC);
                bool disabled = (item->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
                COLORREF background = data.bar ? a.search_colors.footer : a.search_colors.field;
                if (!disabled && !data.separator) {
                    if (item->itemState & ODS_SELECTED)
                        background = a.search_colors.pressed;
                    else if (item->itemState & ODS_HOTLIGHT)
                        background = a.search_colors.hover;
                }
                fill_rect(item->hDC, item->rcItem, background);
                RECT bounds = item->rcItem;
                if (data.separator) {
                    bounds.left += px(a, 12);
                    bounds.right -= px(a, 12);
                    bounds.top = (bounds.top + bounds.bottom) / 2;
                    bounds.bottom = bounds.top + 1;
                    fill_rect(item->hDC, bounds, a.search_colors.border);
                    RestoreDC(item->hDC, saved);
                    return TRUE;
                }
                UINT flags = (data.bar ? DT_CENTER : DT_LEFT) | DT_SINGLELINE | DT_VCENTER;
                if (item->itemState & ODS_NOACCEL)
                    flags |= DT_HIDEPREFIX;
                if (!data.bar) {
                    bounds.left += px(a, 16);
                    bounds.right -= px(a, 16);
                    label(item->hDC, bounds, data.shortcut,
                          disabled ? a.search_colors.muted : a.search_colors.muted, a.font,
                          DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                label(item->hDC, bounds, data.title, disabled ? a.search_colors.muted : a.search_colors.text,
                      a.font, flags);
                RestoreDC(item->hDC, saved);
                return TRUE;
            }
            if (item->CtlID == cmd_install || item->CtlID == cmd_portable) {
                const bool focused = (item->itemState & ODS_FOCUS) != 0;
                const COLORREF fill =
                    (item->itemState & ODS_SELECTED) ? a.search_colors.pressed : a.search_colors.field;
                ui::control(a.painter, item->hDC, item->rcItem, a.dpi, a.search_colors.field, fill,
                            focused ? a.search_colors.accent : a.search_colors.border, 1);
                label(item->hDC, item->rcItem,
                      item->CtlID == cmd_install ? L"Install FastPDF" : L"Continue portable",
                      a.search_colors.text, a.font, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                return TRUE;
            }
            if (item->CtlID != id_sidebar || item->itemID == UINT(-1))
                break;
            // The list toggles its native focus rectangle around unrelated row repaints.
            // Our focus border is part of the selected row; focus changes invalidate it in sidebar_proc.
            if (item->itemAction == ODA_FOCUS)
                return TRUE;
            paint_sidebar_item(a, item->hDC, item->rcItem, int(item->itemID),
                               (item->itemState & ODS_SELECTED) != 0,
                               (item->itemState & ODS_SELECTED) != 0 && GetFocus() == a.sidebar);
            return TRUE;
        }
        case WM_INITMENUPOPUP:
            EnableMenuItem(GetMenu(window), cmd_updates,
                           MF_BYCOMMAND | (a.update_job.valid() ? MF_GRAYED : MF_ENABLED));
            EnableMenuItem(
                GetMenu(window), cmd_install,
                MF_BYCOMMAND |
                    (a.update_job.valid() || !a.pending_update.directory.empty() ? MF_GRAYED : MF_ENABLED));
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == id_sidebar && HIWORD(wp) == LBN_SELCHANGE) {
                int index = int(SendMessageW(a.sidebar, LB_GETCURSEL, 0, 0));
                if (index >= 0) {
                    goto_page(a, index);
                }
            } else if (LOWORD(wp) < 100)
                command(a, LOWORD(wp));
            return 0;
        case WM_DROPFILES: {
            HDROP drop = HDROP(wp);
            UINT n = DragQueryFileW(drop, 0, nullptr, 0);
            std::wstring path(n + 1, 0);
            DragQueryFileW(drop, 0, path.data(), n + 1);
            path.resize(n);
            DragFinish(drop);
            load_file(a, path);
            return 0;
        }
        case WM_VSCROLL:
            scroll(a, SB_VERT, LOWORD(wp));
            return 0;
        case WM_HSCROLL:
            scroll(a, SB_HORZ, LOWORD(wp));
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
                command(a, delta > 0 ? cmd_zoom_in : cmd_zoom_out);
            else {
                auto vp = viewport(a);
                int old = a.scroll_y;
                scroll(a, SB_VERT, -1, -MulDiv(delta, px(a, 100), WHEEL_DELTA));
                if (a.scroll_y == old && !a.pages.empty()) {
                    if (delta < 0 && a.page + 1 < int(a.pages.size()))
                        goto_page(a, a.page + 1);
                    else if (delta > 0 && a.page > 0) {
                        goto_page(a, a.page - 1);
                        a.scroll_y = INT_MAX;
                        update_scroll(a);
                    }
                }
                (void)vp;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            auto splitter = sidebar_splitter(a);
            if (PtInRect(&splitter, p)) {
                a.resizing_sidebar = true;
                a.splitter_offset = viewport(a).left - p.x;
                SetCapture(window);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            auto vp = viewport(a);
            if (PtInRect(&vp, p) && a.bitmap) {
                SetFocus(window);
                a.dragging = true;
                a.drag_start = a.drag_end = p;
                a.selected.clear();
                a.selected_all = false;
                SetCapture(window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MBUTTONDOWN:
            a.panning = true;
            a.pan_start = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            a.pan_x = a.scroll_x;
            a.pan_y = a.scroll_y;
            SetCapture(window);
            return 0;
        case WM_MBUTTONUP:
            a.panning = false;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (a.resizing_sidebar) {
                int maximum = std::max(0, int(client(a).right) - px(a, ui::document_min_width));
                int width = std::clamp(int(p.x) + a.splitter_offset,
                                       std::min(px(a, ui::sidebar_min_width), maximum), maximum);
                const int units = MulDiv(width, 96, int(a.dpi));
                if (units != a.sidebar_width) {
                    a.sidebar_width = units;
                    resize_layout(a);
                }
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            auto splitter = sidebar_splitter(a);
            if (PtInRect(&splitter, p)) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return 0;
            }
            if (a.panning) {
                a.scroll_x = a.pan_x - (p.x - a.pan_start.x);
                a.scroll_y = a.pan_y - (p.y - a.pan_start.y);
                update_scroll(a);
                InvalidateRect(window, nullptr, FALSE);
            } else if (a.dragging && a.bitmap) {
                a.drag_end = p;
                RECT selection{std::min(a.drag_start.x, p.x), std::min(a.drag_start.y, p.y),
                               std::max(a.drag_start.x, p.x), std::max(a.drag_start.y, p.y)};
                a.selected.clear();
                for (size_t i = 0; i < a.bitmap->text.size(); ++i) {
                    RECT intersection{}, rect = span_rect(a, a.bitmap->text[i].bounds);
                    if (IntersectRect(&intersection, &selection, &rect))
                        a.selected.push_back(i);
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, a.panning ? IDC_SIZEALL : hit_link(a, p) ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP:
            if (a.resizing_sidebar) {
                a.resizing_sidebar = false;
                ReleaseCapture();
                finish_resize(a);
                return 0;
            }
            if (a.dragging) {
                a.dragging = false;
                ReleaseCapture();
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (std::abs(p.x - a.drag_start.x) + std::abs(p.y - a.drag_start.y) < px(a, 5))
                    if (auto link = hit_link(a, p)) {
                        if (link->page >= 0)
                            goto_page(a, link->page);
                        else {
                            auto uri = lower(link->uri);
                            if (uri.starts_with(L"https://") || uri.starts_with(L"http://") ||
                                uri.starts_with(L"mailto:"))
                                ShellExecuteW(window, L"open", link->uri.c_str(), nullptr, nullptr,
                                              SW_SHOWNORMAL);
                        }
                    }
            }
            return 0;
        case WM_CAPTURECHANGED:
            a.dragging = a.panning = false;
            if (a.resizing_sidebar) {
                a.resizing_sidebar = false;
                finish_resize(a);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                auto splitter = sidebar_splitter(a);
                if (a.resizing_sidebar || PtInRect(&splitter, point)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1000, L"Copy page or selected text\tCtrl+C");
            AppendMenuW(menu, MF_STRING, cmd_print, L"Print…\tCtrl+P");
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p.x == -1)
                GetCursorPos(&p);
            int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (choice == 1000)
                copy(a);
            else if (choice)
                command(a, choice);
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wp == VK_F6) {
                SetFocus(a.toolbar);
                SendMessageW(a.toolbar, TB_SETHOTITEM, 0, 0);
            } else if (ctrl && wp == 'O')
                command(a, cmd_open);
            else if (ctrl && wp == 'F') {
                if (!a.find_open)
                    command(a, cmd_search);
                else
                    SetFocus(a.search);
            } else if (ctrl && wp == 'G')
                command(a, cmd_goto);
            else if (ctrl && wp == 'P')
                command(a, cmd_print);
            else if (ctrl && wp == 'C')
                copy(a);
            else if (ctrl && wp == 'A') {
                a.selected_all = true;
                InvalidateRect(window, nullptr, FALSE);
            } else if (wp == VK_ADD || wp == VK_OEM_PLUS)
                command(a, cmd_zoom_in);
            else if (wp == VK_SUBTRACT || wp == VK_OEM_MINUS)
                command(a, cmd_zoom_out);
            else if (wp == VK_NEXT || wp == VK_RIGHT)
                goto_page(a, a.page + 1);
            else if (wp == VK_PRIOR || wp == VK_LEFT)
                goto_page(a, a.page - 1);
            else if (wp == VK_HOME)
                goto_page(a, 0);
            else if (wp == VK_END)
                goto_page(a, int(a.pages.size()) - 1);
            else if (wp == VK_DOWN)
                scroll(a, SB_VERT, SB_LINEDOWN);
            else if (wp == VK_UP)
                scroll(a, SB_VERT, SB_LINEUP);
            else if (wp == VK_SPACE)
                scroll(a, SB_VERT, SB_PAGEDOWN);
            else if (wp == VK_F3)
                find(a, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            else if (wp == 'R')
                command(a, cmd_rotate);
            else if (wp == 'F')
                command(a, cmd_fit);
            else if (wp == 'W')
                command(a, cmd_width);
            else if (wp == 'B')
                command(a, cmd_bookmarks);
            else if (wp == VK_ESCAPE && a.find_open)
                command(a, cmd_search);
            else if (wp == VK_F1)
                command(a, cmd_about);
            return 0;
        }
        case worker_message:
            receive(a);
            return 0;
        case search_message:
            receive_search(a);
            return 0;
        case WM_CLOSE:
            if (a.update_on_exit && !a.pending_update.directory.empty()) {
                deployment::start_update(a.pending_update, current_session(a), false);
                a.pending_update = {};
                a.update_on_exit = false;
            }
            a.update_cancel = true;
            if (a.update_job.valid()) {
                auto result = a.update_job.get();
                deployment::cleanup_download(result.download);
            }
            deployment::cleanup_download(a.pending_update);
            a.pending_update = {};
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            ui::close_page_view(a.page_view);
            cancel_search(a.search_worker);
            clear_search_results(a);
            stop_search_worker(a.search_worker);
            a.worker.stop = true;
            a.worker.cancel = true;
            a.worker.ready.notify_all();
            if (a.worker.thread.joinable())
                a.worker.thread.join();
            for (auto f : {a.font, a.title_font, a.small_font, a.selected_font, a.search_font})
                if (f)
                    DeleteObject(f);
            if (a.edit_background)
                DeleteObject(a.edit_background);
            if (a.chrome_background)
                DeleteObject(a.chrome_background);
            if (a.results_background)
                DeleteObject(a.results_background);
            for (auto icon : {a.icon, a.small_icon})
                if (icon)
                    DestroyIcon(icon);
            ui::close(a.painter);
            ui::close(a.frame);
            PostQuitMessage(a.exit_code);
            return 0;
        }
    } catch (const std::exception& e) {
        a.status = utf8(e.what());
        if (a.smoke) {
            a.exit_code = 4;
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else
            ui::message_box(window, a.status.c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
    }
    return DefWindowProcW(window, message, wp, lp);
}
struct InstanceLock {
    HANDLE handle = nullptr;
    ~InstanceLock() {
        if (handle)
            CloseHandle(handle);
    }
};
static int run_app(HINSTANCE instance, int show, const std::vector<std::wstring>& arguments) {
    ui::enable_system_dialog_theme();
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS |
                                                        ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                                                        ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const int maintenance = deployment::maintenance_command(arguments);
    if (maintenance >= 0)
        return maintenance;
    deployment::cleanup_stale_helpers();
    if (arguments.size() == 1 && arguments[0] == L"--uninstall") {
        if (!deployment::installation_present())
            throw std::runtime_error("FastPDF is not installed for this Windows account.");
        if (ui::message_box(nullptr, L"Uninstall FastPDF? Your PDF files will be kept.", L"Uninstall FastPDF",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            deployment::start_uninstall();
        return 0;
    }
    App app;
    app.instance = instance;
    app.installed = deployment::installed_instance();
    InstanceLock instance_lock{deployment::acquire_instance_lock()};
    std::wstring initial_path;
    const bool install = arguments.size() == 1 && arguments[0] == L"--install";
    if (!arguments.empty() && arguments[0] == L"--restore-view") {
        const bool capture = arguments.size() == 8 && arguments[6] == L"--screenshot";
        if (arguments.size() != 6 && !capture &&
            !(arguments.size() == 8 && arguments[6] == L"--update-ready" &&
              arguments[7].starts_with(L"Local\\FastPDF.Ready.")))
            throw std::runtime_error("Invalid restored session arguments");
        app.restore = deployment::parse_session({arguments.begin(), arguments.begin() + 6});
        initial_path = app.restore->document;
        if (capture) {
            app.smoke = true;
            app.smoke_output = arguments[7];
        }
    } else if (!install) {
        bool literal = false;
        for (size_t i = 0; i < arguments.size(); ++i) {
            const auto& arg = arguments[i];
            if (!literal && arg == L"--") {
                literal = true;
                continue;
            }
            if (!literal && arg == L"--screenshot" && i + 1 < arguments.size()) {
                app.smoke = true;
                app.smoke_output = arguments[++i];
            } else {
                if ((!literal && arg.starts_with(L"--")) || !initial_path.empty())
                    throw std::runtime_error("Usage: fastpdf [document.pdf] | --install | --uninstall");
                initial_path = arg;
            }
        }
    }
    app.path = initial_path;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"FastPDF.Window";
    RegisterClassExW(&wc);
    HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, cmd_open, L"&Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, cmd_print, L"&Print…\tCtrl+P");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, cmd_exit, L"E&xit\tAlt+F4");
    AppendMenuW(view, MF_STRING, cmd_search, L"&Find…\tCtrl+F");
    AppendMenuW(view, MF_STRING, cmd_goto, L"&Go to page…\tCtrl+G");
    AppendMenuW(view, MF_STRING, cmd_fit, L"Fit &page\tF");
    AppendMenuW(view, MF_STRING, cmd_width, L"Fit &width\tW");
    AppendMenuW(view, MF_STRING, cmd_rotate, L"&Rotate\tR");
    AppendMenuW(view, MF_STRING, cmd_sidebar, L"Page &list");
    AppendMenuW(view, MF_STRING, cmd_bookmarks, L"&Bookmarks\tB");
    AppendMenuW(help, MF_STRING, cmd_about, L"&About…\tF1");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, cmd_install, L"&Installation…");
    AppendMenuW(help, MF_STRING, cmd_updates, L"Check for &updates…");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(file), menu_titles[0]);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(view), menu_titles[1]);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(help), menu_titles[2]);
    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"FastPDF",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1280,
                                  900, nullptr, menu, instance, &app);
    if (!window) {
        return 1;
    }
    ShowWindow(window, app.smoke ? SW_HIDE : show);
    UpdateWindow(window);
    deployment::startup_succeeded(arguments);
    if (install)
        PostMessageW(window, WM_COMMAND, cmd_install, 0);
    if (!initial_path.empty())
        load_file(app, initial_path);
    else if (app.smoke) {
        try {
            screenshot(app);
        } catch (...) {
            app.exit_code = 3;
        }
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_SYSKEYDOWN && app.find_open && IsWindowEnabled(app.window) &&
            (GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000)) {
            if (message.wParam == 'I') {
                SetFocus(GetDlgItem(app.find_bar, IDC_FIND_SCOPE));
                continue;
            }
            const int option = message.wParam == 'C'   ? IDC_FIND_CASE
                               : message.wParam == 'W' ? IDC_FIND_WORDS
                               : message.wParam == 'R' ? IDC_FIND_REGEX
                                                       : 0;
            if (option) {
                SendDlgItemMessageW(app.find_bar, option, BM_CLICK, 0, 0);
                SetFocus(app.search);
                continue;
            }
        }
        if (message.message == WM_KEYDOWN && IsWindowEnabled(app.window)) {
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const auto focus = GetFocus();
            if (message.wParam == VK_RETURN && GetParent(focus) == app.find_bar && focus != app.search &&
                focus != GetDlgItem(app.find_bar, IDC_FIND_SCOPE)) {
                SendMessageW(focus, BM_CLICK, 0, 0);
                continue;
            }
            if (ctrl && message.wParam == 'F') {
                if (!app.find_open)
                    command(app, cmd_search);
                SetFocus(app.search);
                SendMessageW(app.search, EM_SETSEL, 0, -1);
                continue;
            }
            if (message.wParam == VK_F3) {
                if (!app.find_open)
                    command(app, cmd_search);
                else
                    find(app, shift ? -1 : 1);
                continue;
            }
            if (message.wParam == VK_ESCAPE && app.find_open &&
                !SendDlgItemMessageW(app.find_bar, IDC_FIND_SCOPE, CB_GETDROPPEDSTATE, 0, 0)) {
                command(app, cmd_search);
                continue;
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            std::vector<HWND> order{app.window, app.toolbar};
            for (auto control :
                 {app.page_edit, app.zoom_edit, app.search, GetDlgItem(app.find_bar, IDC_FIND_SCOPE),
                  GetDlgItem(app.find_bar, IDC_FIND_CASE), GetDlgItem(app.find_bar, IDC_FIND_WORDS),
                  GetDlgItem(app.find_bar, IDC_FIND_REGEX), GetDlgItem(app.find_bar, IDC_FIND_PREVIOUS),
                  GetDlgItem(app.find_bar, IDC_FIND_NEXT), GetDlgItem(app.find_bar, IDC_FIND_CLOSE),
                  GetDlgItem(app.find_bar, IDC_FIND_STOP), app.sidebar_tabs, app.search_results, app.sidebar,
                  app.bookmark_tree, app.welcome_install, app.welcome_portable})
                if (IsWindowVisible(control) && IsWindowEnabled(control))
                    order.push_back(control);
            auto it = std::find(order.begin(), order.end(), GetFocus());
            int index = it == order.end() ? 0 : int(it - order.begin());
            index =
                (index + ((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1) + int(order.size())) % int(order.size());
            SetFocus(order[size_t(index)]);
            if (order[size_t(index)] == app.toolbar)
                SendMessageW(app.toolbar, TB_SETHOTITEM, 0, 0);
            else if (order[size_t(index)] == app.page_edit || order[size_t(index)] == app.zoom_edit)
                SendMessageW(order[size_t(index)], EM_SETSEL, 0, -1);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return int(message.wParam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        int count = 0;
        auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!raw)
            throw std::runtime_error("Cannot read command-line arguments");
        std::vector<std::wstring> arguments;
        for (int i = 1; i < count; ++i)
            arguments.emplace_back(raw[i]);
        LocalFree(raw);
        result = run_app(instance, show, arguments);
    } catch (const std::exception& error) {
        ui::message_box(nullptr, utf8(error.what()).c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
