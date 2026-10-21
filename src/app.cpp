#include "render.h"
#include "resource.h"
#include "deployment.h"
#include "installation_dialog.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cwctype>
#include <future>
#include <exception>
#include <optional>
#include "ui_style.h"

using namespace pdf;
constexpr UINT worker_message = WM_APP + 1;
constexpr int id_sidebar = 200, id_search = 201, id_toolbar = 202, id_page = 203, id_zoom = 204;
constexpr int page_slot = 100, zoom_slot = 101, toolbar_height = 36;
constexpr const wchar_t* menu_titles[] = {L"&File", L"&View", L"&Help"};
enum class Job { open, render, search, print };
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
    std::wstring path, password, query;
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
    HWND toolbar = nullptr, page_edit = nullptr, zoom_edit = nullptr;
    HWND welcome_install = nullptr, welcome_portable = nullptr;
    HINSTANCE instance = nullptr;
    HICON icon = nullptr, small_icon = nullptr;
    HFONT font = nullptr, title_font = nullptr, small_font = nullptr, selected_font = nullptr;
    HBRUSH edit_background = nullptr, chrome_background = nullptr;
    std::deque<MenuItem> menu_items;
    ui::Painter painter;
    HWND input_hover = nullptr;
    int sidebar_hover = -1;
    Worker worker;
    UINT dpi = 96;
    uint64_t document = 0, ticket = 0, render_ticket = 0, search_ticket = 0;
    std::wstring path, password, status = L"Open a PDF to begin", query;
    std::vector<PageInfo> pages;
    std::vector<Outline> outline;
    std::shared_ptr<RenderedPage> bitmap;
    int page = 0, rotation = 0, scroll_x = 0, scroll_y = 0;
    double scale = 1, bitmap_scale = 1, zoom = 1;
    Fit fit = Fit::page;
    bool show_sidebar = true, bookmarks = false, find_open = false, busy = false, dragging = false,
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
                    if (request.kind == Job::render)
                        result.bitmap = render(request.page, request.scale, request.rotation);
                    else if (request.kind == Job::search) {
                        auto needle = lower(request.query);
                        result.page = -1;
                        for (size_t offset = 0; offset < document.pages.size(); ++offset) {
                            check_cancel(document);
                            size_t page = (size_t(request.page) + offset) % document.pages.size();
                            auto text = render_page(renderer, document, page, 1, 0, true);
                            if (lower(page_text(text)).find(needle) != std::wstring::npos) {
                                result.page = int(page);
                                result.message = L"Match on page " + std::to_wstring(page + 1);
                                break;
                            }
                        }
                        if (result.page < 0)
                            result.message = L"No matches for “" + request.query + L"”";
                    } else if (request.kind == Job::print) {
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
    if (request.kind == Job::search)
        a.search_ticket = request.ticket;
    {
        std::lock_guard lock(a.worker.mutex);
        if (!a.worker.io_active)
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
static RECT viewport(App& a) {
    RECT r = client(a);
    r.top = px(a, toolbar_height + (a.find_open ? 40 : 0));
    r.bottom -= px(a, 30);
    r.left = a.show_sidebar && !a.pages.empty() ? px(a, ui::sidebar_width) : 0;
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
static void update_scroll(App& a) {
    auto r = viewport(a);
    auto [w, h] = dimensions(a);
    int width = std::max<LONG>(1, r.right - r.left), height = std::max<LONG>(1, r.bottom - r.top),
        margin = px(a, 24);
    int total_w = int(std::ceil(w * a.scale)) + 2 * margin,
        total_h = int(std::ceil(h * a.scale)) + 2 * margin;
    a.scroll_x = std::clamp(a.scroll_x, 0, std::max(0, total_w - width));
    a.scroll_y = std::clamp(a.scroll_y, 0, std::max(0, total_h - height));
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMin = 0;
    si.nMax = std::max(0, total_h - 1);
    si.nPage = UINT(height);
    si.nPos = a.scroll_y;
    SetScrollInfo(a.window, SB_VERT, &si, TRUE);
    si.nMax = std::max(0, total_w - 1);
    si.nPage = UINT(width);
    si.nPos = a.scroll_x;
    SetScrollInfo(a.window, SB_HORZ, &si, TRUE);
}
static void request_render(App& a, bool reset_scroll = false) {
    if (a.pages.empty()) {
        sync_toolbar(a);
        return;
    }
    auto r = viewport(a);
    auto [w, h] = dimensions(a);
    double available_w = std::max<LONG>(32, r.right - r.left - px(a, 48)),
           available_h = std::max<LONG>(32, r.bottom - r.top - px(a, 48));
    if (a.fit == Fit::page)
        a.scale = std::min(available_w / w, available_h / h);
    else if (a.fit == Fit::width)
        a.scale = available_w / w;
    else
        a.scale = a.zoom * double(a.dpi) / 72;
    a.scale = std::clamp(a.scale, 0.05, 12.0);
    double pixels = w * h * a.scale * a.scale;
    if (pixels > 60'000'000)
        a.scale *= std::sqrt(60'000'000 / pixels);
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
            a.menu_items.push_back({std::move(title), std::move(shortcut), bar,
                                    (item.fType & MFT_SEPARATOR) != 0});
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
    SendMessageW(a.search, WM_SETFONT, WPARAM(a.small_font), TRUE);
    for (auto control : {a.welcome_install, a.welcome_portable})
        SendMessageW(control, WM_SETFONT, WPARAM(a.font), TRUE);
    for (auto control : {a.toolbar, a.page_edit, a.zoom_edit})
        SendMessageW(control, WM_SETFONT, WPARAM(a.small_font), TRUE);
    style_menu(a, GetMenu(a.window), true);
    DrawMenuBar(a.window);
}
static void layout(App& a) {
    auto r = client(a);
    int top = px(a, toolbar_height + (a.find_open ? 40 : 0));
    int list_top = top + px(a, ui::sidebar_header);
    MoveWindow(a.sidebar, 0, list_top, px(a, ui::sidebar_width) - 1,
               std::max<LONG>(0, r.bottom - list_top - px(a, 30)), TRUE);
    ShowWindow(a.sidebar, a.show_sidebar && !a.pages.empty() ? SW_SHOW : SW_HIDE);
    MoveWindow(a.search, px(a, 18), px(a, toolbar_height + 11),
               std::max<LONG>(80, std::min<LONG>(px(a, 420), r.right - px(a, 140))), px(a, 18), TRUE);
    ShowWindow(a.search, a.find_open ? SW_SHOW : SW_HIDE);
    const auto vp = viewport(a);
    const int midpoint = (vp.left + vp.right) / 2, center = (vp.top + vp.bottom) / 2;
    const bool welcome = !a.installed && !a.portable_choice && a.path.empty() && a.pages.empty();
    MoveWindow(a.welcome_install, midpoint - px(a, 188), center + px(a, 12), px(a, 180), px(a, 38), TRUE);
    MoveWindow(a.welcome_portable, midpoint + px(a, 8), center + px(a, 12), px(a, 180), px(a, 38), TRUE);
    ShowWindow(a.welcome_install, welcome ? SW_SHOW : SW_HIDE);
    ShowWindow(a.welcome_portable, welcome ? SW_SHOW : SW_HIDE);
    update_scroll(a);
    layout_toolbar(a);
    sync_toolbar(a);
}
static void update_sidebar(App& a) {
    a.sidebar_hover = -1;
    SendMessageW(a.sidebar, LB_SETCOUNT, WPARAM(a.bookmarks ? a.outline.size() : a.pages.size()), 0);
    if (!a.bookmarks)
        SendMessageW(a.sidebar, LB_SETCURSEL, WPARAM(a.page), 0);
    InvalidateRect(a.sidebar, nullptr, TRUE);
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
    a.bitmap.reset();
    a.selected.clear();
    a.find_open = false;
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
struct FileSelection {
    std::wstring extended_path;
    std::exception_ptr error;
};
static LRESULT CALLBACK file_dialog_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR subclass,
                                         DWORD_PTR data) {
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, file_dialog_proc, subclass);
    if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
        auto& selection = *reinterpret_cast<FileSelection*>(data);
        try {
            const LRESULT size = SendMessageW(window, CDM_GETSPEC, 0, 0);
            if (size > 0 && size <= 32768) {
                std::wstring path(size_t(size), L'\0');
                const LRESULT copied =
                    SendMessageW(window, CDM_GETSPEC, WPARAM(path.size()), LPARAM(path.data()));
                if (copied > 0 && copied <= size) {
                    path.resize(wcslen(path.c_str()));
                    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
                        path = path.substr(1, path.size() - 2);
                    if (path.starts_with(L"\\\\?\\")) {
                        // The shell treats '?' as a wildcard. Preserve the exact path for file I/O.
                        selection.extended_path = std::move(path);
                        SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
                        return 0;
                    }
                }
            }
        } catch (...) {
            selection.error = std::current_exception();
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
            return 0;
        }
    }
    return DefSubclassProc(window, message, wp, lp);
}
static UINT_PTR CALLBACK file_dialog_hook(HWND window, UINT message, WPARAM, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto& dialog = *reinterpret_cast<OPENFILENAMEW*>(lp);
        SetWindowSubclass(GetParent(window), file_dialog_proc, 1, DWORD_PTR(dialog.lCustData));
    }
    return 0;
}
static void choose_file(App& a) {
    wchar_t path[32768]{};
    FileSelection selection;
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = a.window;
    dialog.lpstrFilter = L"PDF documents (*.pdf)\0*.pdf\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ENABLEHOOK;
    dialog.lpfnHook = file_dialog_hook;
    dialog.lCustData = LPARAM(&selection);
    const bool accepted = GetOpenFileNameW(&dialog) != FALSE;
    if (selection.error)
        std::rethrow_exception(selection.error);
    if (!selection.extended_path.empty())
        load_file(a, selection.extended_path);
    else if (accepted)
        load_file(a, path);
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
static void theme_dialog(HWND window, LPARAM context) {
    SetWindowLongPtrW(window, DWLP_USER, context);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &ui::chrome, sizeof(ui::chrome));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &ui::text, sizeof(ui::text));
}
static INT_PTR dialog_theme_message(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    const auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, DWLP_USER));
    if (!app)
        return FALSE;
    if (message == WM_CTLCOLORDLG || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT) {
        SetBkColor(HDC(wp), ui::chrome);
        SetTextColor(HDC(wp), GetDlgCtrlID(HWND(lp)) == IDC_REPOSITORY ? ui::link : ui::text);
        return reinterpret_cast<INT_PTR>(app->chrome_background);
    }
    if (message == WM_NOTIFY) {
        const auto header = reinterpret_cast<const NMHDR*>(lp);
        if (header->code == NM_CUSTOMDRAW && (header->idFrom == IDOK || header->idFrom == IDC_NOTICES)) {
            const auto draw = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                const bool focused = (draw->uItemState & CDIS_FOCUS) != 0;
                const auto fill = (draw->uItemState & CDIS_SELECTED) ? ui::selected
                                  : (draw->uItemState & CDIS_HOT) ? ui::hover : ui::surface;
                ui::control(app->painter, draw->hdc, draw->rc, GetDpiForWindow(window), ui::chrome, fill,
                            focused ? ui::focus : ui::border, 1);
                wchar_t text[128]{};
                GetWindowTextW(header->hwndFrom, text, int(std::size(text)));
                UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
                if (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL)
                    flags |= DT_HIDEPREFIX;
                label(draw->hdc, draw->rc, text, ui::text,
                      HFONT(SendMessageW(header->hwndFrom, WM_GETFONT, 0, 0)), flags);
                SetWindowLongPtrW(window, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
                return TRUE;
            }
        }
    }
    return FALSE;
}
static INT_PTR CALLBACK licenses_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        theme_dialog(window, lp);
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
        return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        EndDialog(window, LOWORD(wp));
        return TRUE;
    }
    return dialog_theme_message(window, message, wp, lp);
}
static INT_PTR CALLBACK about_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        theme_dialog(window, lp);
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
    return dialog_theme_message(window, message, wp, lp);
}
static INT_PTR CALLBACK goto_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        SetWindowLongPtrW(window, DWLP_USER, lp);
        auto a = reinterpret_cast<App*>(lp);
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
static void find(App& a, bool next = false) {
    wchar_t text[4096]{};
    GetWindowTextW(a.search, text, 4096);
    a.query = text;
    if (a.query.empty() || a.pages.empty())
        return;
    a.status = L"Searching…";
    a.busy = true;
    Request request;
    request.kind = Job::search;
    request.page = next ? (a.page + 1) % int(a.pages.size()) : a.page;
    request.query = a.query;
    enqueue(a, std::move(request));
    InvalidateRect(a.window, nullptr, FALSE);
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
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hwndParent = a.window;
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (checked && *checked)
        dialog.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = L"FastPDF";
    dialog.pszMainInstruction = title;
    dialog.pszContent = content.c_str();
    dialog.pszVerificationText = checkbox;
    dialog.cButtons = UINT(buttons.size());
    dialog.pButtons = buttons.data();
    dialog.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    if (FAILED(TaskDialogIndirect(&dialog, &pressed, nullptr, checked)))
        throw std::runtime_error("Cannot open the FastPDF dialog");
    return pressed;
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
            MessageBoxW(a.window, L"Installation options saved.", L"FastPDF", MB_OK | MB_ICONINFORMATION);
    } else if (result == ui::InstallationAction::open) {
        deployment::open_installed(current_session(a));
        PostMessageW(a.window, WM_CLOSE, 0, 0);
    } else if (result == ui::InstallationAction::uninstall) {
        if (MessageBoxW(a.window, L"Uninstall FastPDF for your Windows account? Your PDF files will be kept.",
                        L"Uninstall FastPDF", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            deployment::start_uninstall();
            PostMessageW(a.window, WM_CLOSE, 0, 0);
        }
    }
}
static void ready_update(App& a) {
    const int result = task_dialog(
        a, L"Your update is ready",
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
        MessageBoxW(a.window, result.error.c_str(), L"FastPDF update", MB_OK | MB_ICONERROR);
    } else if (result.downloading) {
        a.pending_update = std::move(result.download);
        a.update_status = L"Update ready. Help → Check for updates to install.";
        ready_update(a);
    } else if (result.release.version <= deployment::parse_version(FASTPDF_VERSION)) {
        MessageBoxW(a.window, L"FastPDF " FASTPDF_VERSION_W L" is up to date.", L"FastPDF update",
                    MB_OK | MB_ICONINFORMATION);
    } else if (task_dialog(a, L"A FastPDF update is available",
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
            MessageBoxW(a.window,
                        L"Install FastPDF to update it here. You can also download a new portable copy from "
                        L"GitHub Releases.",
                        L"FastPDF updates", MB_OK | MB_ICONINFORMATION);
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
        a.show_sidebar = !(a.show_sidebar && a.bookmarks == bookmarks);
        a.bookmarks = bookmarks;
        update_sidebar(a);
        layout(a);
        request_render(a);
    } else if (id == cmd_search) {
        a.find_open = !a.find_open;
        layout(a);
        if (a.find_open) {
            SetFocus(a.search);
            SendMessageW(a.search, EM_SETSEL, 0, -1);
        } else {
            SetFocus(a.window);
            a.query.clear();
        }
        request_render(a);
    } else if (id == cmd_goto && !a.pages.empty())
        DialogBoxParamW(a.instance, MAKEINTRESOURCEW(IDD_GOTO), a.window, goto_dialog, LPARAM(&a));
    else if (id == cmd_print && !a.pages.empty()) {
        PRINTDLGW dialog{sizeof(dialog)};
        dialog.hwndOwner = a.window;
        dialog.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE | PD_NOSELECTION;
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

static std::wstring page_number(const App& a) {
    return a.pages.empty() ? L"—" : std::to_wstring(a.page + 1);
}
static std::wstring zoom_percent(const App& a) {
    return a.pages.empty() ? L"—" : std::to_wstring(int(std::lround(a.scale * 72 / a.dpi * 100))) + L"%";
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
        SendMessageW(a.toolbar, TB_ENABLEBUTTON, WPARAM(tool.command), MAKELPARAM(enabled, 0));
    }
    for (auto [id, checked] : {std::pair{cmd_fit, loaded && a.fit == Fit::page},
                               {cmd_width, loaded && a.fit == Fit::width},
                               {cmd_search, loaded && a.find_open},
                               {cmd_sidebar, loaded && a.show_sidebar && !a.bookmarks},
                               {cmd_bookmarks, loaded && a.show_sidebar && a.bookmarks}})
        SendMessageW(a.toolbar, TB_CHECKBUTTON, WPARAM(id), MAKELPARAM(checked, 0));
    EnableWindow(a.page_edit, loaded);
    EnableWindow(a.zoom_edit, loaded);
    if (GetFocus() != a.page_edit)
        SetWindowTextW(a.page_edit, page_number(a).c_str());
    if (GetFocus() != a.zoom_edit)
        SetWindowTextW(a.zoom_edit, zoom_percent(a).c_str());
    InvalidateRect(a.toolbar, nullptr, FALSE);
}

static void layout_toolbar(App& a) {
    if (!a.toolbar)
        return;
    auto r = client(a);
    MoveWindow(a.toolbar, 0, 0, r.right, px(a, toolbar_height), TRUE);
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
    MoveWindow(a.page_edit, page.left + px(a, 5), px(a, 9), px(a, 42), px(a, 18), TRUE);
    MoveWindow(a.zoom_edit, zoom.left + px(a, 5), px(a, 9), px(a, 50), px(a, 18), TRUE);
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
            COLORREF background = active ? ui::selected : hot && enabled ? ui::hover : ui::chrome;
            COLORREF color = !enabled ? ui::disabled : active || hot ? ui::text : ui::muted;
            ui::control(a.painter, dc, rect, a.dpi, ui::chrome, background, ui::focus,
                        (state & CDIS_FOCUS) ? 2 : 0, tool.icon, color);
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
    COLORREF outline = focused ? ui::focus : a.input_hover == input ? ui::muted : ui::border;
    ui::control(a.painter, dc, rect, a.dpi, ui::chrome, ui::surface, outline, focused ? 2 : 1);
}

static void paint_sidebar_item(App& a, HDC dc, RECT rect, int index, bool selected, bool focused) {
    fill_rect(dc, rect, ui::chrome);
    RECT row = rect;
    InflateRect(&row, -px(a, 8), -px(a, 2));
    COLORREF fill = selected ? ui::selected : index == a.sidebar_hover ? ui::hover : ui::chrome;
    ui::control(a.painter, dc, row, a.dpi, ui::chrome, fill, ui::focus, focused ? 2 : 0);
    rect.left = row.left + px(a, 10);
    rect.right = row.right - px(a, 10);
    std::wstring text;
    if (a.bookmarks && size_t(index) < a.outline.size()) {
        const auto& outline = a.outline[size_t(index)];
        text = outline.title;
        rect.left += px(a, std::min(outline.depth, 5) * 12);
    } else
        text = L"Page " + std::to_wstring(index + 1);
    label(dc, rect, text, selected || index == a.sidebar_hover ? ui::text : ui::muted,
          selected ? a.selected_font : a.font);
}

static void paint_toolbar(App& a, HDC dc) {
    auto r = client(a);
    fill_rect(dc, {0, 0, r.right, px(a, toolbar_height)}, ui::chrome);
    fill_rect(dc, {0, px(a, toolbar_height) - 1, r.right, px(a, toolbar_height)}, ui::border);
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
            label(dc, rect, a.pages.empty() ? L"/ —" : L"/ " + std::to_wstring(a.pages.size()), ui::muted,
                  a.small_font);
        } else if (button.idCommand == zoom_slot) {
            continue;
        } else if (button.fsStyle & BTNS_SEP) {
            int x = (rect.left + rect.right) / 2;
            fill_rect(dc, {x, px(a, 8), x + 1, px(a, 28)}, ui::border);
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
static void paint_app(App& a, HDC dc) {
    auto r = client(a);
    auto vp = viewport(a);
    fill_rect(dc, r, ui::canvas);
    paint_toolbar(a, dc);
    if (a.find_open) {
        fill_rect(dc, {0, px(a, toolbar_height), r.right, px(a, toolbar_height + 40)}, ui::chrome);
        label(dc, {px(a, 454), px(a, toolbar_height), r.right - px(a, 12), px(a, toolbar_height + 40)},
              L"Enter to find · F3 next page · Esc to close", ui::muted, a.small_font);
        paint_input_frame(a, dc, a.window, a.search);
    }
    if (a.show_sidebar && !a.pages.empty()) {
        fill_rect(dc, {0, vp.top, vp.left, vp.bottom}, ui::chrome);
        fill_rect(dc, {vp.left - 1, vp.top, vp.left, vp.bottom}, ui::border);
        label(dc, {px(a, 18), vp.top, vp.left - px(a, 12), vp.top + px(a, ui::sidebar_header)},
              a.bookmarks ? L"Bookmarks" : L"Pages", ui::muted, a.small_font);
    }
    int saved = SaveDC(dc);
    IntersectClipRect(dc, vp.left, vp.top, vp.right, vp.bottom);
    if (a.pages.empty()) {
        int center = (vp.top + vp.bottom) / 2;
        constexpr std::wstring_view prompt = L"Drop a PDF here, or press Ctrl+O to open one.";
        RECT heading{vp.left + px(a, 40), 0, vp.right - px(a, 40), 0};
        auto old_font = SelectObject(dc, a.title_font);
        DrawTextW(dc, prompt.data(), int(prompt.size()), &heading, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        SelectObject(dc, old_font);
        // Keep the original headline's center, allowing the prompt to wrap in narrow windows.
        int height = heading.bottom - heading.top;
        heading.left = vp.left + px(a, 40);
        heading.right = vp.right - px(a, 40);
        heading.top = center - px(a, 67) - height / 2;
        heading.bottom = heading.top + height;
        label(dc, heading, prompt, ui::text, a.title_font, DT_CENTER | DT_WORDBREAK);
    } else {
        RECT page = page_rect(a);
        RECT shadow = page;
        OffsetRect(&shadow, px(a, 3), px(a, 5));
        fill_rect(dc, shadow, ui::shadow);
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
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, page.left, page.top, page.right - page.left, page.bottom - page.top, 0, 0,
                          int(a.bitmap->width), int(a.bitmap->height), a.bitmap->pixels.data(), &bmi,
                          DIB_RGB_COLORS, SRCCOPY);
            auto needle = lower(a.query);
            for (size_t i = 0; i < a.bitmap->text.size(); ++i) {
                auto& span = a.bitmap->text[i];
                bool selected =
                    a.selected_all || std::find(a.selected.begin(), a.selected.end(), i) != a.selected.end();
                if (selected)
                    highlight(dc, span_rect(a, span.bounds), 0x5010224b);
                else if (!needle.empty() && lower(span.text).find(needle) != std::wstring::npos)
                    highlight(dc, span_rect(a, span.bounds), 0x60605000);
            }
        } else
            label(dc, page, L"Rendering…", ui::muted, a.font, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RestoreDC(dc, saved);
    RECT status{0, r.bottom - px(a, 30), r.right, r.bottom};
    fill_rect(dc, status, ui::chrome);
    label(dc, {px(a, 14), status.top, r.right - px(a, 250), status.bottom},
          a.update_status.empty() ? a.status : a.update_status, ui::muted, a.small_font);
    if (!a.pages.empty()) {
        std::wstring details =
            L"Page " + std::to_wstring(a.page + 1) + L" of " + std::to_wstring(a.pages.size());
        label(dc, {r.right - px(a, 240), status.top, r.right - px(a, 14), status.bottom}, details, ui::muted,
              a.small_font, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
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
    for (auto control : {a.page_edit, a.zoom_edit, a.search}) {
        if (control == a.search && !a.find_open)
            continue;
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, a.window, reinterpret_cast<POINT*>(&rect), 2);
        int saved = SaveDC(memory);
        SetViewportOrgEx(memory, rect.left, rect.top, nullptr);
        SendMessageW(control, WM_PRINT, WPARAM(memory), PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
        RestoreDC(memory, saved);
    }
    if (a.show_sidebar && !a.pages.empty()) {
        auto vp = viewport(a);
        int top = vp.top + px(a, ui::sidebar_header);
        int first = int(SendMessageW(a.sidebar, LB_GETTOPINDEX, 0, 0));
        int count = int(SendMessageW(a.sidebar, LB_GETCOUNT, 0, 0));
        int selected = int(SendMessageW(a.sidebar, LB_GETCURSEL, 0, 0));
        int saved = SaveDC(memory);
        IntersectClipRect(memory, 0, top, vp.left - 1, vp.bottom);
        for (int i = std::max(0, first); i < count && top < vp.bottom; ++i) {
            paint_sidebar_item(a, memory, {0, top, vp.left - 1, top + px(a, ui::row_height)}, i,
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
        if (result.kind == Job::render && result.ticket != a.render_ticket)
            continue;
        if (result.kind == Job::search && result.ticket != a.search_ticket)
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
                MessageBoxW(a.window, result.error.c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
            continue;
        }
        if (result.kind == Job::open) {
            a.pages = std::move(result.pages);
            a.outline = std::move(result.outline);
            a.status = result.message;
            if (a.restore) {
                a.page = std::clamp(a.restore->page, 0, std::max(0, int(a.pages.size()) - 1));
                a.rotation = a.restore->rotation;
                a.fit = Fit(a.restore->fit);
                a.zoom = a.restore->zoom;
                a.restore.reset();
            }
            update_sidebar(a);
            layout(a);
            request_render(a, true);
        } else if (result.kind == Job::render) {
            a.bitmap = std::move(result.bitmap);
            a.bitmap_scale = result.scale;
            a.status = std::filesystem::path(a.path).filename().wstring();
            if (a.smoke) {
                try {
                    screenshot(a);
                } catch (...) {
                    a.exit_code = 3;
                }
                PostMessageW(a.window, WM_CLOSE, 0, 0);
            }
        } else if (result.kind == Job::search) {
            a.status = result.message;
            if (result.page >= 0 && a.page != result.page)
                goto_page(a, result.page);
            else if (!a.bitmap)
                request_render(a);
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
    GetScrollInfo(a.window, bar, &info);
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
    if (message == WM_MOUSEMOVE) {
        DWORD item = DWORD(SendMessageW(window, LB_ITEMFROMPOINT, 0, lp));
        int hover = HIWORD(item) ? -1 : int(LOWORD(item));
        if (hover != a.sidebar_hover) {
            a.sidebar_hover = hover;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
    } else if (message == WM_MOUSELEAVE || message == WM_VSCROLL || message == WM_MOUSEWHEEL) {
        a.sidebar_hover = -1;
        InvalidateRect(window, nullptr, FALSE);
    } else if (message == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            SetFocus(a.window);
            return 0;
        }
        if (wp == VK_F6 || ((GetKeyState(VK_CONTROL) & 0x8000) &&
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
static LRESULT CALLBACK search_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR,
                                    DWORD_PTR data) {
    auto& a = *reinterpret_cast<App*>(data);
    if (message == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            find(a);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            command(a, cmd_search);
            return 0;
        }
        if (wp == VK_F3) {
            find(a, true);
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
    SendMessageW(tips, TTM_SETTIPBKCOLOR, ui::surface, 0);
    SendMessageW(tips, TTM_SETTIPTEXTCOLOR, ui::text, 0);
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
        auto icon = static_cast<HICON>(LoadImageW(
            a.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
            GetSystemMetricsForDpi(large ? SM_CXICON : SM_CXSMICON, a.dpi),
            GetSystemMetricsForDpi(large ? SM_CYICON : SM_CYSMICON, a.dpi), LR_DEFAULTCOLOR));
        require(icon != nullptr, "Cannot load application icon");
        SendMessageW(a.window, WM_SETICON, WPARAM(kind), LPARAM(icon));
        if (*current)
            DestroyIcon(*current);
        *current = icon;
    }
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
            BOOL dark = TRUE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &ui::chrome, sizeof(ui::chrome));
            DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &ui::text, sizeof(ui::text));
            a.dpi = GetDpiForWindow(window);
            window_icons(a);
            a.sidebar = CreateWindowExW(0, L"LISTBOX", L"Pages",
                                        WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NODATA | LBS_OWNERDRAWFIXED |
                                            LBS_NOINTEGRALHEIGHT,
                                        0, 0, 0, 0, window, HMENU(INT_PTR(id_sidebar)), a.instance, nullptr);
            SetWindowSubclass(a.sidebar, sidebar_proc, 1, DWORD_PTR(&a));
            a.search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 0, 0,
                                       window, HMENU(INT_PTR(id_search)), a.instance, nullptr);
            SendMessageW(a.search, EM_SETCUEBANNER, TRUE, LPARAM(L"Find in document"));
            SendMessageW(a.search, EM_LIMITTEXT, 4095, 0);
            SetWindowSubclass(a.search, search_proc, 1, DWORD_PTR(&a));
            SetWindowSubclass(a.search, input_frame_proc, 2, DWORD_PTR(&a));
            create_toolbar(a);
            a.welcome_install =
                CreateWindowExW(0, L"BUTTON", L"Install FastPDF", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
                                0, 0, window, HMENU(INT_PTR(cmd_install)), a.instance, nullptr);
            a.welcome_portable =
                CreateWindowExW(0, L"BUTTON", L"Continue portable", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0,
                                0, 0, 0, window, HMENU(INT_PTR(cmd_portable)), a.instance, nullptr);
            require(a.welcome_install && a.welcome_portable, "Cannot create welcome controls");
            a.edit_background = CreateSolidBrush(ui::surface);
            a.chrome_background = CreateSolidBrush(ui::chrome);
            fonts(a);
            DragAcceptFiles(window, TRUE);
            a.worker.window = window;
            a.worker.thread = std::thread(worker_run, std::ref(a.worker));
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
                fill_rect(dc, divider, ui::border);
                ReleaseDC(window, dc);
            }
            return result;
        }
        case WM_NOTIFY: {
            auto header = reinterpret_cast<NMHDR*>(lp);
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
            auto r = client(a);
            HDC memory = CreateCompatibleDC(dc);
            HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, r.right), std::max(1L, r.bottom));
            auto old = SelectObject(memory, bitmap);
            paint_app(a, memory);
            BitBlt(dc, 0, 0, r.right, r.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old);
            DeleteObject(bitmap);
            DeleteDC(memory);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                layout(a);
                if (!a.pages.empty())
                    SetTimer(window, 1, 120, nullptr);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_TIMER:
            if (wp == 2)
                poll_update(a);
            else if (wp == 1) {
                KillTimer(window, 1);
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
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize = {px(a, 640), px(a, 480)};
            return 0;
        }
        case WM_CTLCOLORLISTBOX:
            SetBkColor(HDC(wp), ui::chrome);
            return LRESULT(a.chrome_background);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            SetBkColor(HDC(wp), ui::surface);
            SetTextColor(HDC(wp), IsWindowEnabled(HWND(lp)) ? ui::text : ui::disabled);
            return LRESULT(a.edit_background);
        case WM_MEASUREITEM: {
            auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
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
            DrawTextW(dc, data.title.c_str(), -1, &bounds,
                      DT_SINGLELINE | DT_CALCRECT);
            DrawTextW(dc, data.shortcut.c_str(), -1, &shortcut,
                      DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
            SelectObject(dc, old);
            ReleaseDC(window, dc);
            item->itemWidth = UINT(bounds.right + shortcut.right + px(a, data.bar ? 16 : 64));
            item->itemHeight = UINT(data.bar
                ? std::max(bounds.bottom + px(a, 6), LONG(GetSystemMetricsForDpi(SM_CYMENU, a.dpi)))
                : std::max(bounds.bottom + px(a, 12), LONG(px(a, 30))));
            return TRUE;
        }
        case WM_MENUCHAR: {
            HMENU menu = HMENU(lp);
            for (int index = 0; index < GetMenuItemCount(menu); ++index) {
                MENUITEMINFOW item{sizeof(item)};
                item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_STATE;
                if (!GetMenuItemInfoW(menu, UINT(index), TRUE, &item) ||
                    !(item.fType & MFT_OWNERDRAW) || !item.dwItemData ||
                    (item.fState & (MFS_DISABLED | MFS_GRAYED)))
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
            if (item->CtlType == ODT_MENU && item->itemData) {
                const auto& data = *reinterpret_cast<const MenuItem*>(item->itemData);
                int saved = SaveDC(item->hDC);
                bool disabled = (item->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
                COLORREF background = data.bar ? ui::chrome : ui::surface;
                if (!disabled && !data.separator) {
                    if (item->itemState & ODS_SELECTED)
                        background = ui::selected;
                    else if (item->itemState & ODS_HOTLIGHT)
                        background = ui::hover;
                }
                fill_rect(item->hDC, item->rcItem, background);
                RECT bounds = item->rcItem;
                if (data.separator) {
                    bounds.left += px(a, 12);
                    bounds.right -= px(a, 12);
                    bounds.top = (bounds.top + bounds.bottom) / 2;
                    bounds.bottom = bounds.top + 1;
                    fill_rect(item->hDC, bounds, ui::border);
                    RestoreDC(item->hDC, saved);
                    return TRUE;
                }
                UINT flags = (data.bar ? DT_CENTER : DT_LEFT) | DT_SINGLELINE | DT_VCENTER;
                if (item->itemState & ODS_NOACCEL)
                    flags |= DT_HIDEPREFIX;
                if (!data.bar) {
                    bounds.left += px(a, 16);
                    bounds.right -= px(a, 16);
                    label(item->hDC, bounds, data.shortcut, disabled ? ui::disabled : ui::muted,
                          a.font, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                label(item->hDC, bounds, data.title, disabled ? ui::disabled : ui::text, a.font, flags);
                RestoreDC(item->hDC, saved);
                return TRUE;
            }
            if (item->CtlID == cmd_install || item->CtlID == cmd_portable) {
                const bool focused = (item->itemState & ODS_FOCUS) != 0;
                const COLORREF fill = (item->itemState & ODS_SELECTED) ? ui::selected : ui::surface;
                ui::control(a.painter, item->hDC, item->rcItem, a.dpi, ui::canvas, fill,
                            focused ? ui::focus : ui::border, 1);
                label(item->hDC, item->rcItem,
                      item->CtlID == cmd_install ? L"Install FastPDF" : L"Continue portable", ui::text,
                      a.font, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                return TRUE;
            }
            if (item->CtlID != id_sidebar || item->itemID == UINT(-1))
                break;
            paint_sidebar_item(a, item->hDC, item->rcItem, int(item->itemID),
                               (item->itemState & ODS_SELECTED) != 0,
                               (item->itemState & ODS_FOCUS) != 0 && GetFocus() == a.sidebar);
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
                    int page = a.bookmarks
                                   ? (size_t(index) < a.outline.size() ? a.outline[size_t(index)].page : -1)
                                   : index;
                    if (page >= 0)
                        goto_page(a, page);
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
            return 0;
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
                find(a, true);
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
            a.worker.stop = true;
            a.worker.cancel = true;
            a.worker.ready.notify_all();
            if (a.worker.thread.joinable())
                a.worker.thread.join();
            for (auto f : {a.font, a.title_font, a.small_font, a.selected_font})
                if (f)
                    DeleteObject(f);
            if (a.edit_background)
                DeleteObject(a.edit_background);
            if (a.chrome_background)
                DeleteObject(a.chrome_background);
            for (auto icon : {a.icon, a.small_icon})
                if (icon)
                    DestroyIcon(icon);
            ui::close(a.painter);
            PostQuitMessage(a.exit_code);
            return 0;
        }
    } catch (const std::exception& e) {
        a.status = utf8(e.what());
        if (a.smoke) {
            a.exit_code = 4;
            PostMessageW(window, WM_CLOSE, 0, 0);
        } else
            MessageBoxW(window, a.status.c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
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
    const int maintenance = deployment::maintenance_command(arguments);
    if (maintenance >= 0)
        return maintenance;
    deployment::cleanup_stale_helpers();
    if (arguments.size() == 1 && arguments[0] == L"--uninstall") {
        if (!deployment::installation_present())
            throw std::runtime_error("FastPDF is not installed for this Windows account.");
        if (MessageBoxW(nullptr, L"Uninstall FastPDF? Your PDF files will be kept.", L"Uninstall FastPDF",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            deployment::start_uninstall();
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
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
                                  WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900, nullptr, menu, instance, &app);
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
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            std::vector<HWND> order{app.window, app.toolbar};
            for (auto control : {app.page_edit, app.zoom_edit, app.search, app.sidebar, app.welcome_install,
                                 app.welcome_portable})
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
        MessageBoxW(nullptr, utf8(error.what()).c_str(), L"FastPDF", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
