"""Render a local PDF corpus and compare every page with a development reference.

Requires Pillow, NumPy, SciPy and PyMuPDF. These are test tools only.
Timing runs are serial. PNG writing and reference comparison are outside the
engine timers. The file cache is not flushed; open_ms is not cold-disk latency.
"""
import argparse
import concurrent.futures
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import maximum_filter, uniform_filter


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def read_progress(path):
    records = []
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass  # A killed process can leave one partial record.
    return records


def compare_page(actual, reference):
    a = np.asarray(actual.convert("RGB"), dtype=np.float32)
    b = np.asarray(reference.convert("RGB"), dtype=np.float32)
    result = {"actual_size": list(actual.size), "reference_size": list(reference.size)}
    if abs(a.shape[0] - b.shape[0]) > 1 or abs(a.shape[1] - b.shape[1]) > 1:
        result.update(flag=True, reason="page_size")
        return result
    height, width = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
    a, b = a[:height, :width], b[:height, :width]
    # Ink matching tolerates a two-pixel rasterizer displacement. A separate
    # blurred RGB measure retains broad color/geometry differences.
    ia, ib = np.min(a, axis=2) < 210, np.min(b, axis=2) < 210
    near_a = maximum_filter(ia, size=5)
    near_b = maximum_filter(ib, size=5)
    missing = float(np.count_nonzero(ib & ~near_a) / max(1, np.count_nonzero(ib)))
    extra = float(np.count_nonzero(ia & ~near_b) / max(1, np.count_nonzero(ia)))
    aa = uniform_filter(a, size=(5, 5, 1))[::4, ::4]
    bb = uniform_filter(b, size=(5, 5, 1))[::4, ::4]
    delta = np.abs(aa - bb)
    mae = float(delta.mean())
    bad_area = float(np.mean(np.max(delta, axis=2) > 40))
    result.update(missing_ink=round(missing, 6), extra_ink=round(extra, 6),
                  blurred_mae=round(mae, 4), bad_area=round(bad_area, 6),
                  actual_ink=int(ia.sum()), reference_ink=int(ib.sum()),
                  flag=bool(missing > 0.08 or extra > 0.10 or mae > 7 or bad_area > 0.025))
    return result


def render_file(args, item, folder):
    folder.mkdir(parents=True, exist_ok=True)
    result = {"id": item["id"], "path": item["path"], "pages": item.get("pages", 0),
              "samples": [], "attempts": [], "warnings": []}
    first, last = 1, item.get("pages", 1)
    while first <= last:
        progress = folder / f"progress-{first:06d}.jsonl"
        command = [str(args.exe.resolve()), item["path"], "--page", str(first),
                   "--last-page", str(last), "--scale", str(args.scale), "--timings",
                   "--continue-on-error", "--progress", str(progress),
                   "--render-dir", str(folder / "pages"), "--render-format", "bmp"]
        if args.profile:
            command.append("--profile")
        begin = time.perf_counter()
        stalled = False
        with (folder / f"stdout-{first:06d}.json").open("w", encoding="utf-8") as out, \
             (folder / f"stderr-{first:06d}.txt").open("w", encoding="utf-8") as err:
            process = subprocess.Popen(command, stdout=out, stderr=err)
            last_change, previous_size = time.monotonic(), -1
            while process.poll() is None:
                size = progress.stat().st_size if progress.exists() else 0
                if size != previous_size:
                    previous_size, last_change = size, time.monotonic()
                if time.monotonic() - last_change > args.timeout:
                    stalled = True
                    process.kill()
                    process.wait()
                    break
                time.sleep(0.2)
        records = read_progress(progress)
        opening = next((r for r in records if r.get("event") == "open"), None)
        samples = [r for r in records if "page" in r]
        result["samples"].extend(samples)
        result["warnings"] = sorted(set(result["warnings"] + [w for s in samples for w in s.get("new_warnings", [])]))
        if opening and "open_ms" not in result:
            result["open_ms"] = opening["open_ms"]
            result["engine_pages"] = opening["pages"]
        attempt = {"first": first, "returncode": process.returncode, "stalled": stalled,
                   "wall_seconds": round(time.perf_counter() - begin, 3)}
        result["attempts"].append(attempt)
        try:
            summary = json.loads((folder / f"stdout-{first:06d}.json").read_text(encoding="utf-8"))
            result["warnings"] = sorted(set(result["warnings"] + summary.get("warnings", [])))
            result["private_bytes"] = summary.get("private_bytes")
            result["encrypted"] = summary.get("encrypted", False)
            result["repaired"] = summary.get("repaired", False)
        except (OSError, json.JSONDecodeError):
            pass
        if not opening:
            result["error"] = (folder / f"stderr-{first:06d}.txt").read_text(encoding="utf-8") or "Open timeout or crash"
            break
        completed = max((r["page"] for r in samples), default=first - 1)
        if completed >= last:
            break
        failed = completed + 1
        result["samples"].append({"page": failed, "error": "Page timeout" if stalled else "Process stopped", "timeout_seconds": args.timeout})
        first = failed + 1
    write_json(folder / "render.json", result)
    return result


