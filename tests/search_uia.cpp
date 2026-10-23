#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}
static void checked(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::fprintf(stderr, "%s: 0x%08lx\n", message, static_cast<unsigned long>(hr));
        std::exit(1);
    }
}
static void check_name(IUIAutomationElement* element, const wchar_t* prefix) {
    BSTR name = nullptr;
    checked(element->get_CurrentName(&name), "Read result name");
    require(name && wcsncmp(name, prefix, wcslen(prefix)) == 0, "Wrong result name");
    SysFreeString(name);
}
static int count_children(IUIAutomationElement* root, IUIAutomationCondition* condition) {
    IUIAutomationElementArray* children = nullptr;
    checked(root->FindAll(TreeScope_Descendants, condition, &children), "Read result tree");
    int count = 0;
    checked(children->get_Length(&count), "Count result children");
    children->Release();
    require(count < 100, "Accessibility enumerated results outside the viewport");
    return count;
}
int wmain(int argc, wchar_t** argv) {
    // Keep the last completed check in CI logs even if a system UIA call crashes.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    require(argc == 3, "Expected results HWND and total count");
    const HWND window = reinterpret_cast<HWND>(_wcstoui64(argv[1], nullptr, 10));
    const int total = _wtoi(argv[2]);
    checked(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
    IUIAutomation* automation = nullptr;
    checked(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)),
            "Create UI Automation");
    IUIAutomationElement* root = nullptr;
    checked(automation->ElementFromHandle(window, &root), "Get result list");
    IUIAutomationCondition* condition = nullptr;
    checked(automation->CreateTrueCondition(&condition), "Create condition");
    require(count_children(root, condition) > 0, "No visible results in accessibility tree");
    check_name(root, L"Search results");

    IUIAutomationItemContainerPattern* container = nullptr;
    checked(root->GetCurrentPatternAs(UIA_ItemContainerPatternId, IID_PPV_ARGS(&container)),
            "Get virtual result container");
    VARIANT id{};
    wchar_t text[64]{};
    swprintf_s(text, L"result-%d", total);
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(text);
    IUIAutomationElement* last = nullptr;
    checked(container->FindItemByProperty(nullptr, UIA_AutomationIdPropertyId, id, &last),
            "Find last result");
    VariantClear(&id);
    require(last != nullptr, "Last virtual result missing");
    check_name(last, L"Page 241:");
    BOOL offscreen = FALSE;
    checked(last->get_CurrentIsOffscreen(&offscreen), "Read virtual result visibility");
    require(offscreen, "Last result should start outside viewport");
    IUIAutomationVirtualizedItemPattern* virtual_item = nullptr;
    checked(last->GetCurrentPatternAs(UIA_VirtualizedItemPatternId, IID_PPV_ARGS(&virtual_item)),
            "Get virtual result pattern");
    checked(virtual_item->Realize(), "Show last result");
    virtual_item->Release();
    RECT bounds{};
    checked(last->get_CurrentBoundingRectangle(&bounds), "Read realized result bounds");
    require(bounds.bottom > bounds.top && bounds.right > bounds.left, "Realized result has no bounds");
    IUIAutomationSelectionItemPattern* selection_item = nullptr;
    checked(last->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&selection_item)),
            "Get result selection pattern");
    checked(selection_item->Select(), "Select last result");
    BOOL selected = FALSE;
    checked(selection_item->get_CurrentIsSelected(&selected), "Read selection");
    require(selected, "Last result was not selected");
    selection_item->Release();
    IUIAutomationSelectionPattern* selection = nullptr;
    checked(root->GetCurrentPatternAs(UIA_SelectionPatternId, IID_PPV_ARGS(&selection)),
            "Get list selection");
    IUIAutomationElementArray* selected_items = nullptr;
    checked(selection->GetCurrentSelection(&selected_items), "Read list selection");
    int selected_count = 0;
    checked(selected_items->get_Length(&selected_count), "Count selected results");
    require(selected_count == 1, "Wrong number of selected results");
    IUIAutomationElement* selected_item = nullptr;
    checked(selected_items->GetElement(0, &selected_item), "Read selected result");
    check_name(selected_item, L"Page 241:");
    selected_item->Release();
    selected_items->Release();
    selection->Release();
    require(count_children(root, condition) > 0, "No accessible rows after scrolling");

    IUIAutomationElement* next = nullptr;
    checked(container->FindItemByProperty(last, 0, VARIANT{}, &next), "Find after last result");
    require(next == nullptr, "Unexpected row after last result");
    IUIAutomationScrollPattern* scroll = nullptr;
    checked(root->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&scroll)),
            "Get result scroll pattern");
    checked(scroll->SetScrollPercent(UIA_ScrollPatternNoScroll, 0), "Scroll to first result");
    double percent = -1;
    checked(scroll->get_CurrentVerticalScrollPercent(&percent), "Read scroll position");
    require(percent == 0, "Did not scroll to first result");
    scroll->Release();

    // Keep a client reading the tree while the parent posts keyboard input and replaces the query.
    std::puts("ready");
    std::fflush(stdout);
    const auto deadline = GetTickCount64() + 10000;
    int scans = 0;
    while (GetTickCount64() < deadline) {
        const int count = count_children(root, condition);
        ++scans;
        if (!count) {
            std::puts("Query cleared; checking the stale result");
            BSTR stale_name = nullptr;
            const auto hr = last->get_CurrentName(&stale_name);
            SysFreeString(stale_name);
            require(hr == UIA_E_ELEMENTNOTAVAILABLE, "Old query result is still accessible");
            std::printf("UIA: %d bounded tree scans; virtual navigation and stale result checks passed\n",
                        scans);
            last->Release();
            container->Release();
            condition->Release();
            root->Release();
            automation->Release();
            std::puts("UI Automation interfaces released");
            CoUninitialize();
            return 0;
        }
        Sleep(5);
    }
    require(false, "Query was not replaced during accessibility reads");
}
