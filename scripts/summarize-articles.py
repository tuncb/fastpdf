"""Write coverage, timing, and image comparison tables for a corpus audit."""
import argparse
import csv
import json
import statistics
from pathlib import Path


def read_json(path, default=None):
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else default


def timing_summary(values):
    values = sorted(values)
    if not values:
        return {"count": 0}

    def percentile(fraction):
        position = (len(values) - 1) * fraction
        low = int(position)
        high = min(low + 1, len(values) - 1)
        return round(values[low] + (values[high] - values[low]) * (position - low), 3)

    return {"count": len(values), "mean_ms": round(statistics.fmean(values), 3),
            "median_ms": percentile(.5), "p95_ms": percentile(.95), "p99_ms": percentile(.99),
            "max_ms": values[-1], "over_500_ms": sum(value > 500 for value in values),
            "total_seconds": round(sum(values) / 1000, 3)}


def write_csv(path, rows, fields):
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True, help="Compact baseline JSON")
    parser.add_argument("--final", type=Path, required=True, help="Final audit folder")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    inventory = read_json(args.inventory)["files"]
    baseline = read_json(args.baseline)
    before = {(row["id"], row["page"]): row for row in baseline["pages"]}
    files, pages, coverage_errors = [], [], []
    for item in inventory:
        folder = args.final / item["id"]
        result = read_json(folder / "render.json", {})
        comparisons = read_json(folder / "comparison.json", [])
        samples = result.get("samples", [])
        by_page = {row.get("page"): row for row in comparisons}
        expected = set(range(1, item["pages"] + 1))
        actual = [row["page"] for row in samples]
        if set(actual) != expected or len(actual) != len(expected):
            coverage_errors.append({"id": item["id"], "stage": "render", "expected": len(expected),
                                    "actual": len(actual), "missing": sorted(expected - set(actual))})
        if set(by_page) != expected or len(comparisons) != len(expected):
            coverage_errors.append({"id": item["id"], "stage": "compare", "expected": len(expected),
                                    "actual": len(comparisons), "missing": sorted(expected - set(by_page))})
        if result.get("engine_pages") != item["pages"]:
            coverage_errors.append({"id": item["id"], "stage": "page_count",
                                    "expected": item["pages"], "actual": result.get("engine_pages")})
        for sample in samples:
            old = before.get((item["id"], sample["page"]), {})
            comparison = by_page.get(sample["page"], {})
            pages.append({"id": item["id"], "file": item["relative_path"], "page": sample["page"],
                          "baseline_ms": old.get("ms"), "final_ms": sample.get("ms"),
                          "baseline_error": old.get("error", ""), "error": sample.get("error", ""),
                          "comparison_error": comparison.get("error", ""),
                          **{key: value for key, value in comparison.items() if key not in ("page", "error")}})
        times = [row["ms"] for row in samples if "ms" in row]
        first_page = next((row.get("ms") for row in samples if row["page"] == 1), None)
        first_ready = result["open_ms"] + first_page if result.get("open_ms") is not None and first_page is not None else None
        files.append({"id": item["id"], "file": item["relative_path"], "pages": item["pages"],
                      "open_ms": result.get("open_ms"), "max_render_ms": max(times, default=None),
                      "load_and_first_page_ms": round(first_ready, 3) if first_ready is not None else None,
                      "slow_pages": sum(value > 500 for value in times),
                      "render_errors": sum("error" in row for row in samples) + int("error" in result),
                      "comparison_flags": sum(row.get("flag", False) for row in comparisons),
                      "comparison_errors": sum("error" in row for row in comparisons),
                      "encrypted": result.get("encrypted"), "repaired": result.get("repaired"),
                      "warnings": " | ".join(result.get("warnings", []))})
    summary = {"expected_files": len(inventory), "expected_pages": sum(row["pages"] for row in inventory),
               "source_bytes": sum(row["bytes"] for row in inventory),
               "rendered_page_records": len(pages), "coverage_errors": coverage_errors,
               "baseline_open": timing_summary([row["open_ms"] for row in baseline["files"] if "open_ms" in row]),
               "final_open": timing_summary([row["open_ms"] for row in files if row["open_ms"] is not None]),
               "load_and_first_page": timing_summary([row["load_and_first_page_ms"] for row in files if row["load_and_first_page_ms"] is not None]),
               "baseline_render": timing_summary([row["ms"] for row in baseline["pages"] if "ms" in row]),
               "final_render": timing_summary([row["final_ms"] for row in pages if row["final_ms"] is not None]),
               "baseline_render_errors": sum("error" in row for row in baseline["pages"]),
               "final_render_errors": sum(row["render_errors"] for row in files),
               "comparison_flags": sum(row["comparison_flags"] for row in files),
               "comparison_errors": sum(row["comparison_errors"] for row in files),
               "files_with_slow_pages": sum(row["slow_pages"] > 0 for row in files),
               "run": read_json(args.final / "run.json")}
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    file_fields = ["id", "file", "pages", "open_ms", "load_and_first_page_ms", "max_render_ms", "slow_pages", "render_errors",
                   "comparison_flags", "comparison_errors", "encrypted", "repaired", "warnings"]
    page_fields = ["id", "file", "page", "baseline_ms", "final_ms", "baseline_error", "error",
                   "comparison_error", "flag", "missing_ink", "extra_ink", "blurred_mae", "bad_area",
                   "pixel_sha256"]
    write_csv(args.output / "files.csv", files, file_fields)
    write_csv(args.output / "pages.csv", pages, page_fields)
    write_csv(args.output / "slow-opens.csv", sorted((row for row in files if (row["open_ms"] or 0) > 500),
              key=lambda row: row["open_ms"], reverse=True), file_fields)
    write_csv(args.output / "slow-pages.csv", sorted((row for row in pages if (row["final_ms"] or 0) > 500),
              key=lambda row: row["final_ms"], reverse=True), page_fields)
    checks = {(row["id"], row["page"]): "render" for row in pages if (row["final_ms"] or 0) > 500}
    for row in files:
        if (row["open_ms"] or 0) > 500:
            key = row["id"], 1
            checks[key] = "open and render" if key in checks else "open"
        elif (row["load_and_first_page_ms"] or 0) > 500 and (row["id"], 1) not in checks:
            checks[(row["id"], 1)] = "load and first page"
    write_csv(args.output / "timing-check-cases.csv",
              [{"id": key[0], "page": key[1], "reason": value} for key, value in sorted(checks.items())],
              ["id", "page", "reason"])
    write_csv(args.output / "flagged-pages.csv", [row for row in pages if row.get("flag")], page_fields)
    (args.output / "flagged-pages.json").write_text(json.dumps([row for row in pages if row.get("flag")],
               indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
