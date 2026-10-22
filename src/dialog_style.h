#pragma once
#include <windows.h>
#include <commctrl.h>
#include <span>
#include <string>

namespace ui {
struct DialogColors {
    bool dark;
    COLORREF body, footer, field, hover, pressed, border, text, muted, accent, link, on_accent;
};
DialogColors system_dialog_colors();
void enable_system_dialog_theme();
// Dimensions are dialog units, using the template's Segoe UI 10 font.
// A negative footer keeps the full client area as the body (embedded panels).
void style_dialog(HWND window, int footer_top, int primary = IDOK, bool center = true);
void style_common_dialog(HWND window);
void refresh_dialog_style(HWND window);
void dialog_footer(HWND window, int footer_top);
void position_dialog_control(HWND window, int id, RECT units);
// Keep the full native control name for accessibility and its tooltip.
void compact_button(HWND button, const wchar_t* caption);
int show_prompt(HWND owner, const wchar_t* title, const std::wstring& content,
                std::span<const TASKDIALOG_BUTTON> buttons, int default_button = IDCANCEL,
                const wchar_t* checkbox = nullptr, BOOL* checked = nullptr);
int message_box(HWND owner, const wchar_t* content, const wchar_t* title, UINT flags);
} // namespace ui
