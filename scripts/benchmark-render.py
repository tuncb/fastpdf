"""Repeatable fresh-process and warm-image rendering measurements (stdlib only)."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdf", type=Path)
    parser.add_argument("--exe", type=Path, default=Path("build/pdfprobe.exe"))
    parser.add_argument("--pages", default="1", help="Comma-separated one-based pages")
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--runs", type=int, default=3, help="Fresh processes per page")
    parser.add_argument("--repeats", type=int, default=3, help="Renders per process, including the first")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--full-resolution", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        pages = [int(value) for value in args.pages.split(",")]
    except ValueError:
        parser.error("--pages must contain comma-separated integers")
    if not pages or min(pages) < 1 or args.runs < 1 or args.repeats < 1 or args.scale <= 0:
        parser.error("Pages, runs, repeats and scale must be positive")
    report = {"pdf": str(args.pdf.resolve()), "pdf_bytes": args.pdf.stat().st_size,
              "executable": str(args.exe.resolve()), "scale": args.scale,
              "runs_per_page": args.runs, "repeats": args.repeats,
              "full_resolution": args.full_resolution, "measurements": [], "summary": []}
    for page in pages:
        first, warm, opening = [], [], []
        for run in range(args.runs):
            command = [str(args.exe.resolve()), str(args.pdf.resolve()), "--page", str(page),
                       "--scale", str(args.scale), "--benchmark", str(args.repeats), "--timings"]
            if args.profile:
                command.append("--profile")
            if args.full_resolution:
                command.append("--full-resolution")
            started = time.perf_counter()
            process = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=300)
            if process.returncode:
                raise RuntimeError(f"Page {page}: {process.stderr or process.stdout}")
            result = json.loads(process.stdout)
            report["measurements"].append({"page": page, "run": run + 1,
                                           "process_ms": (time.perf_counter() - started) * 1000,
                                           "result": result})
            first.append(result["samples"][0]["ms"])
            warm.extend(sample["ms"] for sample in result["samples"][1:])
            opening.append(result["open_ms"])
        row = {"page": page, "first_ms": statistics.median(first),
               "warm_ms": statistics.median(warm) if warm else None,
               "open_ms": statistics.median(opening)}
        report["summary"].append(row)
        print(json.dumps(row), flush=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
