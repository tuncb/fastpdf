"""Check the real print preview without sending a document to a printer."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import struct
import subprocess
import sys

from fixtures import pdf, stream
from ui_toolbar import user, CALLBACK, wait_for, find_window, text, enter, rect

user.EnumChildWindows.argtypes = [w.HWND, CALLBACK, w.LPARAM]
user.GetDlgCtrlID.argtypes = [w.HWND]
user.GetParent.argtypes = [w.HWND]
user.GetParent.restype = w.HWND
user.IsWindow.argtypes = [w.HWND]
user.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
gdi = c.WinDLL("gdi32", use_last_error=True)
gdi.CreateCompatibleDC.argtypes = [w.HDC]
gdi.CreateCompatibleDC.restype = w.HDC
gdi.CreateDIBSection.argtypes = [w.HDC, c.c_void_p, w.UINT, c.POINTER(c.c_void_p), w.HANDLE, w.DWORD]
gdi.CreateDIBSection.restype = w.HBITMAP
gdi.SelectObject.argtypes = [w.HDC, w.HANDLE]
gdi.SelectObject.restype = w.HANDLE
gdi.DeleteObject.argtypes = [w.HANDLE]
gdi.DeleteDC.argtypes = [w.HDC]


def capture(window, path=None):
    # Owner-drawn static controls do not implement PrintWindow. Capture their parent.
    if user.GetDlgCtrlID(window) == 1020:
        parent = user.GetParent(window)
        parent_width, _, parent_pixels = capture(parent)
        bounds, parent_bounds = rect(window), rect(parent)
        width, height = bounds.right - bounds.left, bounds.bottom - bounds.top
        x, y = bounds.left - parent_bounds.left, bounds.top - parent_bounds.top
        pixels = b"".join(parent_pixels[((y + row) * parent_width + x) * 4:
                                       ((y + row) * parent_width + x + width) * 4]
                          for row in range(height))
        if path:
            header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, len(pixels), 0, 0, 0, 0)
            path.write_bytes(struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54) + header + pixels)
        return width, height, pixels
    bounds = w.RECT()
    user.GetClientRect(window, c.byref(bounds))
    width, height = bounds.right, bounds.bottom
    header = struct.pack("<IiiHHIIiiII", 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
    info = c.create_string_buffer(header + bytes(4))
    dc = gdi.CreateCompatibleDC(None)
    bits = c.c_void_p()
    bitmap = gdi.CreateDIBSection(dc, info, 0, c.byref(bits), None, 0)
    assert bitmap and dc
    old = gdi.SelectObject(dc, bitmap)
    try:
        assert user.PrintWindow(window, dc, 1), "Could not capture preview"
        gdi.GdiFlush()
        pixels = c.string_at(bits, width * height * 4)
        if path:
            path.write_bytes(struct.pack("<2sIHHI", b"BM", 54 + len(pixels), 0, 0, 54) + header + pixels)
        return width, height, pixels
    finally:
        gdi.SelectObject(dc, old)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(dc)


def find_preview(window):
    result = []

    @CALLBACK
    def visit(child, _):
        if user.GetDlgCtrlID(child) == 1020:
            # EnumChildWindows can see a template halfway through its creation.
            # Wait until the dialog has created its controls and initialized its page.
            pane = user.GetParent(child)
            caption = user.GetDlgItem(pane, 1023)
            if caption and text(caption) and all(user.GetDlgItem(pane, control) for control in (1021, 1022)):
                result.append(child)
        return True

    # The modal print dialog is an owned top-level window, not a reader child.
    @CALLBACK
    def top(hwnd, _):
        if user.GetParent(hwnd) == window:
            user.EnumChildWindows(hwnd, visit, 0)
        return True

    user.EnumWindows(top, 0)
    return result[0] if result else None


def check_pixels(image, color, portrait, output):
    def rendered():
        width, height, pixels = capture(image)
        # Each fixture has a large primary-color rectangle on white paper.
        count = sum(pixels[i:i + 3] == color for i in range(0, len(pixels), 4))
        return (width, height, pixels) if count > width * height // 20 else None

    width, _, pixels = wait_for(rendered, "rendered preview pixels")
    white = [i // 4 for i in range(0, len(pixels), 4) if pixels[i:i + 3] == b"\xff\xff\xff"]
    assert white, "Preview has no paper"
    paper_width = max(p % width for p in white) - min(p % width for p in white)
    paper_height = max(p // width for p in white) - min(p // width for p in white)
    assert (paper_height > paper_width) == portrait, "Incorrect PDF page rotation"
    capture(image, output)


def click_preview(button):
    # BM_CLICK can fail for an inactive dialog. Send the same BN_CLICKED notification
    # without taking desktop focus from the user.
    assert user.IsWindowEnabled(button), "Preview action is disabled"
    user.SendMessageW(user.GetParent(button), 0x111, user.GetDlgCtrlID(button), button)


def run(executable, output):
    spool = c.WinDLL("winspool.drv", use_last_error=True)
    length = w.DWORD()
    spool.GetDefaultPrinterW(None, c.byref(length))
    if not length.value:
        print("SKIP: print preview requires a default Windows printer")
        return 77
    output.mkdir(parents=True, exist_ok=True)
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               b"<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R] /Count 3 >>"]
    for index, color in enumerate(("1 0 0", "0 1 0", "0 0 1")):
        rotation = 90 if index == 1 else 0
        objects.extend([
            f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Rotate {rotation} "
            f"/Resources << >> /Contents {4 + index * 2} 0 R >>".encode(),
            stream(f"{color} rg 20 20 260 160 re f"),
        ])
    document = output / "preview.pdf"
    document.write_bytes(pdf(objects))
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable), str(document)], startupinfo=startup)
    try:
        window = wait_for(lambda: find_window(process.pid), "reader")
        toolbar = wait_for(lambda: user.GetDlgItem(window, 202), "toolbar")
        page = wait_for(lambda: user.GetDlgItem(toolbar, 203), "page field")
        wait_for(lambda: user.IsWindowEnabled(page) and text(page) == "1", "PDF opened")
        enter(page, "2")
        user.SendMessageW(window, 0x111, 8, 0)  # Rotate the reader view only.
        for iteration in range(2):
            user.PostMessageW(window, 0x111, 13, 0)
            image = wait_for(lambda: find_preview(window), "print preview pane")
            pane = user.GetParent(image)
            dialog = user.GetParent(pane)
            previous, next_button = (user.GetDlgItem(pane, control) for control in (1021, 1022))
            caption = user.GetDlgItem(pane, 1023)
            capture(dialog, output / "print-dialog-opening.bmp")
            wait_for(lambda: text(caption) == "Page 2 of 3", "current page preview")
            title_bounds = rect(user.GetDlgItem(pane, 1024))
            image_bounds, navigation_bounds = rect(image), rect(previous)
            assert title_bounds.bottom < image_bounds.top, "Preview covers its title"
            assert image_bounds.bottom < navigation_bounds.top, "Preview covers navigation"
            check_pixels(image, b"\x00\xff\x00", True, output / f"page-2-{iteration}.bmp")
            for target, button, color, portrait in (
                (3, next_button, b"\xff\x00\x00", False),
                (2, previous, b"\x00\xff\x00", True),
                (1, previous, b"\x00\x00\xff", False),
            ):
                click_preview(button)
                assert text(caption) == f"Page {target} of 3"
                check_pixels(image, color, portrait, output / f"page-{target}.bmp")
                assert bool(user.IsWindowEnabled(previous)) == (target != 1)
                assert bool(user.IsWindowEnabled(next_button)) == (target != 3)
                assert text(page) == "2", "Preview changed the reader page"
                assert text(user.GetDlgItem(dialog, 0x480)) == "2", "Preview changed the print range"
                assert text(user.GetDlgItem(dialog, 0x481)) == "2", "Preview changed the print range"
            # Rapid requests must discard stale results and retain the last page.
            for target in (next_button, next_button, previous, previous, next_button):
                click_preview(target)
            check_pixels(image, b"\x00\xff\x00", True, output / "rapid-navigation.bmp")
            capture(dialog, output / "print-dialog.bmp")
            # Cancel while another preview may still be rendering, then reopen.
            click_preview(next_button)
            user.PostMessageW(dialog, 0x111, 2, 0)
            wait_for(lambda: not user.IsWindow(dialog), "Cancel closes print dialog")
            assert text(page) == "2"
        user.SendMessageW(window, 0x111, 3, 0)
        assert text(page) == "3", "Reader did not recover after Cancel"
        user.PostMessageW(window, 0x10, 0, 0)
        assert process.wait(timeout=10) == 0, "Reader did not close cleanly"
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
    print("Print preview: rendered pixels, rotation, navigation, stale requests, Cancel, and reopen passed")
    return 0


if __name__ == "__main__":
    sys.exit(run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()))