def compare_file(args, item, folder, pymupdf):
    results = []
    pymupdf.TOOLS.mupdf_display_errors(False)
    pymupdf.TOOLS.mupdf_display_warnings(False)
    try:
        with pymupdf.open(item["path"]) as document:
            for index in range(len(document)):
                row = {"page": index + 1}
                actual_path = folder / "pages" / f"page-{index + 1:06d}.png"
                if not actual_path.exists():
                    actual_path = actual_path.with_suffix(".bmp")
                try:
                    if not actual_path.exists():
                        raise RuntimeError("FastPDF image is missing")
                    reference_path = folder / "reference" / actual_path.with_suffix(".png").name
                    cached_path = reference_path
                    if not cached_path.exists() and args.reuse_reference:
                        cached_path = args.reuse_reference / item["id"] / "reference" / reference_path.name
                    if cached_path.exists():
                        with Image.open(cached_path) as cached:
                            reference = cached.convert("RGB")
                    else:
                        pixmap = document[index].get_pixmap(matrix=pymupdf.Matrix(args.scale, args.scale), colorspace=pymupdf.csRGB, alpha=False)
                        reference = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
                    with Image.open(actual_path) as actual:
                        row.update(compare_page(actual, reference))
                        # Store compact evidence after the measured process exits.
                        # WIC's PNG encoder otherwise dominates corpus wall time.
                        if actual_path.suffix == ".bmp":
                            actual.save(actual_path.with_suffix(".png"), compress_level=1)
                    if actual_path.suffix == ".bmp":
                        actual_path.unlink()
                    if (row["flag"] or args.cache_reference) and not reference_path.exists():
                        reference_path.parent.mkdir(exist_ok=True)
                        reference.save(reference_path, compress_level=1)
                except Exception as error:
                    row.update(error=str(error), flag=True)
                results.append(row)
    except Exception as error:
        results.append({"error": str(error), "flag": True})
    write_json(folder / "comparison.json", results)
    return results


def compare_task(args, item):
    if args.reference_path:
        sys.path.insert(0, str(args.reference_path.resolve()))
    import pymupdf
    folder = args.output / item["id"]
    if args.resume and (folder / "comparison.json").exists():
        comparisons = json.loads((folder / "comparison.json").read_text(encoding="utf-8"))
    else:
        comparisons = compare_file(args, item, folder, pymupdf)
    return {"event": "compared", "id": item["id"], "pages": len(comparisons),
            "flagged_pages": sum(r["flag"] for r in comparisons),
            "errors": sum("error" in r for r in comparisons)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference-path", type=Path)
    parser.add_argument("--scale", type=float, default=96 / 72)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--ids", help="Comma-separated inventory IDs")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--skip-reference", action="store_true")
    parser.add_argument("--compare-only", action="store_true")
    parser.add_argument("--workers", type=int, default=4, help="Reference workers; timing is always serial")
    parser.add_argument("--cache-reference", action="store_true", help="Keep every reference image for later checks")
    parser.add_argument("--reuse-reference", type=Path, help="Reuse reference images from a matching prior run")
    args = parser.parse_args()
    if args.reference_path:
        sys.path.insert(0, str(args.reference_path.resolve()))
    import pymupdf
    inventory = json.loads(args.inventory.read_text(encoding="utf-8"))
    if args.reuse_reference:
        previous = json.loads((args.reuse_reference / "run.json").read_text(encoding="utf-8"))
        if previous["scale"] != args.scale or previous["reference"] != pymupdf.VersionBind:
            raise SystemExit("Reference cache scale or renderer version does not match")
    selected = set(args.ids.split(",")) if args.ids else None
    files = [r for r in inventory["files"] if not selected or r["id"] in selected]
    args.output.mkdir(parents=True, exist_ok=True)
    metadata_path = args.output / "run.json"
    identity = {"exe_sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                "inventory_sha256": hashlib.sha256(args.inventory.read_bytes()).hexdigest(),
                "scale": args.scale, "reference": pymupdf.VersionBind, "profile": args.profile}
    previous = json.loads(metadata_path.read_text(encoding="utf-8")) if metadata_path.exists() else None
    if previous:
        for key, value in identity.items():
            if key in previous and previous[key] != value:
                raise SystemExit(f"Existing run does not match {key}; use a new output folder")
        if not args.resume and not args.compare_only:
            raise SystemExit("Output already contains a run; use --resume or a new output folder")
    if args.compare_only:
        if not previous:
            raise SystemExit("No timing run metadata exists in the output folder")
        with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(compare_task, args, item) for item in files]
            for future in concurrent.futures.as_completed(futures):
                print(json.dumps(future.result()), flush=True)
        previous["comparison_end_time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        write_json(metadata_path, previous)
        return
    metadata = previous or {**identity, "executable": str(args.exe.resolve()),
               "inventory": str(args.inventory.resolve()), "files": len(files),
               "selected_ids": [item["id"] for item in files],
               "start_time": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "threshold_ms": 500,
               "timeout_seconds": args.timeout}
    write_json(metadata_path, metadata)
    for index, item in enumerate(files, 1):
        folder = args.output / item["id"]
        if args.resume and (folder / "render.json").exists():
            result = json.loads((folder / "render.json").read_text(encoding="utf-8"))
        else:
            print(json.dumps({"event": "start", "index": index, "id": item["id"], "file": item["path"], "pages": item.get("pages")}), flush=True)
            result = render_file(args, item, folder)
        comparisons = []
        if not args.skip_reference:
            if args.resume and (folder / "comparison.json").exists():
                comparisons = json.loads((folder / "comparison.json").read_text(encoding="utf-8"))
            else:
                comparisons = compare_file(args, item, folder, pymupdf)
        print(json.dumps({"event": "done", "index": index, "id": item["id"], "pages": len(result["samples"]),
              "open_ms": result.get("open_ms"), "slow_pages": sum(r.get("ms", 0) > 500 for r in result["samples"]),
              "errors": sum("error" in r for r in result["samples"]) + int("error" in result),
              "warnings": result["warnings"], "flagged_pages": sum(r["flag"] for r in comparisons)}), flush=True)
    metadata["render_end_time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    write_json(metadata_path, metadata)


if __name__ == "__main__":
    main()
