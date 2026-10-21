"""Check that a bad page does not hide later pages or discard progress."""
import json
import hashlib
import subprocess
import sys
from pathlib import Path

from fixtures import pdf, stream


def main():
    executable, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    root.mkdir(parents=True, exist_ok=True)
    source = root / "batch.pdf"
    source.write_bytes(pdf([
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 /MediaBox [0 0 100 100] >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 6 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 7 0 R >>",
        b"<< /Type /Page /Parent 2 0 R /Contents 8 0 R >>",
        stream(b"1 0 0 rg 10 10 50 50 re f"),
        stream(b"not-deflate", b"/Filter /FlateDecode"),
        stream(b"0 0 1 rg 20 20 40 40 re f"),
    ]))
    progress = root / "progress.jsonl"
    result = subprocess.run([str(executable), str(source), "--all", "--timings",
                             "--continue-on-error", "--pixel-hash", "--progress", str(progress),
                             "--render-dir", str(root / "pages")],
                            capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert result.returncode == 2, result.stdout + result.stderr
    summary = json.loads(result.stdout)
    assert summary["renders"] == 2 and summary["failed_pages"] == 1, summary
    assert [s["page"] for s in summary["samples"]] == [1, 2, 3], summary
    assert "error" in summary["samples"][1], summary
    records = [json.loads(line) for line in progress.read_text().splitlines()]
    assert records[0]["event"] == "open" and records[0]["pages"] == 3, records
    assert records[1:] == summary["samples"], records
    assert (root / "pages/page-000001.png").exists()
    assert (root / "pages/page-000003.png").exists()
    selected = subprocess.run([str(executable), str(source), "--page", "3", "--last-page", "3", "--timings"],
                              capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert selected.returncode == 0, selected.stdout + selected.stderr
    assert [s["page"] for s in json.loads(selected.stdout)["samples"]] == [3]
    invalid = subprocess.run([str(executable), str(source), "--page", "3", "--last-page", "2"],
                             capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert invalid.returncode == 2
    bitmap = subprocess.run([str(executable), str(source), "--page", "3", "--render-dir",
                             str(root / "bmp"), "--render-format", "bmp"],
                            capture_output=True, text=True, encoding="utf-8", timeout=20)
    assert bitmap.returncode == 0, bitmap.stdout + bitmap.stderr
    data = (root / "bmp/page-000003.bmp").read_bytes()
    assert data[:2] == b"BM" and len(data) == 54 + 100 * 100 * 4
    assert data[54 + (60 * 100 + 30) * 4:54 + (60 * 100 + 30) * 4 + 4] == bytes([255, 0, 0, 255])
    assert summary["samples"][2]["pixel_sha256"] == hashlib.sha256(data[54:]).hexdigest()
    print("Batch page output, progress, error recovery and page ranges passed")


if __name__ == "__main__":
    main()
