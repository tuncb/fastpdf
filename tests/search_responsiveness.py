"""Large-result regression using hidden native windows; no desktop input."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import subprocess
import sys
import time

from fixtures import pdf, stream
from search_accessibility import result_row
from ui_toolbar import user, wait_for, find_window, text

user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT,
                                    c.POINTER(c.c_size_t)]
user.SendMessageTimeoutW.restype = c.c_ssize_t


def fixture(path):
    pages = 241
    kids = " ".join(f"{4 + page * 2} 0 R" for page in range(pages))
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               f"<< /Type /Pages /Count {pages} /Kids [{kids}] >>".encode(),
               b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
    for page in range(pages):
        words = 9000 if page == 0 else 320
        content = "BT /F1 10 Tf 1 0 0 1 30 750 Tm (" + "word " * words + ") Tj ET"
        objects += [f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents {5 + page * 2} 0 R >>".encode(), stream(content)]
    path.write_bytes(pdf(objects))


def run(exe, output, worker_test, uia_test):
    output.mkdir(parents=True, exist_ok=True)
    path = output / "many-results.pdf"
    fixture(path)
    subprocess.run([str(worker_test), str(path)], check=True, timeout=60)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(exe), "--restore-view", "0", "0", "2", "1", str(path)], startupinfo=startup)
    window = None
    maximum = 0
    try:
        window = wait_for(lambda: find_window(process.pid), "reader", 30)
        toolbar = wait_for(lambda: user.GetDlgItem(window, 202), "toolbar")
        page = wait_for(lambda: user.GetDlgItem(toolbar, 203), "page field")
        wait_for(lambda: text(page) == "1", "document load", 30)

        def send(control, message, wp=0, lp=0):
            nonlocal maximum
            value = c.c_size_t()
            start = time.perf_counter()
            assert user.SendMessageTimeoutW(control, message, wp, lp, 2, 1000, c.byref(value)), \
                "Search blocked the UI for one second"
            maximum = max(maximum, (time.perf_counter() - start) * 1000)
            return value.value

        send(window, 0x111, 11)
        bar = user.GetDlgItem(window, 206)
        query, count, status = [user.GetDlgItem(bar, item) for item in (201, 1046, 1047)]
        results = user.GetDlgItem(window, 207)

        def read(control):
            value = c.create_unicode_buffer(256)
            send(control, 0xD, len(value), c.addressof(value))
            return value.value

        def search(value):
            buffer = c.create_unicode_buffer(value)
            send(query, 0xC, 0, c.addressof(buffer))
            send(query, 0x100, 13)

        for iteration in range(2):
            search("word")
            wait_for(lambda: read(count) == "1 of 85800", "all large-search results", 30)
            assert send(results, 0x1004) == 85800
            send(results, 0x100, 0x23)  # End
            assert read(count) == "85800 of 85800" and read(page) == "241"
            name, _ = result_row(results, 85799)
            assert name.startswith("Page 241:") and "word" in name
            send(results, 0x100, 0x24)  # Home
            assert read(count) == "1 of 85800" and read(page) == "1"
            user.PostMessageW(window, 0x100, 0x72, 0)
            wait_for(lambda: read(count) == "2 of 85800", "queued F3", 1)
            if iteration == 0:
                log_path = output / "uia.txt"
                with log_path.open("w") as log:
                    client = subprocess.Popen([str(uia_test), str(results), "85800"],
                                              stdout=log, stderr=log)
                    try:
                        def client_ready():
                            report = log_path.read_text()
                            assert client.poll() is None, report
                            return "ready" in report

                        wait_for(client_ready, "UI Automation client", 20)
                        assert read(count) == "85800 of 85800" and read(page) == "241"
                        send(results, 0x100, 0x24)  # Return to page 1 for the next search.
                        send(query, 0xB1, 4, 4)  # Put the caret at the end before typing.
                        start = time.perf_counter()
                        user.PostMessageW(query, 0x102, ord("x"), 0)  # WM_CHAR through the UI queue.
                        wait_for(lambda: read(query) == "wordx", "typing during accessibility reads", 1)
                        elapsed = (time.perf_counter() - start) * 1000
                        wait_for(lambda: read(count) == "0 matches", "live no-match query", 5)
                        client.wait(timeout=10)
                        assert client.returncode == 0, log_path.read_text()
                        print(log_path.read_text().strip())
                        print(f"Queued edit during UI Automation reads: {elapsed:.1f} ms")
                    finally:
                        if client.poll() is None:
                            client.kill()
                            client.wait()
            else:
                search("absent")
            wait_for(lambda: read(count) == "0 matches", "replace a large result set", 30)
            assert send(results, 0x1004) == 0

        search("word")
        user.PostMessageW(bar, 0x111, 1048, 0)
        wait_for(lambda: read(status).startswith("Search stopped"), "Stop", 1)
        stopped = read(count)
        time.sleep(.1)
        assert read(count) == stopped, "Stopped search received stale matches"
        for _ in range(12):
            search("word")
            search("absent")
        wait_for(lambda: read(count) == "0 matches", "latest rapid query", 30)
        assert send(results, 0x1004) == 0
        send(user.GetDlgItem(bar, 1053), 0xF5)  # Regex
        search(r"\w+")
        wait_for(lambda: read(count) == "1 of 85800", "large regex search", 30)
        search("[")
        wait_for(lambda: read(count) == "Invalid regex", "clear results on regex error", 10)
        assert send(results, 0x1004) == 0
        search(r"\w+")
        send(query, 0x100, 27)  # Close Find while a query is active.
        send(window, 0x111, 11)
        wait_for(lambda: read(count) == "1 of 85800", "reopen after cancellation", 30)
        search("word")
        user.PostMessageW(window, 0x10, 0, 0)
        process.wait(timeout=8)
        assert process.returncode == 0
        print(f"85,800 results: navigation, accessibility, cancellation, replacement and close passed; max UI call {maximum:.1f} ms")
    finally:
        if window and process.poll() is None:
            user.PostMessageW(window, 0x10, 0, 0)
        try:
            process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == "__main__":
    run(*(Path(value).resolve() for value in sys.argv[1:5]))
