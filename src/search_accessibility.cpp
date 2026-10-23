#define CINTERFACE
#define CONST_VTABLE
#include "search_accessibility.h"
#include <commctrl.h>
#include <ole2.h>
#include <uiautomation.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cerrno>
#include <climits>
#include <cwchar>
#include <iterator>
#include <new>
#include <utility>

namespace ui {
// The system ListView accessibility bridge walks every owner-data row. Keep the UIA
// tree limited to the viewport and expose the rest through ItemContainer/VirtualizedItem.
// Native MSAA remains available. C COM vtables keep this provider procedural.
struct ResultProvider;
template <typename T> struct ProviderInterface {
    T api{};
    ResultProvider* owner = nullptr;
};
struct ResultProvider {
    ProviderInterface<IRawElementProviderSimple> simple;
    ProviderInterface<IRawElementProviderFragment> fragment;
    ProviderInterface<IRawElementProviderFragmentRoot> fragment_root;
    ProviderInterface<IItemContainerProvider> items;
    ProviderInterface<ISelectionProvider> selection;
    ProviderInterface<ISelectionItemProvider> selection_item;
    ProviderInterface<IVirtualizedItemProvider> virtual_item;
    ProviderInterface<IScrollItemProvider> scroll_item;
    ProviderInterface<IScrollProvider> scroll;
    std::atomic<ULONG> references{1};
    ResultProvider* root = nullptr;
    HWND window = nullptr;
    int index = -1;
    unsigned generation = 0;
    bool requested = false;
};
template <typename T> static ResultProvider& owner(T* value) {
    return *reinterpret_cast<ProviderInterface<T>*>(value)->owner;
}
template <typename T> static ULONG STDMETHODCALLTYPE add_ref(T* value) {
    return ++owner(value).references;
}
template <typename T> static ULONG STDMETHODCALLTYPE release(T* value) {
    auto& p = owner(value);
    const ULONG remaining = --p.references;
    if (!remaining) {
        if (p.root != &p)
            release(&p.root->simple.api);
        delete &p;
    }
    return remaining;
}
template <typename T> static HRESULT STDMETHODCALLTYPE query(T* value, REFIID iid, void** result) {
    if (!result)
        return E_POINTER;
    auto& p = owner(value);
    *result = nullptr;
    if (iid == IID_IUnknown || iid == IID_IRawElementProviderSimple)
        *result = &p.simple.api;
    else if (iid == IID_IRawElementProviderFragment)
        *result = &p.fragment.api;
    else if (p.index < 0) {
        if (iid == IID_IRawElementProviderFragmentRoot)
            *result = &p.fragment_root.api;
        else if (iid == IID_IItemContainerProvider)
            *result = &p.items.api;
        else if (iid == IID_ISelectionProvider)
            *result = &p.selection.api;
        else if (iid == IID_IScrollProvider)
            *result = &p.scroll.api;
    } else {
        if (iid == IID_ISelectionItemProvider)
            *result = &p.selection_item.api;
        else if (iid == IID_IVirtualizedItemProvider)
            *result = &p.virtual_item.api;
        else if (iid == IID_IScrollItemProvider)
            *result = &p.scroll_item.api;
    }
    if (!*result)
        return E_NOINTERFACE;
    add_ref(value);
    return S_OK;
}
static HWND live_window(const ResultProvider& p) {
    const auto window = p.root->window;
    if (!window ||
        (p.index >= 0 && (p.generation != p.root->generation || p.index >= ListView_GetItemCount(window))))
        return nullptr;
    return window;
}
static std::pair<int, int> visible_rows(HWND window) {
    const int first = ListView_GetTopIndex(window);
    // Include the partially visible row at the bottom.
    return {first, std::min(ListView_GetItemCount(window), first + ListView_GetCountPerPage(window) + 1)};
}
static bool realized(const ResultProvider& p, HWND window) {
    const auto [first, last] = visible_rows(window);
    return p.index < 0 || (p.index >= first && p.index < last);
}
static ResultProvider* make_provider(ResultProvider* root, int index);
static HRESULT item_provider(ResultProvider& p, int index, IRawElementProviderSimple** result) {
    *result = nullptr;
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (index < 0 || index >= ListView_GetItemCount(window))
        return S_OK;
    const auto item = make_provider(p.root, index);
    if (!item)
        return E_OUTOFMEMORY;
    *result = &item->simple.api;
    return S_OK;
}
static HRESULT string_value(VARIANT* result, const wchar_t* text) {
    result->bstrVal = SysAllocString(text);
    if (!result->bstrVal)
        return E_OUTOFMEMORY;
    result->vt = VT_BSTR;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE options(IRawElementProviderSimple*, ProviderOptions* result) {
    *result = ProviderOptions_ServerSideProvider;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE pattern(IRawElementProviderSimple* value, PATTERNID id, IUnknown** result) {
    *result = nullptr;
    auto& p = owner(value);
    if (!live_window(p))
        return UIA_E_ELEMENTNOTAVAILABLE;
    void* found = nullptr;
    if (p.index < 0) {
        if (id == UIA_ItemContainerPatternId)
            found = &p.items.api;
        else if (id == UIA_SelectionPatternId)
            found = &p.selection.api;
        else if (id == UIA_ScrollPatternId)
            found = &p.scroll.api;
    } else {
        if (id == UIA_VirtualizedItemPatternId)
            found = &p.virtual_item.api;
        else if (id == UIA_SelectionItemPatternId)
            found = &p.selection_item.api;
        else if (id == UIA_ScrollItemPatternId)
            found = &p.scroll_item.api;
    }
    if (found) {
        *result = static_cast<IUnknown*>(found);
        add_ref(value);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE property(IRawElementProviderSimple* value, PROPERTYID id, VARIANT* result) {
    VariantInit(result);
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    wchar_t text[8192]{};
    if (id == UIA_NamePropertyId) {
        if (p.index < 0)
            return string_value(result, L"Search results");
        ListView_GetItemText(window, p.index, 0, text, int(std::size(text)));
        return string_value(result, text);
    }
    if (id == UIA_AutomationIdPropertyId) {
        if (p.index < 0)
            swprintf_s(text, L"%d", GetDlgCtrlID(window));
        else
            swprintf_s(text, L"result-%d", p.index + 1);
        return string_value(result, text);
    }
    if (id == UIA_ItemStatusPropertyId && p.index < 0) {
        swprintf_s(text, L"%d results", ListView_GetItemCount(window));
        return string_value(result, text);
    }
    if (id == UIA_ControlTypePropertyId) {
        result->vt = VT_I4;
        result->lVal = p.index < 0 ? UIA_ListControlTypeId : UIA_ListItemControlTypeId;
    } else if (id == UIA_IsControlElementPropertyId || id == UIA_IsContentElementPropertyId ||
               id == UIA_IsKeyboardFocusablePropertyId || id == UIA_IsEnabledPropertyId ||
               id == UIA_HasKeyboardFocusPropertyId || id == UIA_IsOffscreenPropertyId) {
        result->vt = VT_BOOL;
        bool enabled = true;
        if (id == UIA_IsEnabledPropertyId)
            enabled = IsWindowEnabled(window) != FALSE;
        else if (id == UIA_HasKeyboardFocusPropertyId)
            enabled = GetFocus() == window &&
                      (p.index < 0 || ListView_GetNextItem(window, -1, LVNI_FOCUSED) == p.index);
        else if (id == UIA_IsOffscreenPropertyId)
            enabled = !IsWindowVisible(window) || !realized(p, window);
        result->boolVal = enabled ? VARIANT_TRUE : VARIANT_FALSE;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE host(IRawElementProviderSimple* value, IRawElementProviderSimple** result) {
    *result = nullptr;
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    return p.index < 0 ? UiaHostProviderFromHwnd(window, result) : S_OK;
}
static HRESULT STDMETHODCALLTYPE navigate(IRawElementProviderFragment* value, NavigateDirection direction,
                                          IRawElementProviderFragment** result) {
    *result = nullptr;
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (p.index >= 0 && direction == NavigateDirection_Parent)
        return query(&p.root->simple.api, IID_IRawElementProviderFragment, reinterpret_cast<void**>(result));
    const auto [first, last] = visible_rows(window);
    int index = -1;
    if (p.index < 0 && direction == NavigateDirection_FirstChild)
        index = first;
    else if (p.index < 0 && direction == NavigateDirection_LastChild)
        index = last - 1;
    else if (p.index >= 0 && direction == NavigateDirection_NextSibling)
        index = p.index + 1;
    else if (p.index >= 0 && direction == NavigateDirection_PreviousSibling)
        index = p.index - 1;
    if (index >= first && index < last) {
        auto item = make_provider(p.root, index);
        if (!item)
            return E_OUTOFMEMORY;
        *result = &item->fragment.api;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE runtime_id(IRawElementProviderFragment* value, SAFEARRAY** result) {
    *result = nullptr;
    auto& p = owner(value);
    if (!live_window(p))
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (p.index < 0)
        return S_OK;
    *result = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!*result)
        return E_OUTOFMEMORY;
    int ids[] = {UiaAppendRuntimeId, int(p.generation), p.index};
    for (LONG i = 0; i < 3; ++i)
        SafeArrayPutElement(*result, &i, &ids[i]);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE rectangle(IRawElementProviderFragment* value, UiaRect* result) {
    *result = {};
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    RECT bounds{};
    if (p.index < 0)
        GetWindowRect(window, &bounds);
    else {
        if (!realized(p, window))
            return UIA_E_ELEMENTNOTAVAILABLE;
        ListView_GetItemRect(window, p.index, &bounds, LVIR_BOUNDS);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    }
    *result = {double(bounds.left), double(bounds.top), double(bounds.right - bounds.left),
               double(bounds.bottom - bounds.top)};
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE embedded(IRawElementProviderFragment*, SAFEARRAY** result) {
    *result = nullptr;
    return S_OK;
}
template <typename T> static HRESULT STDMETHODCALLTYPE realize(T* value) {
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    ListView_EnsureVisible(window, p.index, FALSE);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE focus(IRawElementProviderFragment* value) {
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (p.index >= 0) {
        ListView_EnsureVisible(window, p.index, FALSE);
        ListView_SetItemState(window, p.index, LVIS_FOCUSED, LVIS_FOCUSED);
    }
    SetFocus(window);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fragment_root(IRawElementProviderFragment* value,
                                               IRawElementProviderFragmentRoot** result) {
    auto& p = owner(value);
    if (!live_window(p)) {
        *result = nullptr;
        return UIA_E_ELEMENTNOTAVAILABLE;
    }
    return query(&p.root->simple.api, IID_IRawElementProviderFragmentRoot, reinterpret_cast<void**>(result));
}
static HRESULT root_item(ResultProvider& p, int index, IRawElementProviderFragment** result) {
    IRawElementProviderSimple* item = nullptr;
    *result = nullptr;
    const auto hr = item_provider(p, index, &item);
    if (FAILED(hr) || !item)
        return hr;
    const auto found = query(item, IID_IRawElementProviderFragment, reinterpret_cast<void**>(result));
    release(item);
    return found;
}
static HRESULT STDMETHODCALLTYPE from_point(IRawElementProviderFragmentRoot* value, double x, double y,
                                            IRawElementProviderFragment** result) {
    auto& p = owner(value);
    const auto window = live_window(p);
    *result = nullptr;
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    LVHITTESTINFO hit{{LONG(x), LONG(y)}};
    ScreenToClient(window, &hit.pt);
    RECT bounds{};
    GetClientRect(window, &bounds);
    if (!PtInRect(&bounds, hit.pt))
        return S_OK;
    const int index = ListView_HitTest(window, &hit);
    return index >= 0
               ? root_item(p, index, result)
               : query(&p.fragment.api, IID_IRawElementProviderFragment, reinterpret_cast<void**>(result));
}
static HRESULT STDMETHODCALLTYPE focused(IRawElementProviderFragmentRoot* value,
                                         IRawElementProviderFragment** result) {
    auto& p = owner(value);
    const auto window = live_window(p);
    *result = nullptr;
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    return GetFocus() == window ? root_item(p, ListView_GetNextItem(window, -1, LVNI_FOCUSED), result) : S_OK;
}
static HRESULT STDMETHODCALLTYPE find_item(IItemContainerProvider* value, IRawElementProviderSimple* after,
                                           PROPERTYID id, VARIANT property_value,
                                           IRawElementProviderSimple** result) {
    *result = nullptr;
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    int first = 0;
    if (after) {
        // UI Automation returns the original server provider for startAfter.
        if (after->lpVtbl != p.simple.api.lpVtbl)
            return E_INVALIDARG;
        auto& previous = owner(after);
        if (previous.root != p.root || previous.index < 0 || !live_window(previous))
            return E_INVALIDARG;
        first = previous.index + 1;
    }
    if (!id)
        return item_provider(p, first, result);
    if (id == UIA_AutomationIdPropertyId && property_value.vt == VT_BSTR && property_value.bstrVal) {
        const auto text = property_value.bstrVal;
        if (wcsncmp(text, L"result-", 7) != 0)
            return S_OK;
        wchar_t* end = nullptr;
        errno = 0;
        const long number = wcstol(text + 7, &end, 10);
        if (!errno && *end == 0 && number > first)
            return item_provider(p, int(number - 1), result);
        return S_OK;
    }
    if (id == UIA_SelectionItemIsSelectedPropertyId && property_value.vt == VT_BOOL) {
        const int selected = ListView_GetNextItem(window, -1, LVNI_SELECTED);
        if (property_value.boolVal)
            return item_provider(p, selected >= first ? selected : -1, result);
        return item_provider(p, first == selected ? first + 1 : first, result);
    }
    if (id == UIA_NamePropertyId && property_value.vt == VT_BSTR && property_value.bstrVal) {
        wchar_t name[8192]{};
        for (int i = first, count = ListView_GetItemCount(window); i < count; ++i) {
            ListView_GetItemText(window, i, 0, name, int(std::size(name)));
            if (wcscmp(name, property_value.bstrVal) == 0)
                return item_provider(p, i, result);
        }
        return S_OK;
    }
    return E_INVALIDARG;
}
static HRESULT STDMETHODCALLTYPE selection(ISelectionProvider* value, SAFEARRAY** result) {
    *result = nullptr;
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    const int index = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    *result = SafeArrayCreateVector(VT_UNKNOWN, 0, index < 0 ? 0 : 1);
    if (!*result)
        return E_OUTOFMEMORY;
    if (index >= 0) {
        IRawElementProviderSimple* item = nullptr;
        const auto hr = item_provider(p, index, &item);
        if (FAILED(hr)) {
            SafeArrayDestroy(*result);
            *result = nullptr;
            return hr;
        }
        LONG at = 0;
        SafeArrayPutElement(*result, &at, item);
        release(item);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE false_selection(ISelectionProvider*, BOOL* result) {
    *result = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE select_item(ISelectionItemProvider* value) {
    auto& p = owner(value);
    const auto window = live_window(p);
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    ListView_EnsureVisible(window, p.index, FALSE);
    ListView_SetItemState(window, p.index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE add_selection(ISelectionItemProvider* value) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    const int selected = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    return selected < 0 || selected == owner(value).index ? select_item(value) : UIA_E_INVALIDOPERATION;
}
static HRESULT STDMETHODCALLTYPE remove_selection(ISelectionItemProvider* value) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    ListView_SetItemState(window, owner(value).index, 0, LVIS_SELECTED);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE selected(ISelectionItemProvider* value, BOOL* result) {
    const auto window = live_window(owner(value));
    *result = FALSE;
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    *result = (ListView_GetItemState(window, owner(value).index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE container(ISelectionItemProvider* value,
                                           IRawElementProviderSimple** result) {
    return query(&owner(value).root->simple.api, IID_IRawElementProviderSimple,
                 reinterpret_cast<void**>(result));
}
static SCROLLINFO scroll_info(HWND window) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(window, SB_VERT, &info);
    return info;
}
static HRESULT STDMETHODCALLTYPE scroll_by(IScrollProvider* value, ScrollAmount horizontal,
                                           ScrollAmount vertical) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (horizontal != ScrollAmount_NoAmount)
        return UIA_E_INVALIDOPERATION;
    const int code = vertical == ScrollAmount_LargeDecrement   ? SB_PAGEUP
                     : vertical == ScrollAmount_SmallDecrement ? SB_LINEUP
                     : vertical == ScrollAmount_LargeIncrement ? SB_PAGEDOWN
                     : vertical == ScrollAmount_SmallIncrement ? SB_LINEDOWN
                                                               : -1;
    if (code >= 0)
        SendMessageW(window, WM_VSCROLL, WPARAM(code), 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE scroll_percent(IScrollProvider* value, double horizontal, double vertical) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    if (horizontal != UIA_ScrollPatternNoScroll)
        return UIA_E_INVALIDOPERATION;
    if (vertical == UIA_ScrollPatternNoScroll)
        return S_OK;
    if (!std::isfinite(vertical) || vertical < 0 || vertical > 100)
        return E_INVALIDARG;
    const auto info = scroll_info(window);
    const int range = std::max(0, info.nMax - int(info.nPage) + 1);
    RECT row{};
    if (range && ListView_GetItemRect(window, 0, &row, LVIR_BOUNDS)) {
        const int target = int(std::round(range * vertical / 100));
        // EnsureVisible jumps to distant rows without overflowing a pixel scroll delta.
        ListView_EnsureVisible(window, target, FALSE);
        const auto delta =
            static_cast<long long>(target - ListView_GetTopIndex(window)) * (row.bottom - row.top);
        ListView_Scroll(
            window, 0,
            int(std::clamp(delta, static_cast<long long>(INT_MIN), static_cast<long long>(INT_MAX))));
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE horizontal_percent(IScrollProvider*, double* result) {
    *result = UIA_ScrollPatternNoScroll;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vertical_percent(IScrollProvider* value, double* result) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    const auto info = scroll_info(window);
    const int range = std::max(0, info.nMax - int(info.nPage) + 1);
    *result = range ? 100.0 * info.nPos / range : UIA_ScrollPatternNoScroll;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE horizontal_size(IScrollProvider*, double* result) {
    *result = 100;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vertical_size(IScrollProvider* value, double* result) {
    const auto window = live_window(owner(value));
    if (!window)
        return UIA_E_ELEMENTNOTAVAILABLE;
    const auto info = scroll_info(window);
    *result = info.nMax > 0 ? std::min(100.0, 100.0 * info.nPage / (info.nMax + 1)) : 100;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE horizontal_scrollable(IScrollProvider*, BOOL* result) {
    *result = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE vertical_scrollable(IScrollProvider* value, BOOL* result) {
    double percent = UIA_ScrollPatternNoScroll;
    const auto hr = vertical_percent(value, &percent);
    *result = percent != UIA_ScrollPatternNoScroll;
    return hr;
}

static ResultProvider* make_provider(ResultProvider* root, int index) {
    static const IRawElementProviderSimpleVtbl simple = {query,   add_ref,  release, options,
                                                         pattern, property, host};
    static const IRawElementProviderFragmentVtbl fragment = {
        query, add_ref, release, navigate, runtime_id, rectangle, embedded, focus, fragment_root};
    static const IRawElementProviderFragmentRootVtbl fragment_root_table = {query, add_ref, release,
                                                                            from_point, focused};
    static const IItemContainerProviderVtbl items = {query, add_ref, release, find_item};
    static const ISelectionProviderVtbl selection_table = {query,     add_ref,         release,
                                                           selection, false_selection, false_selection};
    static const ISelectionItemProviderVtbl selection_item = {
        query, add_ref, release, select_item, add_selection, remove_selection, selected, container};
    static const IVirtualizedItemProviderVtbl virtual_item = {query, add_ref, release, realize};
    static const IScrollItemProviderVtbl scroll_item = {query, add_ref, release, realize};
    static const IScrollProviderVtbl scroll = {query,
                                               add_ref,
                                               release,
                                               scroll_by,
                                               scroll_percent,
                                               horizontal_percent,
                                               vertical_percent,
                                               horizontal_size,
                                               vertical_size,
                                               horizontal_scrollable,
                                               vertical_scrollable};
    auto p = new (std::nothrow) ResultProvider;
    if (!p)
        return nullptr;
    p->simple = {{&simple}, p};
    p->fragment = {{&fragment}, p};
    p->fragment_root = {{&fragment_root_table}, p};
    p->items = {{&items}, p};
    p->selection = {{&selection_table}, p};
    p->selection_item = {{&selection_item}, p};
    p->virtual_item = {{&virtual_item}, p};
    p->scroll_item = {{&scroll_item}, p};
    p->scroll = {{&scroll}, p};
    p->root = root ? root : p;
    p->index = index;
    if (root) {
        add_ref(&root->simple.api);
        p->generation = root->generation;
    }
    return p;
}
static LRESULT CALLBACK result_accessibility_proc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                                                  UINT_PTR id, DWORD_PTR data) {
    auto& p = *reinterpret_cast<ResultProvider*>(data);
    if (message == WM_GETOBJECT && LONG(lp) == UiaRootObjectId) {
        p.requested = true;
        return UiaReturnRawElementProvider(window, wp, lp, &p.simple.api);
    }
    if (message == WM_NCDESTROY) {
        p.window = nullptr;
        if (p.requested)
            UiaDisconnectProvider(&p.simple.api);
        RemoveWindowSubclass(window, result_accessibility_proc, id);
        release(&p.simple.api);
        return DefSubclassProc(window, message, wp, lp);
    }
    const bool changed = message == LVM_SETITEMCOUNT || message == LVM_SETITEMSTATE ||
                         message == LVM_ENSUREVISIBLE || message == LVM_SCROLL || message == WM_VSCROLL ||
                         message == WM_MOUSEWHEEL || message == WM_KEYDOWN || message == WM_LBUTTONDOWN ||
                         message == WM_LBUTTONUP || message == WM_SIZE || message == WM_SETFOCUS;
    if (!changed)
        return DefSubclassProc(window, message, wp, lp);
    const int count = ListView_GetItemCount(window), top = ListView_GetTopIndex(window);
    const int before = ListView_GetNextItem(window, -1, LVNI_SELECTED);
    const int focused_before = ListView_GetNextItem(window, -1, LVNI_FOCUSED);
    if (message == LVM_SETITEMCOUNT && int(wp) < count)
        ++p.generation;
    const auto result = DefSubclassProc(window, message, wp, lp);
    if (p.requested && UiaClientsAreListening()) {
        if (count != ListView_GetItemCount(window) || top != ListView_GetTopIndex(window) ||
            message == WM_SIZE)
            UiaRaiseStructureChangedEvent(&p.simple.api, StructureChangeType_ChildrenInvalidated, nullptr, 0);
        const int current = ListView_GetNextItem(window, -1, LVNI_SELECTED);
        if (current >= 0 && current != before) {
            if (auto item = make_provider(&p, current)) {
                UiaRaiseAutomationEvent(&item->simple.api, UIA_SelectionItem_ElementSelectedEventId);
                release(&item->simple.api);
            }
        }
        const int focused_now = ListView_GetNextItem(window, -1, LVNI_FOCUSED);
        if (GetFocus() == window && focused_now >= 0 &&
            (focused_now != focused_before || message == WM_SETFOCUS)) {
            if (auto item = make_provider(&p, focused_now)) {
                UiaRaiseAutomationEvent(&item->simple.api, UIA_AutomationFocusChangedEventId);
                release(&item->simple.api);
            }
        }
    }
    return result;
}
bool enable_search_accessibility(HWND window) {
    const auto provider = make_provider(nullptr, -1);
    if (!provider)
        return false;
    provider->window = window;
    if (!SetWindowSubclass(window, result_accessibility_proc, 1, DWORD_PTR(provider))) {
        release(&provider->simple.api);
        return false;
    }
    return true;
}
} // namespace ui
