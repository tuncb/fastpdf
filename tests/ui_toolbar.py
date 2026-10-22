"""Exercise the real Win32 toolbar in a hidden reader window (no desktop input)."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import time

from fixtures import base, pdf

user = c.WinDLL("user32", use_last_error=True)
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = c.c_ssize_t
user.GetDlgItem.argtypes = [w.HWND, c.c_int]
user.GetDlgItem.restype = w.HWND
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.SetWindowTextW.argtypes = [w.HWND, w.LPCWSTR]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetDpiForWindow.argtypes = [w.HWND]
user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user.MoveWindow.argtypes = [w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.BOOL]
user.GetWindowLongW.argtypes = [w.HWND, c.c_int]
user.IsWindowEnabled.argtypes = [w.HWND]
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
CALLBACK = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = [CALLBACK, w.LPARAM]
user.SetProcessDpiAwarenessContext.argtypes = [c.c_void_p]
user.SetProcessDpiAwarenessContext(c.c_void_p(-4))

WM_COMMAND, WM_KEYDOWN, WM_CLOSE = 0x111, 0x100, 0x10
TB_ISBUTTONENABLED, TB_ISBUTTONCHECKED = 0x400 + 9, 0x400 + 10
TB_ISBUTTONHIDDEN = 0x400 + 12


class GUIThreadInfo(c.Structure):
    _fields_ = [("size", w.DWORD), ("flags", w.DWORD), ("active", w.HWND),
                ("focus", w.HWND), ("capture", w.HWND), ("menu", w.HWND),
                ("move", w.HWND), ("caret", w.HWND), ("caret_rect", w.RECT)]


user.GetGUIThreadInfo.argtypes = [w.DWORD, c.POINTER(GUIThreadInfo)]


def focused(window):
    info = GUIThreadInfo()
    info.size = c.sizeof(info)
    assert user.GetGUIThreadInfo(user.GetWindowThreadProcessId(window, None), c.byref(info))
    return info.focus


def wait_for(predicate, description, timeout=10):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(0.02)
    raise AssertionError(f"Timed out: {description}")


def find_window(pid):
    result = []

    @CALLBACK
    def visit(hwnd, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            name = c.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, name, len(name))
            if name.value == "FastPDF.Window":
                result.append(hwnd)
        return True

    user.EnumWindows(visit, 0)
    return result[0] if result else None


def text(hwnd):
    value = c.create_unicode_buffer(128)
    user.SendMessageW(hwnd, 0xD, len(value), c.addressof(value))
    return value.value


def enter(hwnd, value):
    buffer = c.create_unicode_buffer(value)
    user.SendMessageW(hwnd, 0xC, 0, c.addressof(buffer))
    assert text(hwnd) == value, "Could not set entry text"
    user.SendMessageW(hwnd, WM_KEYDOWN, 13, 0)


def rect(hwnd):
    value = w.RECT()
    assert user.GetWindowRect(hwnd, c.byref(value))
    return value


def check_bookmarks(window, toolbar, page):
    tree = user.GetDlgItem(window, 205)
    assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0), "Bookmarks did not open by default"
    next_item = lambda item, relation: user.SendMessageW(tree, 0x110A, relation, item)
    roots = []
    item = next_item(0, 0)  # TVGN_ROOT
    while item:
        roots.append(item)
        item = next_item(item, 1)  # TVGN_NEXT
    assert len(roots) == 5, "Bookmark hierarchy was flattened"
    child = next_item(roots[1], 4)  # TVGN_CHILD
    assert child, "Child bookmark is missing"

    def expanded(item):
        return bool(user.SendMessageW(tree, 0x1127, item, 0x20) & 0x20)  # TVM_GETITEMSTATE

    def selection(expected):
        assert next_item(0, 9) == expected, "Wrong selected bookmark"

    def click_row(index, x=60):
        dpi = user.GetDpiForWindow(window)
        point = (((index * 32 + 16) * dpi // 96) << 16) | (x * dpi // 96)
        user.SendMessageW(tree, 0x201, 1, point)
        user.SendMessageW(tree, 0x202, 0, point)

    assert not expanded(roots[1]), "Bookmarks must start collapsed"
    click_row(1, 10)  # Expand with the disclosure button without changing page.
    assert expanded(roots[1]) and text(page) == "1"
    click_row(2)
    assert text(page) == "2"
    selection(child)
    assert focused(window) == tree, "Bookmark click lost tree focus"
    for index in (0, 3, 1, 2, 0):
        user.SendMessageW(tree, 0x200, 0, ((index * 32 + 16) << 16) | 60)
        user.SendMessageW(tree, 0xF, 0, 0)
        selection(child)

    user.SendMessageW(window, WM_COMMAND, 3, 0)
    selection(roots[2])
    user.SendMessageW(window, WM_COMMAND, 2, 0)
    selection(roots[1])
    user.SendMessageW(tree, WM_KEYDOWN, 0x28, 0)
    selection(child)  # A child on the same page must remain selectable.
    user.SendMessageW(tree, WM_KEYDOWN, 0x28, 0)
    selection(roots[2])
    assert text(page) == "3"
    user.SendMessageW(window, WM_COMMAND, 3, 0)
    assert text(page) == "4"
    selection(roots[2])
    enter(page, "1")
    selection(0)

    click_row(1)
    selection(roots[1])
    user.SendMessageW(tree, WM_KEYDOWN, 0x25, 0)  # Left collapses a branch.
    wait_for(lambda: not expanded(roots[1]), "Left collapses the branch")
    user.SendMessageW(tree, WM_KEYDOWN, 0x27, 0)  # Right expands it.
    wait_for(lambda: expanded(roots[1]), "Right expands the branch")
    user.SendMessageW(window, WM_COMMAND, 9, 0)
    user.SendMessageW(window, WM_COMMAND, 10, 0)
    assert expanded(roots[1]), "Changing pane mode lost expansion state"
    click_row(1, 10)
    assert not expanded(roots[1])
    enter(page, "3")
    enter(page, "2")
    assert not expanded(roots[1]), "Page navigation opened a collapsed branch"
    user.SendMessageW(tree, WM_KEYDOWN, 27, 0)
    assert focused(window) == window
    enter(page, "1")


def check_sidebar_resize(window, toolbar):
    dpi = user.GetDpiForWindow(window)
    scale = lambda value: (value * dpi + 48) // 96
    width = lambda control: rect(control).right - rect(control).left
    tree, pages = user.GetDlgItem(window, 205), user.GetDlgItem(window, 200)
    assert width(pages) == scale(320) - scale(6), "Default pane width is not doubled"
    y = scale(120)

    def drag(start, end):
        user.SendMessageW(window, 0x201, 1, (y << 16) | start)
        user.SendMessageW(window, 0x200, 1, (y << 16) | (end & 0xFFFF))
        user.SendMessageW(window, 0x202, 0, (y << 16) | (end & 0xFFFF))

    drag(scale(317), scale(457))
    assert abs(width(pages) - scale(454)) <= 1, "Page pane did not resize"
    user.SendMessageW(window, WM_COMMAND, 10, 0)
    assert width(tree) == width(pages), "Pane modes do not share their width"
    drag(scale(457), scale(397))
    assert abs(width(tree) - scale(394)) <= 1, "Bookmark pane did not resize"
    drag(scale(397), 0)
    assert width(tree) == scale(160) - scale(6), "Minimum pane width was not enforced"
    drag(scale(157), scale(2000))
    bounds = w.RECT()
    user.GetClientRect(window, c.byref(bounds))
    assert bounds.right - width(tree) - scale(6) >= scale(240) - 1, "Resize hid the document"
    drag(width(tree) + scale(3), scale(317))
    assert abs(width(tree) - (scale(320) - scale(6))) <= 1
    user.SendMessageW(window, WM_COMMAND, 9, 0)
    user.SendMessageW(window, WM_COMMAND, 9, 0)
    user.SendMessageW(window, WM_COMMAND, 9, 0)
    assert width(pages) == width(tree), "Hiding the pane lost its width"


def check_bookmark_titles(executable, output):
    # The same titles must appear whether they are stored in the outline item
    # or in a separate PDF string object (as in the Mothes thesis).
    document = output / "bookmark-titles.pdf"
    titles = [b"(Introduction)",
              b"<" + (b"\xfe\xff" + "Spline theory \u03a9".encode("utf-16-be")).hex().encode() + b">",
              b"(Summary)"]
    captures = []
    for indirect in (False, True):
        objects = base(extra_catalog=b"/Outlines 6 0 R")
        objects.extend([
            b"<< /Type /Outlines /First 7 0 R /Last 9 0 R /Count 3 >>",
            b"<< /Parent 6 0 R /Next 9 0 R /First 8 0 R /Last 8 0 R /Count 1 /Dest [3 0 R /Fit] /Title "
            + (b"10 0 R" if indirect else titles[0]) + b" >>",
            b"<< /Parent 7 0 R /Dest [3 0 R /Fit] /Title "
            + (b"11 0 R" if indirect else titles[1]) + b" >>",
            b"<< /Parent 6 0 R /Prev 7 0 R /Dest [3 0 R /Fit] /Title "
            + (b"12 0 R" if indirect else titles[2]) + b" >>",
            *titles,
        ])
        document.write_bytes(pdf(objects))
        capture = output / f"bookmark-titles-{'indirect' if indirect else 'direct'}.png"
        subprocess.run([str(executable), str(document), "--screenshot", str(capture)],
                       check=True, timeout=20)
        captures.append(capture.read_bytes())
    assert captures[0] == captures[1], "Indirect bookmark titles differ from direct titles"


def run(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    objects = base()
    objects[1] = b"<< /Type /Pages /Kids [3 0 R 6 0 R 7 0 R] /Count 3 >>"
    objects.extend([objects[2], objects[2]])
    sample = output / "toolbar.pdf"
    sample.write_bytes(pdf(objects))
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R >>"
    objects[1] = b"<< /Type /Pages /Kids [3 0 R 6 0 R 7 0 R 15 0 R] /Count 4 >>"
    objects.extend([
        b"<< /Type /Outlines /First 9 0 R /Last 14 0 R /Count 6 >>",
        b"<< /Title (Heading without a destination) /Parent 8 0 R /Next 10 0 R >>",
        b"<< /Title (Section) /Parent 8 0 R /Prev 9 0 R /Next 12 0 R /First 11 0 R /Last 11 0 R /Count 1 /Dest [6 0 R /Fit] >>",
        b"<< /Title (Child) /Parent 10 0 R /Dest [6 0 R /Fit] >>",
        b"<< /Title (Last section) /Parent 8 0 R /Prev 10 0 R /Next 13 0 R /Dest [7 0 R /Fit] >>",
        b"<< /Title (Earlier section) /Parent 8 0 R /Prev 12 0 R /Next 14 0 R /Dest [6 0 R /Fit] >>",
        b"<< /Title (Heading without a destination) /Parent 8 0 R /Prev 13 0 R >>",
        objects[2],
    ])
    bookmarked = output / "bookmarks.pdf"
    bookmarked.write_bytes(pdf(objects))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    for document in (None, sample, bookmarked):
        loaded = document is not None
        process = subprocess.Popen([str(executable)] + ([str(document)] if loaded else []), startupinfo=startup)
        try:
            window = wait_for(lambda: find_window(process.pid), "reader window")
            toolbar = wait_for(lambda: user.GetDlgItem(window, 202), "native toolbar")
            page = wait_for(lambda: user.GetDlgItem(toolbar, 203), "page field")
            zoom = wait_for(lambda: user.GetDlgItem(toolbar, 204), "zoom field")
            if loaded:
                wait_for(lambda: user.IsWindowEnabled(page) and text(page) == "1", "PDF opened")
                if document == bookmarked:
                    check_bookmarks(window, toolbar, page)
            else:
                wait_for(lambda: text(page) == "—", "empty state")
            assert bool(user.SendMessageW(toolbar, TB_ISBUTTONENABLED, 1, 0))
            assert not user.SendMessageW(toolbar, TB_ISBUTTONENABLED, 2, 0), "Previous enabled on first page"
            assert bool(user.SendMessageW(toolbar, TB_ISBUTTONENABLED, 3, 0)) == loaded
            assert bool(user.IsWindowEnabled(zoom)) == loaded
            bounds = rect(toolbar)
            dpi = user.GetDpiForWindow(window)
            assert bounds.bottom - bounds.top == (36 * dpi + 48) // 96
            if not loaded:
                # Maintenance stays available without a PDF; Continue portable hides both welcome actions.
                install = user.GetDlgItem(window, 17)
                portable = user.GetDlgItem(window, 20)
                assert install and portable, "Native installation controls are missing"
                assert user.GetWindowLongW(install, -16) & 0x10000000, "Install choice is hidden"
                assert user.GetWindowLongW(portable, -16) & 0x10000000, "Portable choice is hidden"
                user.SendMessageW(portable, 0xF5, 0, 0)  # BM_CLICK
                assert not user.GetWindowLongW(install, -16) & 0x10000000
                assert not user.GetWindowLongW(portable, -16) & 0x10000000
                for cmd in (*range(2, 12), 13):
                    assert not user.SendMessageW(toolbar, TB_ISBUTTONENABLED, cmd, 0)
                continue

            # Native navigation, direct page entry, bounds, and invalid-input rejection.
            last_page = "4" if document == bookmarked else "3"
            user.SendMessageW(window, WM_COMMAND, 3, 0)
            wait_for(lambda: text(page) == "2", "next page")
            enter(page, last_page)
            wait_for(lambda: text(page) == last_page, "direct page entry")
            assert not user.SendMessageW(toolbar, TB_ISBUTTONENABLED, 3, 0)
            enter(page, "99")
            assert text(page) == "99", "Invalid page unexpectedly accepted"
            user.SendMessageW(page, WM_KEYDOWN, 27, 0)
            assert text(page) == last_page, "Escape did not restore the page"

            enter(zoom, "125%")
            wait_for(lambda: text(zoom) == "125%", "zoom entry")
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 6, 0), "Fit remains selected after custom zoom"
            for invalid in ("0", "801%", "nan", "120junk"):
                enter(zoom, invalid)
                assert text(zoom) == invalid
                user.SendMessageW(zoom, WM_KEYDOWN, 27, 0)
                assert text(zoom) == "125%", "Invalid zoom changed the document"
            user.SendMessageW(window, WM_COMMAND, 5, 0)
            wait_for(lambda: text(zoom) == "150%", "zoom in")
            user.SendMessageW(window, WM_COMMAND, 6, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 6, 0)
            user.SendMessageW(window, WM_COMMAND, 7, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 7, 0)
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 6, 0)

            # Switching from bookmarks to pages shows pages instead of hiding the sidebar.
            if not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0):
                user.SendMessageW(window, WM_COMMAND, 10, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0)
            user.SendMessageW(window, WM_COMMAND, 9, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 9, 0)
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0)

            check_sidebar_resize(window, toolbar)

            # Selecting a page must keep focus in the list for repeated arrow navigation.
            sidebar = user.GetDlgItem(window, 200)
            point = (10 << 16) | 20
            user.SendMessageW(sidebar, 0x201, 1, point)  # WM_LBUTTONDOWN
            user.SendMessageW(sidebar, 0x202, 0, point)  # WM_LBUTTONUP
            assert focused(window) == sidebar, "Page selection stole sidebar focus"
            for expected in ("2", "3"):
                user.SendMessageW(sidebar, WM_KEYDOWN, 0x28, 0)
                assert text(page) == expected, "Sidebar arrow navigation failed"
                assert focused(window) == sidebar, "Arrow navigation stole sidebar focus"
            user.SendMessageW(sidebar, WM_KEYDOWN, 27, 0)
            assert focused(window) == window, "Escape did not return to the document"

            user.SendMessageW(window, WM_COMMAND, 9, 0)
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 9, 0)
            user.SendMessageW(window, WM_COMMAND, 11, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 11, 0)
            user.SendMessageW(window, WM_COMMAND, 11, 0)
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 11, 0)

            # All toolbar fields stay inside the client area at minimum size and below.
            for width in (640, 480, 1280):
                user.MoveWindow(window, 0, 0, width * dpi // 96, 600 * dpi // 96, True)
                bounds = rect(toolbar)
                for field in (page, zoom):
                    field_rect = rect(field)
                    assert bounds.left <= field_rect.left < field_rect.right <= bounds.right
                    assert bounds.top <= field_rect.top < field_rect.bottom <= bounds.bottom
            assert not user.SendMessageW(toolbar, TB_ISBUTTONHIDDEN, 10, 0), "Wide toolbar failed to restore bookmarks"
        finally:
            hwnd = find_window(process.pid)
            if hwnd:
                user.PostMessageW(hwnd, WM_CLOSE, 0, 0)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise AssertionError("Reader did not close")
        assert process.returncode == 0
    check_bookmark_titles(executable, output)
    print("Toolbar: empty/loaded states, bookmark titles, navigation, input validation, zoom, toggles, and resizing passed")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
