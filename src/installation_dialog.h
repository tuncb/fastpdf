#pragma once
#include <windows.h>
#include <string>

namespace ui {
enum class InstallationAction { cancel, apply, open, uninstall };
struct InstallationOptions {
    std::wstring folder;
    bool installed = false, present = false, shortcut = true, default_pdf = false;
};
InstallationAction show_installation_dialog(HINSTANCE instance, HWND owner, InstallationOptions& options);
} // namespace ui
