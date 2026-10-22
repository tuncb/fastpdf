#include "dialog_style.h"
#include "resource.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <bit>
#include <memory>
#include <stdexcept>
#include <vector>
#include "ui_style.h"

namespace ui {
struct NativeDialogTheme {
    using AppMode = int(WINAPI*)(int);
    using WindowMode = bool(WINAPI*)(HWND, bool);
    AppMode app = nullptr;
    WindowMode window = nullptr;
};
static const NativeDialogTheme& native_dialog_theme() {
    // Shell views need the UxTheme opt-in in addition to the DWM title-bar colors.
    // These optional exports are not part of the public SDK. Ordinal 135 had a
    // different signature before Windows 10 1903, so never call it on older builds.
    static const NativeDialogTheme theme = [] {
        NativeDialogTheme result;
        using Version = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto version =
            std::bit_cast<Version>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW info{sizeof(info)};
        if (!version || version(&info) != 0 || info.dwMajorVersion < 10 || info.dwBuildNumber < 18362)
            return result;
        const auto module = GetModuleHandleW(L"uxtheme.dll");
        if (module) {
            result.app =
                std::bit_cast<NativeDialogTheme::AppMode>(GetProcAddress(module, MAKEINTRESOURCEA(135)));
            result.window =
                std::bit_cast<NativeDialogTheme::WindowMode>(GetProcAddress(module, MAKEINTRESOURCEA(133)));
        }
        return result;
    }();
    return theme;
}
void enable_system_dialog_theme() {
    if (const auto mode = native_dialog_theme().app)
        mode(system_dialog_colors().dark ? 2 : 3);
}
DialogColors system_dialog_colors() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON)) {
        return {false,
                GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_GRAYTEXT),
                GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_HOTLIGHT),
                GetSysColor(COLOR_HIGHLIGHTTEXT)};
    }
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (!light)
        return {true,   surface, chrome, canvas, hover, selected,
                border, text,    muted,  focus,  link,  RGB(255, 255, 255)};
    return {false,
            RGB(249, 249, 249),
            RGB(240, 240, 240),
            RGB(255, 255, 255),
            RGB(240, 244, 250),
            RGB(225, 232, 242),
            RGB(195, 195, 195),
            RGB(28, 28, 28),
            RGB(100, 100, 100),
            RGB(40, 94, 218),
            RGB(25, 80, 185),
            RGB(255, 255, 255)};
}
struct DialogStyle {
    HWND window = nullptr;
    HWND tooltips = nullptr;
    std::vector<std::pair<HWND, std::wstring>> button_captions;
    std::vector<std::pair<HWND, ToolbarIcon>> button_icons;
    DialogColors colors{};
    HBRUSH body = nullptr, footer = nullptr, field = nullptr;
    Painter painter;
    HFONT font = nullptr;
    int footer_top = -1, primary = IDOK;
    ~DialogStyle() {
        if (tooltips)
            DestroyWindow(tooltips);
        DeleteObject(body);
        DeleteObject(footer);
        DeleteObject(field);
        close(painter);
        DeleteObject(font);
    }
};
static constexpr wchar_t style_property[] = L"FastPDF.DialogStyle";
static const UINT refresh_message = RegisterWindowMessageW(L"FastPDF.RefreshDialogStyle");
static int pixels(HWND window, int value) {
    return MulDiv(value, int(GetDpiForWindow(window)), 96);
}
void position_dialog_control(HWND window, int id, RECT units) {
    MapDialogRect(window, &units);
    MoveWindow(GetDlgItem(window, id), units.left, units.top, units.right - units.left,
               units.bottom - units.top, TRUE);
}
static int footer_y(HWND window, const DialogStyle& state) {
    RECT units{0, 0, 0, state.footer_top};
    MapDialogRect(window, &units);
    return state.footer_top < 0 ? MAXLONG : units.bottom;
}
static bool in_footer(HWND window, HWND child, const DialogStyle& state) {
    RECT bounds{};
    GetWindowRect(child, &bounds);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds.top >= footer_y(window, state);
}
static bool window_class(HWND window, const wchar_t* name) {
    wchar_t value[64]{};
    GetClassNameW(window, value, int(std::size(value)));
    return _wcsicmp(value, name) == 0;
}
struct CenteredEdit {
    int top = 0, bottom = 0;
};
static void update_edit_frame(HWND window) {
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}
static LRESULT CALLBACK centered_edit_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                           DWORD_PTR data) {
    auto& state = *reinterpret_cast<CenteredEdit*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, centered_edit_proc, id);
        delete &state;
        return DefSubclassProc(window, message, wp, lp);
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_NCCALCSIZE) {
        auto& bounds = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0] : *reinterpret_cast<RECT*>(lp);
        const HDC dc = GetDC(window);
        const auto font = HFONT(SendMessageW(window, WM_GETFONT, 0, 0));
        const auto old = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        const int extra = std::max(0L, bounds.bottom - bounds.top - metrics.tmHeight);
        state.top = extra / 2;
        state.bottom = extra - state.top;
        bounds.top += state.top;
        bounds.bottom -= state.bottom;
        return wp ? WVR_REDRAW : 0;
    }
    if (message == WM_NCPAINT && (state.top || state.bottom)) {
        const HDC dc = GetWindowDC(window);
        RECT outer{}, client{};
        GetWindowRect(window, &outer);
        GetClientRect(window, &client);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        OffsetRect(&client, -outer.left, -outer.top);
        const bool read_only =
            !IsWindowEnabled(window) || (GetWindowLongPtrW(window, GWL_STYLE) & ES_READONLY);
        auto brush = HBRUSH(SendMessageW(GetParent(window), read_only ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT,
                                         WPARAM(dc), LPARAM(window)));
        if (!brush)
            brush = GetSysColorBrush(COLOR_WINDOW);
        const RECT top{client.left, client.top - state.top, client.right, client.top};
        const RECT bottom{client.left, client.bottom, client.right, client.bottom + state.bottom};
        FillRect(dc, &top, brush);
        FillRect(dc, &bottom, brush);
        ReleaseDC(window, dc);
    }
    if (message == WM_NCHITTEST) {
        POINT point{short(LOWORD(lp)), short(HIWORD(lp))};
        ScreenToClient(window, &point);
        RECT bounds{};
        GetClientRect(window, &bounds);
        bounds.top -= state.top;
        bounds.bottom += state.bottom;
        // The padding is still part of the editable field for mouse input.
        if (PtInRect(&bounds, point))
            return HTCLIENT;
    }
    if (message == WM_SETFONT || message == WM_THEMECHANGED || message == WM_DPICHANGED_AFTERPARENT)
        update_edit_frame(window);
    return result;
}
void center_edit_text(HWND window) {
    if (!window_class(window, L"Edit") || (GetWindowLongPtrW(window, GWL_STYLE) & ES_MULTILINE))
        return;
    DWORD_PTR existing = 0;
    if (!GetWindowSubclass(window, centered_edit_proc, 1, &existing)) {
        auto state = std::make_unique<CenteredEdit>();
        if (SetWindowSubclass(window, centered_edit_proc, 1, DWORD_PTR(state.get())))
            state.release();
    }
    update_edit_frame(window);
}
static LRESULT CALLBACK field_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                   DWORD_PTR data) {
    const auto& state = *reinterpret_cast<DialogStyle*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, field_proc, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (message == WM_NCPAINT || message == WM_PAINT || message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        HDC dc = GetWindowDC(window);
        RECT bounds{};
        GetWindowRect(window, &bounds);
        OffsetRect(&bounds, -bounds.left, -bounds.top);
        const auto brush = CreateSolidBrush(GetFocus() == window ? state.colors.accent : state.colors.border);
        FrameRect(dc, &bounds, brush);
        DeleteObject(brush);
        ReleaseDC(window, dc);
    }
    return result;
}
static LRESULT CALLBACK native_control_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                            DWORD_PTR data) {
    auto& state = *reinterpret_cast<DialogStyle*>(data);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, native_control_proc, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORLISTBOX) {
        SetTextColor(HDC(wp), state.colors.text);
        SetBkColor(HDC(wp), state.colors.field);
        return LRESULT(state.field);
    }
    if (window_class(window, WC_LISTVIEWW)) {
        if (message == LVM_SETBKCOLOR || message == LVM_SETTEXTBKCOLOR)
            lp = LPARAM(state.colors.field);
        if (message == LVM_SETTEXTCOLOR)
            lp = LPARAM(state.colors.text);
    }
    if (message == WM_NOTIFY) {
        const auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (draw->hdr.code == NM_CUSTOMDRAW && window_class(draw->hdr.hwndFrom, WC_LISTVIEWW)) {
            const auto list = draw->hdr.hwndFrom;
            const auto list_draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lp);
            const bool report = ListView_GetView(list) == LV_VIEW_DETAILS;
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                FillRect(draw->hdc, &draw->rc, state.field);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT && report)
                return CDRF_NOTIFYSUBITEMDRAW;
            if (draw->dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) ||
                (draw->dwDrawStage == CDDS_ITEMPREPAINT && !report)) {
                const int row = int(draw->dwItemSpec), column = report ? list_draw->iSubItem : 0;
                const bool item_selected =
                    (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                RECT bounds{}, label{}, icon_bounds{};
                if (report) {
                    ListView_GetSubItemRect(list, row, column, LVIR_BOUNDS, &bounds);
                    RECT column_bounds{};
                    Header_GetItemRect(ListView_GetHeader(list), column, &column_bounds);
                    bounds.left = column_bounds.left;
                    bounds.right = column_bounds.right;
                    label = bounds;
                    InflateRect(&label, -pixels(list, 6), 0);
                } else {
                    ListView_GetItemRect(list, row, &bounds, LVIR_BOUNDS);
                    ListView_GetItemRect(list, row, &label, LVIR_LABEL);
                }
                const int saved = SaveDC(draw->hdc);
                SetDCBrushColor(draw->hdc, item_selected ? state.colors.accent : state.colors.field);
                FillRect(draw->hdc, &bounds, HBRUSH(GetStockObject(DC_BRUSH)));
                if (column == 0) {
                    LVITEMW item{};
                    item.mask = LVIF_IMAGE;
                    item.iItem = row;
                    ListView_GetItem(list, &item);
                    ListView_GetItemRect(list, row, &icon_bounds, LVIR_ICON);
                    const auto icons = ListView_GetImageList(
                        list, (GetWindowLongPtrW(list, GWL_STYLE) & LVS_TYPEMASK) == LVS_ICON ? LVSIL_NORMAL
                                                                                              : LVSIL_SMALL);
                    if (icons) {
                        int icon_width = 0, icon_height = 0;
                        ImageList_GetIconSize(icons, &icon_width, &icon_height);
                        ImageList_Draw(icons, item.iImage, draw->hdc, icon_bounds.left,
                                       (icon_bounds.top + icon_bounds.bottom - icon_height) / 2,
                                       ILD_TRANSPARENT);
                    }
                    if (report)
                        label.left = icon_bounds.right + pixels(list, 4);
                }
                wchar_t value[1024]{};
                ListView_GetItemText(list, row, column, value, int(std::size(value)));
                SelectObject(draw->hdc, HFONT(SendMessageW(list, WM_GETFONT, 0, 0)));
                SetBkMode(draw->hdc, TRANSPARENT);
                SetTextColor(draw->hdc, item_selected ? state.colors.on_accent : state.colors.text);
                DrawTextW(draw->hdc, value, -1, &label,
                          DT_NOPREFIX | DT_END_ELLIPSIS |
                              (report ? DT_SINGLELINE | DT_VCENTER : DT_WORDBREAK | DT_CENTER));
                RestoreDC(draw->hdc, saved);
                return CDRF_SKIPDEFAULT;
            }
        }
        if (draw->hdr.code == NM_CUSTOMDRAW && window_class(draw->hdr.hwndFrom, WC_HEADERW)) {
            if (draw->dwDrawStage == CDDS_PREPAINT)
                return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                const int saved = SaveDC(draw->hdc);
                FillRect(draw->hdc, &draw->rc, state.footer);
                wchar_t title[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT;
                item.pszText = title;
                item.cchTextMax = int(std::size(title));
                Header_GetItem(draw->hdr.hwndFrom, int(draw->dwItemSpec), &item);
                RECT label = draw->rc;
                InflateRect(&label, -pixels(window, 6), 0);
                SetBkMode(draw->hdc, TRANSPARENT);
                SetTextColor(draw->hdc, state.colors.text);
                DrawTextW(draw->hdc, title, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
                RestoreDC(draw->hdc, saved);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    if (message == WM_PAINT && window_class(window, L"Button")) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        const int saved = SaveDC(dc);
        wchar_t title[128]{};
        GetWindowTextW(window, title, int(std::size(title)));
        SelectObject(dc, HFONT(SendMessageW(window, WM_GETFONT, 0, 0)));
        RECT label{};
        DrawTextW(dc, title, -1, &label, DT_CALCRECT | DT_SINGLELINE);
        RECT bounds{};
        GetClientRect(window, &bounds);
        bounds.top += label.bottom / 2;
        SetDCBrushColor(dc, state.colors.border);
        FrameRect(dc, &bounds, HBRUSH(GetStockObject(DC_BRUSH)));
        OffsetRect(&label, pixels(window, 8), 0);
        InflateRect(&label, pixels(window, 2), 0);
        FillRect(dc, &label, state.body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state.colors.text);
        DrawTextW(dc, title, -1, &label, DT_SINGLELINE);
        RestoreDC(dc, saved);
        EndPaint(window, &paint);
        return 0;
    }
    return DefSubclassProc(window, message, wp, lp);
}
static BOOL CALLBACK theme_child(HWND child, LPARAM context) {
    auto& state = *reinterpret_cast<DialogStyle*>(context);
    const auto font = state.font ? LRESULT(state.font) : SendMessageW(state.window, WM_GETFONT, 0, 0);
    if (font && SendMessageW(child, WM_GETFONT, 0, 0) != font)
        SendMessageW(child, WM_SETFONT, WPARAM(font), FALSE);
    if (const auto mode = native_dialog_theme().window)
        mode(child, state.colors.dark);
    if (GetPropW(child, style_property))
        SendMessageW(child, WM_SETTINGCHANGE, 0, 0);
    const bool edit = window_class(child, L"Edit");
    if (edit || window_class(child, L"ComboBox") || window_class(child, L"ScrollBar"))
        SetWindowTheme(child, state.colors.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (window_class(child, L"ComboBox") || window_class(child, WC_COMBOBOXEXW)) {
        SetWindowTheme(child, state.colors.dark ? L"DarkMode_CFD" : L"Explorer", nullptr);
        SetWindowSubclass(child, native_control_proc, 2, DWORD_PTR(&state));
    }
    if (window_class(child, L"Button") && (GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX)
        SetWindowSubclass(child, native_control_proc, 2, DWORD_PTR(&state));
    if (edit) {
        SetWindowSubclass(child, field_proc, 1, DWORD_PTR(&state));
        SetWindowLongPtrW(child, GWL_EXSTYLE, GetWindowLongPtrW(child, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        SetWindowLongPtrW(child, GWL_STYLE, GetWindowLongPtrW(child, GWL_STYLE) | WS_BORDER);
        center_edit_text(child);
        SetWindowPos(child, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (window_class(child, WC_LISTVIEWW)) {
        SetWindowSubclass(child, native_control_proc, 2, DWORD_PTR(&state));
        SetWindowTheme(child, state.colors.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        ListView_SetBkColor(child, state.colors.field);
        ListView_SetTextBkColor(child, state.colors.field);
        ListView_SetTextColor(child, state.colors.text);
        if (const auto tips = ListView_GetToolTips(child)) {
            SetWindowTheme(tips, L"", L"");
            SendMessageW(tips, TTM_SETTIPBKCOLOR, state.colors.body, 0);
            SendMessageW(tips, TTM_SETTIPTEXTCOLOR, state.colors.text, 0);
        }
    }
    if (window_class(child, TOOLBARCLASSNAMEW))
        SetWindowTheme(child, L"", L"");
    if (window_class(child, L"SHELLDLL_DefView"))
        SetWindowSubclass(child, native_control_proc, 2, DWORD_PTR(&state));
    if (window_class(child, WC_TREEVIEWW)) {
        TreeView_SetBkColor(child, state.colors.field);
        TreeView_SetTextColor(child, state.colors.text);
    }
    return TRUE;
}
static void refresh_style(HWND window, DialogStyle& state) {
    state.colors = system_dialog_colors();
    enable_system_dialog_theme();
    if (const auto mode = native_dialog_theme().window)
        mode(window, state.colors.dark);
    DeleteObject(state.body);
    DeleteObject(state.footer);
    DeleteObject(state.field);
    state.body = CreateSolidBrush(state.colors.body);
    state.footer = CreateSolidBrush(state.colors.footer);
    state.field = CreateSolidBrush(state.colors.field);
    if (state.tooltips) {
        SetWindowTheme(state.tooltips, L"", L"");
        SendMessageW(state.tooltips, TTM_SETTIPBKCOLOR, state.colors.body, 0);
        SendMessageW(state.tooltips, TTM_SETTIPTEXTCOLOR, state.colors.text, 0);
    }
    BOOL dark = state.colors.dark;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &state.colors.footer, sizeof(COLORREF));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &state.colors.text, sizeof(COLORREF));
    EnumChildWindows(window, theme_child, LPARAM(&state));
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
static void draw_button(HWND window, DialogStyle& state, const NMCUSTOMDRAW& draw) {
    const auto button = draw.hdr.hwndFrom;
    const auto kind = GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK;
    const bool radio = kind == BS_RADIOBUTTON || kind == BS_AUTORADIOBUTTON;
    const bool check =
        radio || kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX || kind == BS_3STATE || kind == BS_AUTO3STATE;
    const bool toggle = check && (GetWindowLongPtrW(button, GWL_STYLE) & BS_PUSHLIKE);
    const bool checked = check && SendMessageW(button, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
    const bool enabled = IsWindowEnabled(button) != FALSE;
    const bool pressed = (draw.uItemState & CDIS_SELECTED) != 0;
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0;
    const bool primary = !check && GetDlgCtrlID(button) == state.primary;
    std::optional<ToolbarIcon> icon;
    for (const auto& [control, value] : state.button_icons)
        if (control == button)
            icon = value;
    const auto& c = state.colors;
    const bool footer = in_footer(window, button, state);
    const auto background = footer ? c.footer : c.body;
    const int saved = SaveDC(draw.hdc);
    FillRect(draw.hdc, &draw.rc, footer ? state.footer : state.body);
    RECT label = draw.rc;
    if (check && !toggle) {
        const int size = pixels(window, 16);
        RECT box{draw.rc.left, (draw.rc.top + draw.rc.bottom - size) / 2, draw.rc.left + size,
                 (draw.rc.top + draw.rc.bottom + size) / 2};
        SelectObject(draw.hdc, GetStockObject(DC_PEN));
        SelectObject(draw.hdc, GetStockObject(DC_BRUSH));
        SetDCPenColor(draw.hdc, !enabled ? c.muted : checked || focused ? c.accent : c.muted);
        SetDCBrushColor(draw.hdc, checked ? (enabled ? c.accent : c.muted) : pressed ? c.pressed : c.body);
        if (radio)
            Ellipse(draw.hdc, box.left, box.top, box.right, box.bottom);
        else
            RoundRect(draw.hdc, box.left, box.top, box.right, box.bottom, pixels(window, 4),
                      pixels(window, 4));
        if (checked) {
            if (radio) {
                InflateRect(&box, -pixels(window, 5), -pixels(window, 5));
                SetDCBrushColor(draw.hdc, c.on_accent);
                SetDCPenColor(draw.hdc, c.on_accent);
                Ellipse(draw.hdc, box.left, box.top, box.right, box.bottom);
            } else {
                const auto pen = CreatePen(PS_SOLID, pixels(window, 2), c.on_accent);
                const auto old = SelectObject(draw.hdc, pen);
                const POINT points[] = {{box.left + pixels(window, 3), box.top + pixels(window, 8)},
                                        {box.left + pixels(window, 7), box.top + pixels(window, 12)},
                                        {box.left + pixels(window, 13), box.top + pixels(window, 4)}};
                Polyline(draw.hdc, points, int(std::size(points)));
                SelectObject(draw.hdc, old);
                DeleteObject(pen);
            }
        }
        label.left += pixels(window, 25);
    } else {
        const auto fill = (primary || (toggle && checked)) && enabled ? c.accent
                          : pressed                                   ? c.pressed
                          : (draw.uItemState & CDIS_HOT) && enabled   ? c.hover
                                                                      : c.body;
        control(state.painter, draw.hdc, draw.rc, GetDpiForWindow(window), background, fill,
                (primary || focused || checked) && enabled ? c.accent : c.border, !icon || focused ? 1 : 0,
                icon, enabled ? c.text : c.muted);
    }
    wchar_t caption[256]{};
    GetWindowTextW(button, caption, int(std::size(caption)));
    for (const auto& [control, caption_text] : state.button_captions)
        if (control == button) {
            lstrcpynW(caption, caption_text.c_str(), int(std::size(caption)));
            break;
        }
    SelectObject(draw.hdc, HFONT(SendMessageW(button, WM_GETFONT, 0, 0)));
    SetBkMode(draw.hdc, TRANSPARENT);
    SetTextColor(draw.hdc, !enabled ? c.muted : primary || (toggle && checked) ? c.on_accent : c.text);
    const auto cues = SendMessageW(button, WM_QUERYUISTATE, 0, 0);
    DrawTextW(draw.hdc, caption, -1, &label,
              DT_SINGLELINE | DT_VCENTER | (check && !toggle ? DT_LEFT : DT_CENTER) |
                  ((cues & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0));
    if (focused && !(cues & UISF_HIDEFOCUS)) {
        InflateRect(&label, -pixels(window, 3), -pixels(window, 3));
        DrawFocusRect(draw.hdc, &label);
    }
    RestoreDC(draw.hdc, saved);
}
static LRESULT CALLBACK dialog_proc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id,
                                    DWORD_PTR data) {
    auto& state = *reinterpret_cast<DialogStyle*>(data);
    if (message == refresh_message) {
        refresh_style(window, state);
        return 0;
    }
    switch (message) {
    case WM_NCDESTROY: {
        const auto result = DefSubclassProc(window, message, wp, lp);
        RemoveWindowSubclass(window, dialog_proc, id);
        RemovePropW(window, style_property);
        delete &state;
        return result;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        refresh_style(window, state);
        break;
    case WM_DPICHANGED:
    case WM_DPICHANGED_AFTERPARENT: {
        const auto result = DefSubclassProc(window, message, wp, lp);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        return result;
    }
    case WM_CTLCOLORDLG:
        return LRESULT(state.body);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        const auto child = HWND(lp);
        const bool field =
            message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || window_class(child, L"Edit");
        const bool footer = !field && in_footer(window, child, state);
        SetBkColor(HDC(wp), field ? state.colors.field : footer ? state.colors.footer : state.colors.body);
        SetTextColor(HDC(wp), !IsWindowEnabled(child)        ? state.colors.muted
                              : window_class(child, WC_LINK) ? state.colors.link
                                                             : state.colors.text);
        return LRESULT(field ? state.field : footer ? state.footer : state.body);
    }
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(HDC(wp), &bounds, state.body);
        bounds.top = footer_y(window, state);
        if (bounds.top < bounds.bottom) {
            FillRect(HDC(wp), &bounds, state.footer);
            bounds.bottom = bounds.top + pixels(window, 1);
            SetDCBrushColor(HDC(wp), state.colors.border);
            FillRect(HDC(wp), &bounds, HBRUSH(GetStockObject(DC_BRUSH)));
        }
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (draw->CtlType == ODT_COMBOBOX &&
            (GetWindowLongPtrW(draw->hwndItem, GWL_STYLE) & CBS_HASSTRINGS)) {
            const bool highlighted = (draw->itemState & ODS_SELECTED) != 0;
            const int saved = SaveDC(draw->hDC);
            SetDCBrushColor(draw->hDC, highlighted ? state.colors.accent : state.colors.field);
            FillRect(draw->hDC, &draw->rcItem, HBRUSH(GetStockObject(DC_BRUSH)));
            std::wstring value;
            const auto length = SendMessageW(draw->hwndItem, CB_GETLBTEXTLEN, draw->itemID, 0);
            if (length >= 0 && length < 32768) {
                value.resize(size_t(length) + 1);
                SendMessageW(draw->hwndItem, CB_GETLBTEXT, draw->itemID, LPARAM(value.data()));
            }
            RECT label = draw->rcItem;
            InflateRect(&label, -pixels(window, 4), 0);
            SelectObject(draw->hDC, HFONT(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0)));
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, highlighted ? state.colors.on_accent : state.colors.text);
            DrawTextW(draw->hDC, value.c_str(), -1, &label,
                      DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            RestoreDC(draw->hDC, saved);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto draw = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
        if (draw->hdr.hwndFrom == state.tooltips && draw->hdr.code == TTN_GETDISPINFOW) {
            auto tip = reinterpret_cast<NMTTDISPINFOW*>(lp);
            GetWindowTextW(HWND(tip->hdr.idFrom), tip->szText, int(std::size(tip->szText)));
            tip->lpszText = tip->szText;
            return 0;
        }
        if (draw->hdr.code == NM_CUSTOMDRAW && window_class(draw->hdr.hwndFrom, TOOLBARCLASSNAMEW)) {
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                FillRect(draw->hdc, &draw->rc, state.body);
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                const auto toolbar = reinterpret_cast<NMTBCUSTOMDRAW*>(lp);
                toolbar->clrText = state.colors.text;
                toolbar->clrBtnFace = state.colors.body;
                toolbar->clrBtnHighlight = state.colors.hover;
                toolbar->clrHighlightHotTrack = state.colors.hover;
                toolbar->nStringBkMode = TRANSPARENT;
                toolbar->nHLStringBkMode = TRANSPARENT;
                return TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK;
            }
        }
        if (draw->hdr.code == NM_CUSTOMDRAW && draw->dwDrawStage == CDDS_PREPAINT &&
            window_class(draw->hdr.hwndFrom, L"Button") &&
            (GetWindowLongPtrW(draw->hdr.hwndFrom, GWL_STYLE) & BS_TYPEMASK) != BS_GROUPBOX) {
            draw_button(window, state, *draw);
            return CDRF_SKIPDEFAULT;
        }
        break;
    }
    }
    return DefSubclassProc(window, message, wp, lp);
}
void dialog_footer(HWND window, int footer_top) {
    if (const auto state = reinterpret_cast<DialogStyle*>(GetPropW(window, style_property))) {
        state->footer_top = footer_top;
        InvalidateRect(window, nullptr, TRUE);
    }
}
void compact_button(HWND button, const wchar_t* caption) {
    const auto parent = GetParent(button);
    const auto state = reinterpret_cast<DialogStyle*>(GetPropW(parent, style_property));
    if (!state)
        return;
    state->button_captions.emplace_back(button, caption);
    if (!state->tooltips) {
        state->tooltips = CreateWindowExW(
            WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(state->tooltips, TTM_SETDELAYTIME, TTDT_INITIAL, 400);
        SendMessageW(state->tooltips, TTM_SETDELAYTIME, TTDT_AUTOPOP, 10000);
        SetWindowTheme(state->tooltips, L"", L"");
        SendMessageW(state->tooltips, TTM_SETTIPBKCOLOR, state->colors.body, 0);
        SendMessageW(state->tooltips, TTM_SETTIPTEXTCOLOR, state->colors.text, 0);
    }
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = parent;
    tool.uId = UINT_PTR(button);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(state->tooltips, TTM_ADDTOOLW, 0, LPARAM(&tool));
    InvalidateRect(button, nullptr, FALSE);
}
void icon_button(HWND button, ToolbarIcon icon) {
    const auto state = reinterpret_cast<DialogStyle*>(GetPropW(GetParent(button), style_property));
    if (!state)
        return;
    state->button_icons.emplace_back(button, icon);
    compact_button(button, L"");
}
void style_dialog(HWND window, int footer_top, int primary, bool center) {
    if (GetPropW(window, style_property))
        return;
    auto state = std::make_unique<DialogStyle>();
    state->window = window;
    state->footer_top = footer_top;
    state->primary = primary;
    if (!SetPropW(window, style_property, state.get()))
        return;
    if (!SetWindowSubclass(window, dialog_proc, 1, DWORD_PTR(state.get()))) {
        RemovePropW(window, style_property);
        return;
    }
    refresh_style(window, *state.release());
    // Common dialogs can create their shell controls after the initialization hook.
    PostMessageW(window, refresh_message, 0, 0);
    if (center && !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) {
        RECT bounds{}, parent{};
        GetWindowRect(window, &bounds);
        const auto owner = GetWindow(window, GW_OWNER);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
            return;
        if (!owner || !GetWindowRect(owner, &parent))
            parent = monitor.rcWork;
        const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
        const auto& work = monitor.rcWork;
        const int x = std::clamp((parent.left + parent.right - width) / 2, work.left,
                                 std::max(work.left, work.right - width));
        const int y = std::clamp((parent.top + parent.bottom - height) / 2, work.top,
                                 std::max(work.top, work.bottom - height));
        SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}
void refresh_dialog_style(HWND window) {
    PostMessageW(window, refresh_message, 0, 0);
}
void style_common_dialog(HWND window) {
    const auto font = CreateFontW(-MulDiv(10, int(GetDpiForWindow(window)), 72), 0, 0, 0, FW_NORMAL, FALSE,
                                  FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    RECT old_units{0, 0, 4, 8}, client{};
    MapDialogRect(window, &old_units);
    GetClientRect(window, &client);
    HDC dc = GetDC(window);
    const auto old_font = SelectObject(dc, font);
    SIZE alphabet{};
    GetTextExtentPoint32W(dc, L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", 52, &alphabet);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old_font);
    ReleaseDC(window, dc);
    const int unit_x = (alphabet.cx / 26 + 1) / 2, unit_y = metrics.tmHeight;
    SendMessageW(window, WM_SETFONT, WPARAM(font), FALSE);
    int content_bottom = 0;
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
        const int x = MulDiv(bounds.left, unit_x, old_units.right);
        const int y = MulDiv(bounds.top, unit_y, old_units.bottom);
        const int width = MulDiv(bounds.right - bounds.left, unit_x, old_units.right);
        const int height = MulDiv(bounds.bottom - bounds.top, unit_y, old_units.bottom);
        SendMessageW(child, WM_SETFONT, WPARAM(font), FALSE);
        MoveWindow(child, x, y, width, height, FALSE);
        if (GetDlgCtrlID(child) != IDOK && GetDlgCtrlID(child) != IDCANCEL &&
            (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE))
            content_bottom = std::max(content_bottom, y + height);
    }
    const int width = MulDiv(client.right, unit_x, old_units.right);
    const int footer = content_bottom + MulDiv(9, unit_y, 8);
    const int margin = MulDiv(12, unit_x, 4), gap = MulDiv(6, unit_x, 4);
    const int cancel_width = MulDiv(44, unit_x, 4), primary_width = MulDiv(50, unit_x, 4);
    const int button_y = footer + MulDiv(6, unit_y, 8), button_height = MulDiv(14, unit_y, 8);
    MoveWindow(GetDlgItem(window, IDCANCEL), width - margin - cancel_width, button_y, cancel_width,
               button_height, FALSE);
    MoveWindow(GetDlgItem(window, IDOK), width - margin - cancel_width - gap - primary_width, button_y,
               primary_width, button_height, FALSE);
    RECT size{0, 0, width, footer + MulDiv(26, unit_y, 8)};
    AdjustWindowRectExForDpi(&size, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                             DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
    SetWindowPos(window, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT units{0, 0, 4, 8};
    MapDialogRect(window, &units);
    style_dialog(window, MulDiv(footer, 8, units.bottom));
    if (const auto state = reinterpret_cast<DialogStyle*>(GetPropW(window, style_property)))
        state->font = font;
    else
        DeleteObject(font);
}
struct Prompt {
    const wchar_t* title;
    const std::wstring& content;
    std::span<const TASKDIALOG_BUTTON> buttons;
    int default_button;
    const wchar_t* checkbox;
    BOOL* checked;
};
static INT_PTR CALLBACK prompt_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto prompt = reinterpret_cast<Prompt*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        prompt = reinterpret_cast<Prompt*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        SetWindowTextW(window, prompt->title);
        SetDlgItemTextW(window, IDC_PROMPT_TEXT, prompt->content.c_str());
        const auto font = HFONT(SendMessageW(window, WM_GETFONT, 0, 0));
        HDC dc = GetDC(window);
        const auto old = SelectObject(dc, font);
        RECT unit{0, 0, 4, 8};
        MapDialogRect(window, &unit);
        std::vector<int> widths;
        int total = -6;
        for (const auto& button : prompt->buttons) {
            RECT text_bounds{};
            DrawTextW(dc, button.pszButtonText, -1, &text_bounds, DT_CALCRECT | DT_SINGLELINE);
            const int width = std::max(44, MulDiv(text_bounds.right, 4, unit.right) + 16);
            widths.push_back(width);
            total += width + 6;
        }
        const int width = std::max(220, total + 24);
        RECT content{0, 0, MulDiv(width - 24, unit.right, 4), 0};
        DrawTextW(dc, prompt->content.c_str(), -1, &content, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(window, dc);
        const int height = std::max(12, MulDiv(content.bottom, 8, unit.bottom) + 2);
        const int footer = 9 + height + (prompt->checkbox ? 24 : 8);
        position_dialog_control(window, IDC_PROMPT_TEXT, {12, 9, width - 12, 9 + height});
        if (prompt->checkbox) {
            SetDlgItemTextW(window, IDC_PROMPT_CHECK, prompt->checkbox);
            position_dialog_control(window, IDC_PROMPT_CHECK, {12, 15 + height, width - 12, 27 + height});
            CheckDlgButton(window, IDC_PROMPT_CHECK,
                           prompt->checked && *prompt->checked ? BST_CHECKED : BST_UNCHECKED);
        } else
            ShowWindow(GetDlgItem(window, IDC_PROMPT_CHECK), SW_HIDE);
        int left = width - 12 - total;
        for (size_t i = 0; i < prompt->buttons.size(); ++i) {
            const auto& button = prompt->buttons[i];
            const auto control = CreateWindowExW(
                0, L"Button", button.pszButtonText,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                    (button.nButtonID == prompt->default_button ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                0, 0, 0, 0, window, HMENU(INT_PTR(button.nButtonID)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(control, WM_SETFONT, WPARAM(font), TRUE);
            position_dialog_control(window, button.nButtonID,
                                    {left, footer + 6, left + widths[i], footer + 20});
            left += widths[i] + 6;
        }
        RECT client{0, 0, width, footer + 26};
        MapDialogRect(window, &client);
        AdjustWindowRectExForDpi(&client, DWORD(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
                                 DWORD(GetWindowLongPtrW(window, GWL_EXSTYLE)), GetDpiForWindow(window));
        SetWindowPos(window, nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        style_dialog(window, footer, prompt->buttons.front().nButtonID);
        SendMessageW(window, DM_SETDEFID, WPARAM(prompt->default_button), 0);
        SetFocus(GetDlgItem(window, prompt->default_button));
        return FALSE;
    }
    if (!prompt)
        return FALSE;
    if (message == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == IDCANCEL || std::any_of(prompt->buttons.begin(), prompt->buttons.end(),
                                          [id](const auto& button) { return id == button.nButtonID; })) {
            if (prompt->checked)
                *prompt->checked = IsDlgButtonChecked(window, IDC_PROMPT_CHECK) == BST_CHECKED;
            EndDialog(window, id);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(window, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
int show_prompt(HWND owner, const wchar_t* title, const std::wstring& content,
                std::span<const TASKDIALOG_BUTTON> buttons, int default_button, const wchar_t* checkbox,
                BOOL* checked) {
    Prompt prompt{title, content, buttons, default_button, checkbox, checked};
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROMPT), owner,
                                        prompt_proc, LPARAM(&prompt));
    if (result == -1)
        throw std::runtime_error("Cannot open the FastPDF dialog");
    return int(result);
}
int message_box(HWND owner, const wchar_t* content, const wchar_t* title, UINT flags) {
    const bool question = (flags & MB_TYPEMASK) == MB_YESNO;
    const TASKDIALOG_BUTTON yes_no[] = {{IDYES, L"Yes"}, {IDNO, L"No"}};
    const TASKDIALOG_BUTTON ok[] = {{IDOK, L"OK"}};
    const int result =
        show_prompt(owner, title, content, question ? std::span<const TASKDIALOG_BUTTON>(yes_no) : ok,
                    question ? ((flags & MB_DEFMASK) == MB_DEFBUTTON2 ? IDNO : IDYES) : IDOK);
    return result == IDCANCEL ? (question ? IDNO : IDOK) : result;
}
} // namespace ui
