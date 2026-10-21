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


def run(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    objects = base()
    objects[1] = b"<< /Type /Pages /Kids [3 0 R 6 0 R 7 0 R] /Count 3 >>"
    objects.extend([objects[2], objects[2]])
    sample = output / "toolbar.pdf"
    sample.write_bytes(pdf(objects))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    for loaded in (False, True):
        process = subprocess.Popen([str(executable)] + ([str(sample)] if loaded else []), startupinfo=startup)
        try:
            window = wait_for(lambda: find_window(process.pid), "reader window")
            toolbar = wait_for(lambda: user.GetDlgItem(window, 202), "native toolbar")
            page = wait_for(lambda: user.GetDlgItem(toolbar, 203), "page field")
            zoom = wait_for(lambda: user.GetDlgItem(toolbar, 204), "zoom field")
            if loaded:
                wait_for(lambda: user.IsWindowEnabled(page) and text(page) == "1", "PDF opened")
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
                for cmd in (*range(2, 12), 13):
                    assert not user.SendMessageW(toolbar, TB_ISBUTTONENABLED, cmd, 0)
                continue

            # Native navigation, direct page entry, bounds, and invalid-input rejection.
            user.SendMessageW(window, WM_COMMAND, 3, 0)
            wait_for(lambda: text(page) == "2", "next page")
            enter(page, "3")
            wait_for(lambda: text(page) == "3", "direct page entry")
            assert not user.SendMessageW(toolbar, TB_ISBUTTONENABLED, 3, 0)
            enter(page, "99")
            assert text(page) == "99", "Invalid page unexpectedly accepted"
            user.SendMessageW(page, WM_KEYDOWN, 27, 0)
            assert text(page) == "3", "Escape did not restore the page"

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
            user.SendMessageW(window, WM_COMMAND, 10, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0)
            user.SendMessageW(window, WM_COMMAND, 9, 0)
            assert user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 9, 0)
            assert not user.SendMessageW(toolbar, TB_ISBUTTONCHECKED, 10, 0)
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
    print("Toolbar: empty/loaded states, navigation, input validation, zoom, toggles, and resizing passed")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
