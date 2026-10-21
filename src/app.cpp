#include "render.h"
#include "resource.h"
#include "deployment.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cwctype>
#include <future>
#include <optional>

using namespace pdf;
constexpr UINT worker_message = WM_APP + 1;
constexpr int id_sidebar = 200, id_search = 201;
enum class Job { open, render, search, export_png, print };
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
struct Button {
    RECT rect{};
    int command = 0;
    std::wstring label;
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
    cmd_export,
    cmd_print,
    cmd_info,
    cmd_goto,
    cmd_licenses,
    cmd_install,
    cmd_updates,
    cmd_cancel_update,
    cmd_portable
};
struct UpdateResult {
    deployment::Release release;
    deployment::Download download;
    std::wstring error;
    bool downloading = false;
};
struct App {
    HWND window = nullptr, sidebar = nullptr, search = nullptr;
    HINSTANCE instance = nullptr;
    HFONT font = nullptr, title_font = nullptr, small_font = nullptr;
    HBRUSH edit_background = nullptr;
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
    std::vector<Button> buttons;
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
                w.io_active = request.kind == Job::print || request.kind == Job::export_png;
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
                    } else if (request.kind == Job::export_png) {
                        auto image = render(request.page, request.scale, request.rotation);
                        save_png(renderer, *image, request.path);
                        result.message = L"Saved " + request.path;
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
    r.top = px(a, a.find_open ? 104 : 64);
    r.bottom -= px(a, 30);
    r.left = a.show_sidebar && !a.pages.empty() ? px(a, 220) : 0;
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
    if (a.pages.empty())
        return;
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
    InvalidateRect(a.window, nullptr, FALSE);
}
static void fonts(App& a) {
    for (auto f : {a.font, a.title_font, a.small_font})
        if (f)
            DeleteObject(f);
    a.font =
        CreateFontW(-px(a, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    a.title_font =
        CreateFontW(-px(a, 28), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    a.small_font =
        CreateFontW(-px(a, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(a.sidebar, WM_SETFONT, WPARAM(a.font), TRUE);
    SendMessageW(a.sidebar, LB_SETITEMHEIGHT, 0, px(a, 40));
    SendMessageW(a.search, WM_SETFONT, WPARAM(a.font), TRUE);
}
static void layout(App& a) {
    auto r = client(a);
    int top = px(a, a.find_open ? 104 : 64);
    MoveWindow(a.sidebar, 0, top, px(a, 220), std::max<LONG>(0, r.bottom - top - px(a, 30)), TRUE);
    ShowWindow(a.sidebar, a.show_sidebar && !a.pages.empty() ? SW_SHOW : SW_HIDE);
    MoveWindow(a.search, px(a, 18), px(a, 70),
               std::max<LONG>(80, std::min<LONG>(px(a, 420), r.right - px(a, 140))), px(a, 26), TRUE);
    ShowWindow(a.search, a.find_open ? SW_SHOW : SW_HIDE);
    update_scroll(a);
}
static void update_sidebar(App& a) {
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
static void choose_file(App& a) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = a.window;
    dialog.lpstrFilter = L"PDF documents (*.pdf)\0*.pdf\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog))
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
static INT_PTR CALLBACK licenses_dialog(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto instance = reinterpret_cast<HINSTANCE>(lp);
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
    return FALSE;
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
        throw std::runtime_error("Finish or cancel the current update before changing the installation.");
    const bool present = deployment::installation_present();
    std::wstring content = L"Install folder:\n" + deployment::install_directory().wstring();
    BOOL shortcut = present ? deployment::shortcut_present() : TRUE;
    std::vector<TASKDIALOG_BUTTON> buttons;
    if (a.installed)
        buttons = {{100, L"Apply"}, {101, L"Uninstall FastPDF"}};
    else if (present)
        buttons = {{102, L"Open installed FastPDF"}, {101, L"Uninstall FastPDF"}};
    else {
        buttons = {{100, L"Install FastPDF"}};
        content += L"\n\nInstall for your Windows account, or cancel to keep using the portable reader.";
    }
    const int result =
        task_dialog(a, a.installed ? L"FastPDF installation" : L"Install FastPDF", content, buttons,
                    (!present || a.installed) ? L"Create a Start menu shortcut" : nullptr,
                    (!present || a.installed) ? &shortcut : nullptr);
    if (result == 100) {
        deployment::install(shortcut != FALSE);
        if (!a.installed) {
            deployment::open_installed(current_session(a));
            PostMessageW(a.window, WM_CLOSE, 0, 0);
        } else
            MessageBoxW(a.window, L"Installation options saved.", L"FastPDF", MB_OK | MB_ICONINFORMATION);
    } else if (result == 102) {
        deployment::open_installed(current_session(a));
        PostMessageW(a.window, WM_CLOSE, 0, 0);
    } else if (result == 101) {
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
        a.update_status =
            L"Update will install when you close FastPDF. Help → Cancel update to keep this version.";
    } else {
        a.update_on_exit = false;
        a.update_status = L"Update ready. Help → Check for updates to install.";
    }
}
static void start_update_job(App& a, std::optional<deployment::Release> release = {}) {
    a.update_cancel = false;
    a.update_status = release ? L"Downloading update…  Help → Cancel update" : L"Checking for updates…";
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
static void cancel_update(App& a) {
    a.update_cancel = true;
    a.update_on_exit = false;
    deployment::cleanup_download(a.pending_update);
    a.pending_update = {};
    a.update_status = a.update_job.valid() ? L"Canceling update…" : L"Update canceled.";
}
static void command(App& a, int id) {
    if (id == cmd_install)
        installation_dialog(a);
    else if (id == cmd_portable)
        a.portable_choice = true;
    else if (id == cmd_cancel_update)
        cancel_update(a);
    else if (id == cmd_updates) {
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
        if (id == cmd_bookmarks) {
            a.bookmarks = !a.bookmarks;
            a.show_sidebar = true;
        } else
            a.show_sidebar = !a.show_sidebar;
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
    else if (id == cmd_export && !a.pages.empty()) {
        wchar_t name[32768] = L"page.png";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = a.window;
        dialog.lpstrFilter = L"PNG image\0*.png\0";
        dialog.lpstrFile = name;
        dialog.nMaxFile = 32768;
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) {
            Request request;
            request.kind = Job::export_png;
            request.page = a.page;
            request.rotation = a.rotation;
            request.scale = std::max(2.0, a.scale);
            request.path = name;
            enqueue(a, std::move(request));
            a.status = L"Exporting page…";
            a.busy = true;
        }
    } else if (id == cmd_print && !a.pages.empty()) {
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
    } else if (id == cmd_licenses) {
        DialogBoxParamW(a.instance, MAKEINTRESOURCEW(IDD_LICENSES), a.window, licenses_dialog,
                        reinterpret_cast<LPARAM>(a.instance));
    } else if (id == cmd_info) {
        std::wstring info =
            L"FastPDF " FASTPDF_VERSION_W L"\nNative C++ reader with an owned PDF engine.\n\n";
        if (!a.pages.empty())
            info += std::to_wstring(a.pages.size()) + L" pages\n" + a.path + L"\n\n";
        if (a.bitmap && !a.bitmap->warnings.empty()) {
            info += L"Compatibility notes:\n";
            for (auto& warning : a.bitmap->warnings)
                info += L"• " + utf8(warning) + L"\n";
        } else
            info += L"No compatibility warnings have been reported for this page.\n";
        info += L"\nCtrl+O  Open     Ctrl+F  Find     Ctrl+G  Go to page\nCtrl+C  Copy text     Ctrl+P  "
                L"Print     R  Rotate\nCtrl+wheel  Zoom     Middle drag  Pan\nF  Fit page     W  Fit width   "
                L"  B  Bookmarks";
        MessageBoxW(a.window, info.c_str(), L"FastPDF — document information", MB_OK | MB_ICONINFORMATION);
    }
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
    fill_rect(dc, r, RGB(33, 36, 43));
    RECT top{0, 0, r.right, px(a, 64)};
    fill_rect(dc, top, RGB(20, 23, 29));
    fill_rect(dc, {0, top.bottom - 1, r.right, top.bottom}, RGB(58, 62, 73));
    label(dc, {px(a, 18), 0, px(a, 102), top.bottom}, L"FastPDF", RGB(226, 230, 241), a.font);
    a.buttons.clear();
    int x = px(a, 112);
    auto button = [&](int id, std::wstring text, int width, bool active = false) {
        int w = px(a, width);
        if (x + w > r.right - px(a, 12))
            return;
        RECT rect{x, px(a, 15), x + w, px(a, 49)};
        if (active || id == cmd_open) {
            fill_rect(dc, rect, id == cmd_open ? RGB(93, 77, 204) : RGB(50, 55, 67));
        }
        label(dc, rect, text, RGB(235, 237, 245), a.font, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        a.buttons.push_back({rect, id, text});
        x += w + px(a, 5);
    };
    button(cmd_open, L"Open", 64);
    button(cmd_prev, L"‹", 30);
    button(cmd_next, L"›", 30);
    button(cmd_goto,
           a.pages.empty() ? L"— / —"
                           : std::to_wstring(a.page + 1) + L" / " + std::to_wstring(a.pages.size()),
           96);
    button(cmd_zoom_out, L"−", 28);
    button(cmd_zoom_in, L"+", 28);
    button(cmd_fit, L"Fit page", 78, a.fit == Fit::page);
    button(cmd_width, L"Fit width", 82, a.fit == Fit::width);
    button(cmd_rotate, L"Rotate", 66);
    button(cmd_search, L"Find", 54, a.find_open);
    button(cmd_sidebar, L"Pages", 62, a.show_sidebar && !a.bookmarks);
    button(cmd_bookmarks, L"Outline", 72, a.show_sidebar && a.bookmarks);
    if (a.find_open) {
        fill_rect(dc, {0, px(a, 64), r.right, px(a, 104)}, RGB(26, 30, 38));
        label(dc, {px(a, 454), px(a, 64), r.right - px(a, 12), px(a, 104)},
              L"Enter to find · F3 next page · Esc to close", RGB(165, 173, 192), a.small_font);
    }
    int saved = SaveDC(dc);
    IntersectClipRect(dc, vp.left, vp.top, vp.right, vp.bottom);
    if (a.pages.empty()) {
        int center = (vp.top + vp.bottom) / 2;
        label(dc, {vp.left + px(a, 40), center - px(a, 92), vp.right - px(a, 40), center - px(a, 42)},
              L"Your documents. At full speed.", RGB(234, 237, 245), a.title_font,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        label(dc, {vp.left + px(a, 40), center - px(a, 28), vp.right - px(a, 40), center + px(a, 12)},
              L"Drop a PDF here, or press Ctrl+O to open one.", RGB(159, 169, 190), a.font,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        label(dc, {vp.left + px(a, 40), center + px(a, 32), vp.right - px(a, 40), center + px(a, 64)},
              L"Native Windows · Local files · An independent PDF engine", RGB(111, 122, 144), a.small_font,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!a.installed && !a.portable_choice && a.path.empty()) {
            const int midpoint = (vp.left + vp.right) / 2;
            RECT install_rect{midpoint - px(a, 188), center + px(a, 88), midpoint - px(a, 8),
                              center + px(a, 126)};
            RECT portable_rect{midpoint + px(a, 8), center + px(a, 88), midpoint + px(a, 188),
                               center + px(a, 126)};
            fill_rect(dc, install_rect, RGB(93, 77, 204));
            fill_rect(dc, portable_rect, RGB(50, 55, 67));
            label(dc, install_rect, L"Install FastPDF", RGB(235, 237, 245), a.font,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            label(dc, portable_rect, L"Continue portable", RGB(235, 237, 245), a.font,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            a.buttons.push_back({install_rect, cmd_install, L"Install FastPDF"});
            a.buttons.push_back({portable_rect, cmd_portable, L"Continue portable"});
        }
    } else {
        RECT page = page_rect(a);
        RECT shadow = page;
        OffsetRect(&shadow, px(a, 3), px(a, 5));
        fill_rect(dc, shadow, RGB(17, 19, 24));
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
                    highlight(dc, span_rect(a, span.bounds), 0x50252550);
                else if (!needle.empty() && lower(span.text).find(needle) != std::wstring::npos)
                    highlight(dc, span_rect(a, span.bounds), 0x60605000);
            }
        } else
            label(dc, page, L"Rendering…", RGB(130, 134, 146), a.font,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    RestoreDC(dc, saved);
    RECT status{0, r.bottom - px(a, 30), r.right, r.bottom};
    fill_rect(dc, status, RGB(20, 23, 29));
    label(dc, {px(a, 14), status.top, r.right - px(a, 250), status.bottom},
          a.update_status.empty() ? a.status : a.update_status, RGB(158, 169, 191), a.small_font);
    if (!a.pages.empty()) {
        std::wstring details = std::to_wstring(int(a.scale * 72 / a.dpi * 100)) + L"%";
        if (a.bitmap)
            details += L"   ·   " + std::to_wstring(int(a.bitmap->milliseconds)) + L" ms";
        if (a.bitmap && !a.bitmap->warnings.empty())
            details += L"   ·   " + std::to_wstring(a.bitmap->warnings.size()) + L" notes";
        label(dc, {r.right - px(a, 240), status.top, r.right - px(a, 14), status.bottom}, details,
              RGB(165, 175, 195), a.small_font, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
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
    if (a.show_sidebar) {
        auto vp = viewport(a);
        fill_rect(memory, {0, vp.top, px(a, 220), vp.bottom}, RGB(26, 30, 38));
        for (size_t i = 0; i < std::min<size_t>(a.pages.size(), size_t((vp.bottom - vp.top) / px(a, 40)));
             ++i)
            label(memory,
                  {px(a, 18), vp.top + int(i) * px(a, 40), px(a, 202), vp.top + int(i + 1) * px(a, 40)},
                  L"Page " + std::to_wstring(i + 1), RGB(185, 196, 218), a.font);
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
            if (a.bitmap && !a.bitmap->warnings.empty())
                a.status += L"  —  See Document information for compatibility notes";
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
            a.dpi = GetDpiForWindow(window);
            a.sidebar = CreateWindowExW(0, L"LISTBOX", L"Pages",
                                        WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NODATA | LBS_OWNERDRAWFIXED |
                                            LBS_NOINTEGRALHEIGHT,
                                        0, 0, 0, 0, window, HMENU(INT_PTR(id_sidebar)), a.instance, nullptr);
            a.search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP,
                                       0, 0, 0, 0, window, HMENU(INT_PTR(id_search)), a.instance, nullptr);
            SendMessageW(a.search, EM_SETCUEBANNER, TRUE, LPARAM(L"Find in document"));
            SendMessageW(a.search, EM_LIMITTEXT, 4095, 0);
            SetWindowSubclass(a.search, search_proc, 1, DWORD_PTR(&a));
            a.edit_background = CreateSolidBrush(RGB(38, 43, 54));
            fonts(a);
            DragAcceptFiles(window, TRUE);
            a.worker.window = window;
            a.worker.thread = std::thread(worker_run, std::ref(a.worker));
            layout(a);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
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
            SetBkColor(HDC(wp), RGB(26, 30, 38));
            return LRESULT(a.edit_background);
        case WM_CTLCOLOREDIT:
            SetBkColor(HDC(wp), RGB(38, 43, 54));
            SetTextColor(HDC(wp), RGB(236, 240, 248));
            return LRESULT(a.edit_background);
        case WM_DRAWITEM: {
            auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (item->CtlID != id_sidebar || item->itemID == UINT(-1))
                break;
            bool selected = (item->itemState & ODS_SELECTED) != 0;
            fill_rect(item->hDC, item->rcItem, selected ? RGB(63, 56, 105) : RGB(26, 30, 38));
            std::wstring text;
            RECT rect = item->rcItem;
            rect.left += px(a, 18);
            rect.right -= px(a, 10);
            if (a.bookmarks && item->itemID < a.outline.size()) {
                auto& outline = a.outline[item->itemID];
                text = outline.title;
                rect.left += px(a, std::min(outline.depth, 5) * 12);
            } else
                text = L"Page " + std::to_wstring(item->itemID + 1);
            label(item->hDC, rect, text, selected ? RGB(243, 241, 255) : RGB(177, 187, 208), a.font);
            return TRUE;
        }
        case WM_INITMENUPOPUP:
            EnableMenuItem(
                GetMenu(window), cmd_cancel_update,
                MF_BYCOMMAND |
                    (a.update_job.valid() || !a.pending_update.directory.empty() ? MF_ENABLED : MF_GRAYED));
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
                SetFocus(window);
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
            for (auto& button : a.buttons)
                if (PtInRect(&button.rect, p)) {
                    command(a, button.command);
                    return 0;
                }
            auto vp = viewport(a);
            if (p.y >= client(a).bottom - px(a, 30)) {
                command(a, cmd_info);
                return 0;
            }
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
            AppendMenuW(menu, MF_STRING, cmd_export, L"Save page as PNG…");
            AppendMenuW(menu, MF_STRING, cmd_print, L"Print…\tCtrl+P");
            AppendMenuW(menu, MF_STRING, cmd_info, L"Document information");
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
            if (ctrl && wp == 'O')
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
            } else if (ctrl && wp == 'S')
                command(a, cmd_export);
            else if (wp == VK_ADD || wp == VK_OEM_PLUS)
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
                command(a, cmd_info);
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
            for (auto f : {a.font, a.title_font, a.small_font})
                if (f)
                    DeleteObject(f);
            if (a.edit_background)
                DeleteObject(a.edit_background);
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
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
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
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"FastPDF.Window";
    RegisterClassExW(&wc);
    HMENU menu = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, cmd_open, L"&Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, cmd_export, L"Save page as &PNG…\tCtrl+S");
    AppendMenuW(file, MF_STRING, cmd_print, L"&Print…\tCtrl+P");
    AppendMenuW(file, MF_STRING, cmd_info, L"Document &information");
    AppendMenuW(view, MF_STRING, cmd_search, L"&Find…\tCtrl+F");
    AppendMenuW(view, MF_STRING, cmd_goto, L"&Go to page…\tCtrl+G");
    AppendMenuW(view, MF_STRING, cmd_fit, L"Fit &page\tF");
    AppendMenuW(view, MF_STRING, cmd_width, L"Fit &width\tW");
    AppendMenuW(view, MF_STRING, cmd_rotate, L"&Rotate\tR");
    AppendMenuW(view, MF_STRING, cmd_sidebar, L"Page &list");
    AppendMenuW(view, MF_STRING, cmd_bookmarks, L"&Bookmarks\tB");
    AppendMenuW(help, MF_STRING, cmd_info, L"&About and shortcuts\tF1");
    AppendMenuW(help, MF_STRING, cmd_licenses, L"Third-party &notices…");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, cmd_install, L"&Installation…");
    AppendMenuW(help, MF_STRING, cmd_updates, L"Check for &updates…");
    AppendMenuW(help, MF_STRING, cmd_cancel_update, L"&Cancel update");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(file), L"&File");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(view), L"&View");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(help), L"&Help");
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
        app.exit_code = 1;
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
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
