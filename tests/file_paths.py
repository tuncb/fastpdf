"""Check Unicode and long paths through file I/O and the real Open dialog."""
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import subprocess
import sys
import winreg

from fixtures import base, pdf
from reader_restore import blue_bounds
from ui_toolbar import CALLBACK, find_window, text, user, wait_for


def extended(path):
    value = str(path)
    return path if value.startswith('\\\\?\\') else Path('\\\\?\\' + value)


def long_paths_enabled():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                       r'SYSTEM\CurrentControlSet\Control\FileSystem') as key:
        try:
            return winreg.QueryValueEx(key, 'LongPathsEnabled')[0] == 1
        except FileNotFoundError:
            return False


def probe(executable, document, image, output, cwd=None):
    image.unlink(missing_ok=True)
    output.unlink(missing_ok=True)
    result = subprocess.run([str(executable), str(document), '--text',
                             '--render', str(image), '--extract', str(output)],
                            capture_output=True, timeout=20, cwd=cwd)
    assert result.returncode == 0, result.stderr
    info = json.loads(result.stdout)
    assert info['pages'] == 1 and 'Path audit' in info['text'], info
    assert output.read_text(encoding='utf8') == info['text']
    blue_bounds(image)


def reader(executable, document, image):
    image.unlink(missing_ok=True)
    subprocess.run([str(executable), str(document), '--screenshot', str(image)],
                   check=True, timeout=20)
    blue_bounds(image)


user.GetDlgCtrlID.argtypes = [w.HWND]
user.EnumChildWindows.argtypes = [w.HWND, CALLBACK, w.LPARAM]
user.IsWindow.argtypes = [w.HWND]
user.IsWindowVisible.argtypes = [w.HWND]


def windows(pid, parent=None):
    found = []

    @CALLBACK
    def visit(hwnd, _):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            name = c.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, name, len(name))
            found.append((hwnd, name.value, user.GetDlgCtrlID(hwnd)))
        return True

    if parent:
        user.EnumChildWindows(parent, visit, 0)
    else:
        user.EnumWindows(visit, 0)
    return found


def open_dialog(pid, window):
    user.PostMessageW(window, 0x111, 1, 0)  # File -> Open
    dialog = wait_for(lambda: next((hwnd for hwnd, cls, _ in windows(pid)
                                   if cls == '#32770'), None), 'Open dialog')
    edit = wait_for(lambda: next((hwnd for hwnd, cls, control in windows(pid, dialog)
                                 if cls == 'Edit' and control in (1148, 1152, 1001)
                                 and user.IsWindowVisible(hwnd)), None),
                    'File name field')
    return dialog, edit


def dialog_case(executable, document, typed):
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable)], startupinfo=startup)
    try:
        window = wait_for(lambda: find_window(process.pid), 'Reader window')
        toolbar = wait_for(lambda: user.GetDlgItem(window, 202), 'Toolbar')
        page = wait_for(lambda: user.GetDlgItem(toolbar, 203), 'Page field')
        dialog, edit = open_dialog(process.pid, window)
        buffer = c.create_unicode_buffer(typed)
        user.SendMessageW(edit, 0xC, 0, c.addressof(buffer))
        user.PostMessageW(dialog, 0x111, 1, 0)  # Open
        wait_for(lambda: not user.IsWindow(dialog), 'Open dialog closed')
        wait_for(lambda: user.IsWindowEnabled(page) and text(page) == '1', 'PDF loaded')
        assert text(window) == document.name + ' — FastPDF'

        # Cancel must keep the loaded document, even with another path in the field.
        dialog, edit = open_dialog(process.pid, window)
        buffer = c.create_unicode_buffer(str(extended(document.parent / 'missing.pdf')))
        user.SendMessageW(edit, 0xC, 0, c.addressof(buffer))
        user.PostMessageW(dialog, 0x111, 2, 0)  # Cancel
        wait_for(lambda: not user.IsWindow(dialog), 'Cancel')
        assert user.IsWindowEnabled(page) and text(page) == '1'
        assert text(window) == document.name + ' — FastPDF'
    finally:
        if process.poll() is None:
            window = find_window(process.pid)
            if window:
                user.PostMessageW(window, 0x10, 0, 0)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    assert process.returncode == 0, process.returncode


def run(reader_exe, probe_exe, root):
    root.mkdir(parents=True, exist_ok=True)
    name = 'Türkçe_İı_文書_Ж_ا_😀_e\u0301'
    deep = root
    while len(str(deep)) < 1050:
        deep /= 'folder_' + 'x' * 55
    extended(deep).mkdir(parents=True, exist_ok=True)
    short = root / (name + '.pdf')
    long = deep / (name + '.pdf')
    content = pdf(base(content=b'0 0 1 rg 20 20 100 80 re f\n'
                              b'BT /F1 20 Tf 1 0 0 1 20 175 Tm (Path audit) Tj ET'))
    short.write_bytes(content)
    extended(long).write_bytes(content)
    cases = [('unicode', short), ('extended', extended(long))]
    if long_paths_enabled():
        cases.append(('long', long))
    else:
        print('LongPathsEnabled is off; skip unprefixed long paths.')

    for label, document in cases:
        image = document.with_suffix('.png')
        output = document.with_suffix('.txt')
        probe(probe_exe, document, image, output)
        reader(reader_exe, document, root / (label + '-reader.png'))
        # The native Windows picker uses normal paths. The Win32 device prefix is
        # supported by direct file I/O, but the Shell rejects it in the name field.
        if label != 'extended':
            dialog_case(reader_exe, document, str(document))
            print(label + ': PDF, PNG, text, reader, Open, and Cancel passed.', flush=True)
        else:
            print(label + ': PDF, PNG, text, and reader passed.', flush=True)

    # Keep extended paths with spaces covered by direct file I/O.
    spaced = deep / (name + ' with spaces.pdf')
    extended(spaced).write_bytes(content)
    reader(reader_exe, extended(spaced), root / 'extended-spaces-reader.png')
    # The standard picker handles quotes and spaces in a normal file path.
    if not long_paths_enabled():
        spaced = root / (name + ' with spaces.pdf')
        spaced.write_bytes(content)
    dialog_case(reader_exe, spaced, '"' + str(spaced) + '"')

    # Relative paths must continue to resolve against the caller's working directory.
    probe(probe_exe, Path(short.name), root / 'relative.png', root / 'relative.txt', cwd=root)
    print('Extended paths with spaces, quoted picker paths, and relative paths passed.', flush=True)


if __name__ == '__main__':
    run(*(Path(arg).resolve() for arg in sys.argv[1:]))
