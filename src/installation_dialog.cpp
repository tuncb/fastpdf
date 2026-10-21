#include "installation_dialog.h"
#include "resource.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <stdexcept>
#include <vector>
#include "ui_style.h"

namespace ui {
struct InstallationState {
    InstallationOptions& options;
    UINT dpi = 96;
    int footer_top = 69;
    HBRUSH background = CreateSolidBrush(surface), footer = CreateSolidBrush(chrome);
    Painter painter;
    ~InstallationState() {
        DeleteObject(background);
        DeleteObject(footer);
        close(painter);
    }
};
static int scaled(const InstallationState& state, int value) {
    return MulDiv(value, int(state.dpi), 96);
}
static void position_control(HWND window, int id, RECT units) {
    MapDialogRect(window, &units);
    MoveWindow(GetDlgItem(window, id), units.left, units.top, units.right - units.left,
               units.bottom - units.top, TRUE);
}
static void layout_installation(HWND window, InstallationState& state) {
    const bool editable = !state.options.present || state.options.installed;
    state.footer_top = editable ? 69 : 39;
    const int button_top = state.footer_top + 6, button_bottom = button_top + 14;
    const int primary_width = state.options.installed ? 42 : state.options.present ? 77 : 50;
    position_control(window, IDOK, {158 - primary_width, button_top, 158, button_bottom});
    position_control(window, IDCANCEL, {164, button_top, 208, button_bottom});
    position_control(window, IDC_INSTALL_UNINSTALL, {12, button_top, 69, button_bottom});
    RECT height{0, 0, 220, state.footer_top + 26}, bounds{}, client{};
    MapDialogRect(window, &height);
    GetWindowRect(window, &bounds);
    GetClientRect(window, &client);
    SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left,
                 bounds.bottom - bounds.top - client.bottom + height.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void center_dialog(HWND window, HWND owner) {
    RECT bounds{}, parent{};
    GetWindowRect(window, &bounds);
    GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor))
        return;
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    const auto& work = monitor.rcWork;
    const int x = std::clamp((parent.left + parent.right - width) / 2, work.left,
                             std::max(work.left, work.right - width));
    const int y = std::clamp((parent.top + parent.bottom - height) / 2, work.top,
                             std::max(work.top, work.bottom - height));
    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void draw_button(HWND window, InstallationState& state, const NMCUSTOMDRAW& draw) {
    const HWND button = draw.hdr.hwndFrom;
    const int id = GetDlgCtrlID(button);
    const bool checkbox = id == IDC_INSTALL_SHORTCUT || id == IDC_INSTALL_PDF;
    const bool pressed = (draw.uItemState & CDIS_SELECTED) != 0;
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0;
    const bool primary = id == IDOK;
    const int saved = SaveDC(draw.hdc);
    FillRect(draw.hdc, &draw.rc, checkbox ? state.background : state.footer);
    RECT label = draw.rc;
    if (checkbox) {
        const bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const int size = scaled(state, 16);
        RECT box{draw.rc.left, (draw.rc.top + draw.rc.bottom - size) / 2, draw.rc.left + size,
                 (draw.rc.top + draw.rc.bottom + size) / 2};
        SelectObject(draw.hdc, GetStockObject(DC_PEN));
        SelectObject(draw.hdc, GetStockObject(DC_BRUSH));
        SetDCPenColor(draw.hdc, checked || focused ? focus : muted);
        SetDCBrushColor(draw.hdc, checked ? focus : pressed ? selected : surface);
        RoundRect(draw.hdc, box.left, box.top, box.right, box.bottom, scaled(state, 4), scaled(state, 4));
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, scaled(state, 2), RGB(255, 255, 255));
            const auto old = SelectObject(draw.hdc, pen);
            const POINT points[] = {{box.left + scaled(state, 3), box.top + scaled(state, 8)},
                                    {box.left + scaled(state, 7), box.top + scaled(state, 12)},
                                    {box.left + scaled(state, 13), box.top + scaled(state, 4)}};
            Polyline(draw.hdc, points, int(std::size(points)));
            SelectObject(draw.hdc, old);
            DeleteObject(pen);
        }
        label.left += scaled(state, 25);
    } else {
        control(state.painter, draw.hdc, draw.rc, state.dpi, chrome,
                primary   ? focus
                : pressed ? selected
                          : surface,
                primary || focused ? focus : border, 1);
    }
    wchar_t caption[128]{};
    GetWindowTextW(button, caption, int(std::size(caption)));
    SelectObject(draw.hdc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
    SetBkMode(draw.hdc, TRANSPARENT);
    SetTextColor(draw.hdc, primary ? RGB(255, 255, 255) : text);
    const bool hide_accelerators = (draw.uItemState & CDIS_SHOWKEYBOARDCUES) == 0;
    DrawTextW(draw.hdc, caption, -1, &label,
              DT_SINGLELINE | DT_VCENTER | (checkbox ? DT_LEFT : DT_CENTER) |
                  (hide_accelerators ? DT_HIDEPREFIX : 0));
    if (focused && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT ring = checkbox ? label : draw.rc;
        InflateRect(&ring, -scaled(state, 3), -scaled(state, 3));
        DrawFocusRect(draw.hdc, &ring);
    }
    RestoreDC(draw.hdc, saved);
}
static INT_PTR CALLBACK installation_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto state = reinterpret_cast<InstallationState*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<InstallationState*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        const auto& options = state->options;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
        DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
        state->dpi = GetDpiForWindow(window);
        SetWindowTextW(window, options.present ? L"FastPDF installation" : L"Install FastPDF");
        SetDlgItemTextW(window, IDC_INSTALL_FOLDER, options.folder.c_str());
        SetDlgItemTextW(window, IDOK,
                        options.installed ? L"Apply"
                        : options.present ? L"Open FastPDF"
                                          : L"Install");
        CheckDlgButton(window, IDC_INSTALL_SHORTCUT, options.shortcut ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(window, IDC_INSTALL_PDF, options.default_pdf ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(GetDlgItem(window, IDC_INSTALL_UNINSTALL), options.present ? SW_SHOW : SW_HIDE);
        if (options.present && !options.installed) {
            ShowWindow(GetDlgItem(window, IDC_INSTALL_SHORTCUT), SW_HIDE);
            ShowWindow(GetDlgItem(window, IDC_INSTALL_PDF), SW_HIDE);
        }
        layout_installation(window, *state);
        center_dialog(window, GetParent(window));
        return TRUE;
    }
    if (!state)
        return FALSE;
    switch (message) {
    case WM_CTLCOLORDLG:
        return INT_PTR(state->background);
    case WM_CTLCOLORSTATIC:
        SetBkColor(HDC(wp), surface);
        SetTextColor(HDC(wp), text);
        return INT_PTR(state->background);
    case WM_ERASEBKGND: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        FillRect(HDC(wp), &bounds, state->background);
        RECT footer_rect{0, state->footer_top, 220, state->footer_top + 26};
        MapDialogRect(window, &footer_rect);
        footer_rect.right = bounds.right;
        footer_rect.bottom = bounds.bottom;
        FillRect(HDC(wp), &footer_rect, state->footer);
        SetDCBrushColor(HDC(wp), border);
        RECT line{0, footer_rect.top, bounds.right, footer_rect.top + scaled(*state, 1)};
        FillRect(HDC(wp), &line, HBRUSH(GetStockObject(DC_BRUSH)));
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lp);
        if (draw->hdr.code == NM_CUSTOMDRAW && draw->dwDrawStage == CDDS_PREPAINT) {
            draw_button(window, *state, *draw);
            SetWindowLongPtrW(window, DWLP_MSGRESULT, CDRF_SKIPDEFAULT);
            return TRUE;
        }
        break;
    }
    case WM_DPICHANGED:
        // Let the dialog manager scale the template before updating the compact footer layout.
        PostMessageW(window, WM_APP, 0, 0);
        break;
    case WM_APP:
        state->dpi = GetDpiForWindow(window);
        layout_installation(window, *state);
        InvalidateRect(window, nullptr, TRUE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (!state->options.present || state->options.installed) {
                state->options.shortcut = IsDlgButtonChecked(window, IDC_INSTALL_SHORTCUT) == BST_CHECKED;
                state->options.default_pdf = IsDlgButtonChecked(window, IDC_INSTALL_PDF) == BST_CHECKED;
            }
            EndDialog(window, INT_PTR(state->options.present && !state->options.installed
                                          ? InstallationAction::open
                                          : InstallationAction::apply));
            return TRUE;
        case IDC_INSTALL_UNINSTALL:
            if (state->options.present)
                EndDialog(window, INT_PTR(InstallationAction::uninstall));
            return TRUE;
        case IDCANCEL:
            EndDialog(window, INT_PTR(InstallationAction::cancel));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(window, INT_PTR(InstallationAction::cancel));
        return TRUE;
    }
    return FALSE;
}
InstallationAction show_installation_dialog(HINSTANCE instance, HWND owner, InstallationOptions& options) {
    InstallationState state{options};
    const auto result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_INSTALLATION), owner,
                                        installation_proc, LPARAM(&state));
    if (result == -1)
        throw std::runtime_error("Cannot open the FastPDF installation dialog");
    return InstallationAction(result);
}
} // namespace ui
