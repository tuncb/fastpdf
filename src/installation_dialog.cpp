#include "installation_dialog.h"
#include "resource.h"
#include <stdexcept>
#include "dialog_style.h"

namespace ui {
struct InstallationState {
    InstallationOptions& options;
    int footer_top = 69;
};
static void layout_installation(HWND window, InstallationState& state) {
    const bool editable = !state.options.present || state.options.installed;
    state.footer_top = editable ? 69 : 39;
    const int button_top = state.footer_top + 6, button_bottom = button_top + 14;
    const int primary_width = state.options.present && !state.options.installed ? 77 : 50;
    position_dialog_control(window, IDOK, {158 - primary_width, button_top, 158, button_bottom});
    position_dialog_control(window, IDCANCEL, {164, button_top, 208, button_bottom});
    position_dialog_control(window, IDC_INSTALL_UNINSTALL, {12, button_top, 69, button_bottom});
    RECT height{0, 0, 220, state.footer_top + 26}, bounds{}, client{};
    MapDialogRect(window, &height);
    GetWindowRect(window, &bounds);
    GetClientRect(window, &client);
    dialog_footer(window, state.footer_top);
    SetWindowPos(window, nullptr, 0, 0, bounds.right - bounds.left,
                 bounds.bottom - bounds.top - client.bottom + height.bottom,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static INT_PTR CALLBACK installation_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto state = reinterpret_cast<InstallationState*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<InstallationState*>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        const auto& options = state->options;
        SetWindowTextW(window, options.present ? L"FastPDF Installation" : L"Install FastPDF");
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
        style_dialog(window, state->footer_top);
        return TRUE;
    }
    if (!state)
        return FALSE;
    switch (message) {
    case WM_DPICHANGED:
        // Let the dialog manager scale the template before updating the compact footer layout.
        PostMessageW(window, WM_APP, 0, 0);
        break;
    case WM_APP:
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
        throw std::runtime_error("Cannot open the FastPDF Installation dialog");
    return InstallationAction(result);
}
} // namespace ui
