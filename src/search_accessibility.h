#pragma once
#include <windows.h>

namespace ui {
// Keep the automation tree bounded to the visible rows of an owner-data list.
bool enable_search_accessibility(HWND window);
} // namespace ui
