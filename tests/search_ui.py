"""Check real search navigation without sending desktop input."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys

from search_fixture import search_document
from search_accessibility import result_row
from ui_toolbar import user, wait_for, find_window, text, enter, rect

user.ScreenToClient.argtypes = [w.HWND, c.POINTER(w.POINT)]

def visible(control):
    # The test window stays hidden. Check each child's own visibility flag.
    return bool(user.GetWindowLongW(control, -16) & 0x10000000)


def run(exe, output):
    output.mkdir(parents=True, exist_ok=True)
    document = output / "product-guide.pdf"
    search_document(document)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), str(document)], startupinfo=startup)
    window = None
    try:
        window = wait_for(lambda: find_window(process.pid), "reader")
        toolbar = wait_for(lambda: user.GetDlgItem(window, 202), "toolbar")
        page = wait_for(lambda: user.GetDlgItem(toolbar, 203), "page field")
        wait_for(lambda: text(page) == "1", "loaded page")
        tabs, bookmarks, pages = [user.GetDlgItem(window, control) for control in (208, 205, 200)]
        assert user.SendMessageW(tabs, 0x1304, 0, 0) == 2, "Unexpected tabs before Find"
        assert visible(bookmarks), "Bookmarks did not open"
        root = user.SendMessageW(bookmarks, 0x110A, 0, 0)
        assert root, "Missing bookmark fixture"
        user.SendMessageW(bookmarks, 0x1102, 2, root)  # Expand the first branch.
        user.MoveWindow(window, 50, 50, 1000, 700, True)
        zoom = user.GetDlgItem(toolbar, 204)
        enter(zoom, "200")
        wait_for(lambda: text(zoom) == "200%", "zoom")
        user.SendMessageW(window, 0x111, 11, 0)
        bar = user.GetDlgItem(window, 206)
        query, count = user.GetDlgItem(bar, 201), user.GetDlgItem(bar, 1046)
        results = user.GetDlgItem(window, 207)
        summary = user.GetDlgItem(window, 209)
        assert visible(bookmarks), "Opening Find hid bookmarks"
        assert user.SendMessageW(tabs, 0x1304, 0, 0) == 3, "Missing Results tab"

        def expect(value):
            wait_for(lambda: text(count) == value, f"count {value}, currently {text(count)}")

        def command(control):
            user.SendMessageW(bar, 0x111, control, 0)

        def type_query(value):
            buffer = c.create_unicode_buffer(value)
            user.SendMessageW(query, 0xC, 0, c.addressof(buffer))

        first_bookmark = user.SendMessageW(bookmarks, 0x110A, 5, 0)
        type_query("warranty")
        expect("1 of 8")
        assert visible(bookmarks), "Search replaced bookmarks without a request"
        assert text(summary) == "8 matches on 3 pages", "Missing results summary"
        assert not visible(user.GetDlgItem(bar, 1047)), "Completed search kept a redundant status row"
        user.SendMessageW(query, 0x100, 13, 0)
        expect("1 of 8")
        assert visible(results), "First Enter did not open Results"
        result_area = w.RECT()
        user.GetClientRect(results, c.byref(result_area))
        assert not (user.GetWindowLongW(results, -16) & 0x100000), \
            f"Opening Results added a horizontal scrollbar: client {result_area.right}, column {user.SendMessageW(results, 0x101D, 0, 0)}"
        assert text(page) == "1"
        user.SendMessageW(window, 0x100, 0x72, 0)  # F3
        expect("2 of 8")
        assert text(page) == "1", "F3 skipped a match on the same page"
        command(1041)
        expect("1 of 8")
        for number in (2, 3, 4):
            user.SendMessageW(query, 0x100, 13, 0)
            expect(f"{number} of 8")
            assert text(page) == "1", "Enter skipped a match on the same page"
        user.GetScrollPos.argtypes = [w.HWND, c.c_int]
        wait_for(lambda: user.GetScrollPos(window, 1) > 0, "scroll to the match near the page bottom")
        command(1042)
        expect("5 of 8")
        wait_for(lambda: text(page) == "2", "next matching page")
        command(1041)
        expect("4 of 8")
        wait_for(lambda: text(page) == "1", "previous matching page")
        for _ in range(4):
            command(1041)
        expect("8 of 8")
        wait_for(lambda: text(page) == "3", "reverse wrap")
        command(1042)
        expect("1 of 8")

        assert user.SendMessageW(results, 0x1004, 0, 0) == 8, "Missing results"
        name, _ = result_row(results, 0)
        assert name.startswith("Page 1:") and "warranty" in name, "Missing accessible result text"
        user.SendMessageW(results, 0x100, 0x24, 0)  # Home, then Down selects the sixth result.
        for _ in range(5):
            user.SendMessageW(results, 0x100, 0x28, 0)
        expect("6 of 8")
        wait_for(lambda: text(page) == "2", "result selection")

        def select_tab(index, control):
            user.SendMessageW(tabs, 0x1330, index, 0)  # Native tab focus changes selection.
            wait_for(lambda: user.SendMessageW(tabs, 0x130B, 0, 0) == index, "selected tab")
            assert visible(control), "Tab did not show its view"
            assert sum(bool(visible(view)) for view in (pages, bookmarks, results)) == 1
            assert text(query) == "warranty", "Switching tabs cleared the query"
            expect("6 of 8")
            assert text(page) == "2", "Switching tabs changed the page"
            assert user.SendMessageW(results, 0x100C, -1, 2) == 5, "Switching tabs lost the selected match"

        select_tab(1, bookmarks)
        assert user.SendMessageW(bookmarks, 0x1127, root, 0x20) & 0x20, "Switching tabs collapsed bookmarks"
        assert user.SendMessageW(bookmarks, 0x110A, 5, 0) == first_bookmark, "Search moved the bookmark scroll position"
        user.SendMessageW(query, 0x100, 13, 0)
        expect("6 of 8")
        assert visible(results), "Enter did not reveal the current match from Bookmarks"
        select_tab(1, bookmarks)
        user.SendMessageW(tabs, 0x100, 0x27, 0)  # Right selects Results.
        assert visible(results), "Arrow-key tab navigation failed"
        user.SendMessageW(window, 0x111, 10, 0)  # Bookmarks toolbar command from Results.
        assert visible(bookmarks), "Bookmarks command closed the sidebar from Results"
        select_tab(0, pages)
        select_tab(2, results)
        user.SendMessageW(window, 0x111, 9, 0)
        assert visible(pages), "Pages command closed the sidebar from Results"
        select_tab(1, bookmarks)
        select_tab(2, results)
        assert not user.GetDlgItem(bar, 1043) and not user.GetDlgItem(bar, 1044), "Obsolete search buttons remain"
        assert "Previous match" in text(user.GetDlgItem(bar, 1041)), "Icon lost its accessible name"
        assert "Close find" in text(user.GetDlgItem(bar, 1045)), "Close icon lost its accessible name"

        def toggle(control):
            user.SendMessageW(user.GetDlgItem(bar, control), 0xF5, 0, 0)  # BM_CLICK

        toggle(1050)
        wait_for(lambda: text(count).endswith(" of 7"), "match case toggle")
        toggle(1051)
        wait_for(lambda: text(count).endswith(" of 6"), "case and whole words")
        toggle(1053)
        enter(query, r"warr[a-z]+")
        wait_for(lambda: text(count).endswith(" of 7"), "regex toggle")
        assert user.SendMessageW(user.GetDlgItem(bar, 1053), 0xF0, 0, 0) == 1, "Regex toggle is not checked"
        enter(query, "[")
        expect("Invalid regex")
        assert text(user.GetDlgItem(bar, 1047)).startswith("Invalid regex:"), "Missing inline regex error"
        assert user.SendMessageW(results, 0x1004, 0, 0) == 0, "Invalid regex kept old results"
        assert not user.IsWindowEnabled(user.GetDlgItem(bar, 1042)), "Invalid regex left Next enabled"
        enter(query, r"warranty(?=\s)")
        wait_for(lambda: text(count).endswith(" of 6"), "regex error recovery")
        toggle(1053)
        expect("0 matches")
        enter(query, "warranty")
        wait_for(lambda: text(count).endswith(" of 6"), "literal mode restored")

        user.SendMessageW(tabs, 0x1330, 1, 0)
        scope = user.GetDlgItem(bar, 1052)
        assert user.SendMessageW(scope, 0x146, 0, 0) == 2, "Missing inline scopes"
        prior_count = text(count)
        user.SendMessageW(scope, 0x14F, 1, 0)  # Open, highlight Current page, then cancel.
        user.SendMessageW(scope, 0x100, 0x28, 0)
        assert text(count) == prior_count, "Highlighting a scope applied an unconfirmed choice"
        user.SendMessageW(scope, 0x100, 27, 0)
        assert user.SendMessageW(scope, 0x147, 0, 0) == 0, "Escape did not restore the scope"
        assert text(count) == prior_count, "Canceling the scope changed the search"
        user.SendMessageW(scope, 0x14F, 1, 0)
        user.SendMessageW(scope, 0x100, 0x28, 0)
        user.SendMessageW(scope, 0x100, 13, 0)
        expect("1 of 2")
        assert visible(bookmarks), "Changing scope replaced Bookmarks"
        assert text(page) == "2", "Current page scope changed pages"
        user.SendMessageW(scope, 0x100, 0x26, 0)  # Closed-combo arrow keys apply immediately.
        wait_for(lambda: text(count).endswith(" of 6"), "entire-document scope restored")
        user.SendMessageW(scope, 0x100, 0x28, 0)
        expect("1 of 2")
        user.SendMessageW(query, 0x100, 13, 0)
        expect("1 of 2")
        assert visible(results), "Enter did not open scoped Results"
        enter(query, "not in this document")
        expect("0 matches")
        type_query("warranty")
        expect("1 of 2")
        user.SendMessageW(query, 0x100, 13, 0)
        expect("1 of 2")
        user.SendMessageW(query, 0x100, 13, 0)
        expect("2 of 2")
        user.SendMessageW(query, 0x100, 27, 0)
        assert not visible(bar), "Escape did not close search"
        assert visible(bookmarks), "Closing Find did not restore bookmarks"
        assert user.SendMessageW(tabs, 0x1304, 0, 0) == 2, "Closing Find kept the Results tab"
        user.SendMessageW(window, 0x111, 11, 0)
        expect("1 of 2")
        assert visible(bookmarks), "Reopening Find replaced bookmarks"
        query_heights = {}
        for dpi, window_width in ((96, 1000), (96, 640), (144, 1000), (144, 640), (192, 640), (96, 640)):
            bounds = w.RECT(50, 50, 50 + window_width * dpi // 96, 50 + 700 * dpi // 96)
            user.SendMessageW(window, 0x2E0, dpi | (dpi << 16), c.addressof(bounds))
            user.SendMessageW(tabs, 0x1330, 2, 0)
            _, row = result_row(results, 0)
            assert row[3] == 72 * dpi // 96, f"Result row height does not follow DPI: {row[3]} at {dpi}"
            assert not (user.GetWindowLongW(results, -16) & 0x100000), "Results have a horizontal scrollbar"
            user.SendMessageW(tabs, 0x1330, 1, 0)
            assert visible(bookmarks), "DPI check did not restore bookmarks"
            expect("1 of 2")
            controls = [query, scope] + [user.GetDlgItem(bar, i) for i in (1050, 1051, 1053)] + [count] + [user.GetDlgItem(bar, i) for i in (1041, 1042, 1045)]
            boxes = [rect(control) for control in controls]
            assert all(box.left < box.right and box.top < box.bottom for box in boxes)
            assert all(left.right <= right.left or right.right <= left.left or left.bottom <= right.top or right.bottom <= left.top
                       for i, left in enumerate(boxes) for right in boxes[i + 1:]), "Search controls overlap"
            assert boxes[-1].right <= rect(bar).right, "Search controls exceed the bar"
            height = boxes[0].bottom - boxes[0].top
            assert height >= 16 * dpi // 96, f"Search field is too short: {height} at {dpi} DPI"
            assert height == query_heights.setdefault(dpi, height), "Returning to a DPI changed the field height"
            assert boxes[-1].bottom <= rect(bar).bottom, "Search buttons exceed the bar"
            assert boxes[0].right < boxes[-1].left, "Query does not leave room for Close"
            assert abs(boxes[0].top + boxes[0].bottom - boxes[-1].top - boxes[-1].bottom) <= 2, "Close is not centered beside the query"
            assert rect(summary).bottom <= rect(results).top, "Results summary overlaps matches"
            assert rect(tabs).bottom <= rect(bookmarks).top, "Sidebar tabs overlap content"
            assert rect(tabs).bottom - rect(tabs).top >= 32 * dpi // 96, "Tabs do not follow DPI"
            width = rect(bookmarks).right - rect(bookmarks).left
            point = w.POINT(rect(bookmarks).right + 3 * dpi // 96, rect(bookmarks).top + 20 * dpi // 96)
            assert user.ScreenToClient(window, c.byref(point))
            user.SendMessageW(window, 0x201, 1, (point.y << 16) | point.x)
            user.SendMessageW(window, 0x200, 1, point.y << 16)
            user.SendMessageW(window, 0x202, 0, point.y << 16)
            assert user.SendMessageW(tabs, 0x132C, 0, 0) > 1, f"Narrow sidebar tabs did not wrap at {dpi} DPI: width {rect(bookmarks).right - rect(bookmarks).left}, before {width}"
            assert rect(tabs).bottom <= rect(bookmarks).top, "Wrapped tabs overlap content"
        assert query_heights[96] < query_heights[144] < query_heights[192], \
            f"Search field height did not scale with DPI: {query_heights}"
        print("Search navigation, sidebar tabs, bookmark state, options, DPI, and reopen passed")
    finally:
        if window:
            user.PostMessageW(window, 0x10, 0, 0)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
