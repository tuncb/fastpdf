"""Verify a hash-only audit against saved page comparisons and review changed pixels.

Identical pixels retain the prior reference comparison. Changed pages are rendered
again and compared with MuPDF. Their replay hash must match the measured run.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def reference_tools(args):
    if args.reference_path:
        sys.path.insert(0, str(args.reference_path.resolve()))
    import pymupdf
    module = importlib.util.spec_from_file_location("audit", Path(__file__).with_name("audit-articles.py"))
    audit = importlib.util.module_from_spec(module)
    module.loader.exec_module(audit)
    return pymupdf, audit


def verify_file(args, item, run):
    pymupdf, audit = reference_tools(args)
    inherited, changed, errors = 0, 0, 0
    if hashlib.sha256(Path(item["path"]).read_bytes()).hexdigest() != item["sha256"]:
        raise RuntimeError(f"Source PDF changed: {item['id']}")
    folder = args.current / item["id"]
    before = read(args.previous / item["id"] / "comparison.json")
    samples = sorted(read(folder / "render.json")["samples"], key=lambda row: row["page"])
    expected = set(range(1, item["pages"] + 1))
    if len(before) != len(expected) or {row.get("page") for row in before} != expected:
        raise SystemExit(f"Incomplete prior comparison: {item['id']}")
    if len(samples) != len(expected) or {row.get("page") for row in samples} != expected:
        raise SystemExit(f"Incomplete current render: {item['id']}")
    before = {row["page"]: row for row in before}
    completed = folder / "comparison.json"
    if args.resume and completed.exists():
        saved = read(completed)
        expected_hashes = {row["page"]: row.get("pixel_sha256") for row in samples}
        if len(saved) == len(expected) and {row.get("page") for row in saved} == expected and all(
            row.get("pixel_sha256") == expected_hashes[row["page"]] and "error" not in row and
            row.get("comparison_source") in ("identical_pixels", "rendered_again") for row in saved):
            return {"id": item["id"], "identical_pages": sum(row["comparison_source"] == "identical_pixels" for row in saved),
                    "replayed_pages": sum(row["comparison_source"] == "rendered_again" for row in saved),
                    "errors": 0, "flags": sum(row.get("flag", False) for row in saved)}
    measured = {row["page"]: row for row in samples}
    wanted = [row["page"] for row in samples if row.get("pixel_sha256") != before[row["page"]].get("pixel_sha256") or
              before[row["page"]].get("actual_size") != [row.get("width"), row.get("height")] or
              "error" in before[row["page"]]]
    replayed = {}
    replay_batches = 0
    rows = []
    with pymupdf.open(item["path"]) as document:
        for sample in samples:
            page = sample["page"]
            old = before[page]
            digest = sample.get("pixel_sha256")
            row = {"page": page}
            try:
                if not digest or "error" in sample:
                    raise RuntimeError("The measured page has no valid pixel hash")
                if digest == old.get("pixel_sha256") and old.get("actual_size") == [sample["width"], sample["height"]] and "error" not in old:
                    row = {**old, "comparison_source": "identical_pixels"}
                    inherited += 1
                    # A short replay batch can include unchanged pages between
                    # changed pages. Their hashes were checked with the batch.
                    (folder / "pages" / f"page-{page:06d}.bmp").unlink(missing_ok=True)
                else:
                    image = folder / "pages" / f"page-{page:06d}.png"
                    image.parent.mkdir(exist_ok=True)
                    if page not in replayed:
                        # Bound temporary bitmaps to 32 pages per worker. This
                        # avoids opening a large manual once for every page.
                        last = max(number for number in wanted if page <= number < page + 32)
                        process = subprocess.run([str(args.exe.resolve()), item["path"], "--page", str(page),
                            "--last-page", str(last), "--scale", str(run["scale"]),
                            "--render-dir", str(image.parent), "--render-format", "bmp", "--pixel-hash"],
                            capture_output=True, text=True, encoding="utf-8", timeout=max(180, (last - page + 1) * 20))
                        if process.returncode:
                            raise RuntimeError(process.stderr or process.stdout)
                        batch = json.loads(process.stdout)["samples"]
                        replayed = {entry["page"]: entry for entry in batch}
                        if len(batch) != last - page + 1 or set(replayed) != set(range(page, last + 1)):
                            replayed = {}
                            raise RuntimeError("The replay batch has incomplete page coverage")
                        for number, entry in replayed.items():
                            expected_page = measured[number]
                            if any(entry.get(key) != expected_page.get(key) for key in ("pixel_sha256", "width", "height")):
                                # Keep the evidence before a later batch can
                                # overwrite this bitmap. Do not accept the replay.
                                failures = folder / "replay-failures"
                                failures.mkdir(exist_ok=True)
                                diagnostic = failures / f"batch-{page:06d}-page-{number:06d}-{time.time_ns()}.json"
                                audit.write_json(diagnostic, {
                                    "start": page, "last": last, "page": number,
                                    "actual": entry, "expected": expected_page,
                                    "scale": run["scale"], "exe_sha256": run["exe_sha256"],
                                    "input_sha256": item["sha256"], "command": process.args,
                                })
                                bitmap = image.parent / f"page-{number:06d}.bmp"
                                if bitmap.exists():
                                    shutil.copy2(bitmap, diagnostic.with_suffix(".bmp"))
                                replayed = {}
                                raise RuntimeError(f"The replay pixels differ from the measured run on page {number}")
                        replay_batches += 1
                    reference = None
                    candidates = [args.previous / item["id"] / "reference" / image.name]
                    if args.reuse_reference:
                        candidates.append(args.reuse_reference / item["id"] / "reference" / image.name)
                    for candidate in candidates:
                        if candidate.exists():
                            try:
                                with Image.open(candidate) as cached:
                                    reference = cached.convert("RGB")
                                break
                            except OSError:
                                pass
                    if reference is None:
                        pixmap = document[page - 1].get_pixmap(matrix=pymupdf.Matrix(run["scale"], run["scale"]),
                                                             colorspace=pymupdf.csRGB, alpha=False)
                        reference = Image.frombytes("RGB", (pixmap.width, pixmap.height), pixmap.samples)
                    bitmap = image.with_suffix(".bmp")
                    with Image.open(bitmap) as actual:
                        row.update(audit.compare_page(actual, reference))
                        temporary = image.with_suffix(".png.tmp")
                        actual.save(temporary, format="PNG", compress_level=1)
                        temporary.replace(image)
                    bitmap.unlink()
                    row.update(pixel_sha256=digest, comparison_source="rendered_again")
                    reference_path = folder / "reference" / image.name
                    reference_path.parent.mkdir(exist_ok=True)
                    reference.save(reference_path, compress_level=1)
                    changed += 1
            except Exception as error:
                row.update(error=str(error), flag=True)
                errors += 1
            rows.append(row)
    audit.write_json(folder / "comparison.json", rows)
    return {"id": item["id"], "identical_pages": inherited, "replayed_pages": changed, "errors": errors,
            "replay_batches": replay_batches,
            "flags": sum(row.get("flag", False) for row in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--reference-path", type=Path)
    parser.add_argument("--reuse-reference", type=Path)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    if args.workers < 1:
        raise SystemExit("Worker count must be positive")
    pymupdf, audit = reference_tools(args)
    old_run, run = read(args.previous / "run.json"), read(args.current / "run.json")
    for key in ("scale", "inventory_sha256", "reference"):
        if old_run.get(key) != run.get(key):
            raise SystemExit(f"Audit inputs do not match: {key}")
    if run["reference"] != pymupdf.VersionBind:
        raise SystemExit("Reference renderer version does not match")
    if args.reuse_reference:
        cached_run = read(args.reuse_reference / "run.json")
        if any(cached_run.get(key) != run.get(key) for key in ("scale", "reference")):
            raise SystemExit("Reference cache does not match the current audit")
        if "inventory_sha256" in cached_run:
            if cached_run["inventory_sha256"] != run["inventory_sha256"]:
                raise SystemExit("Reference cache inventory does not match")
        elif Path(cached_run["inventory"]).resolve() != args.inventory.resolve():
            # Early audit metadata stored the inventory path. Each source file
            # is still checked against its recorded SHA-256 before cache use.
            raise SystemExit("Legacy reference cache uses another inventory")
    if hashlib.sha256(args.exe.read_bytes()).hexdigest() != run["exe_sha256"]:
        raise SystemExit("Executable does not match the measured run")
    if hashlib.sha256(args.inventory.read_bytes()).hexdigest() != run["inventory_sha256"]:
        raise SystemExit("Inventory does not match the measured run")
    inherited, changed, errors = 0, 0, 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(verify_file, args, item, run) for item in read(args.inventory)["files"]]
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            inherited += result["identical_pages"]
            changed += result["replayed_pages"]
            errors += result["errors"]
            print(json.dumps(result), flush=True)
    run["comparison_end_time"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    run["comparison_previous_exe_sha256"] = old_run["exe_sha256"]
    run["comparison_identical_pages"] = inherited
    run["comparison_replayed_pages"] = changed
    run["comparison_errors"] = errors
    audit.write_json(args.current / "run.json", run)
    print(json.dumps({"identical_pages": inherited, "replayed_pages": changed, "errors": errors}), flush=True)
    if errors:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
