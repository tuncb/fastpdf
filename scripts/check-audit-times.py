"""Repeat slow corpus pages in fresh processes and collect diagnostic profiles."""
import argparse
import collections
import csv
import hashlib
import json
import statistics
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--pages", type=Path, required=True, help="CSV with id and page columns")
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--scale", type=float, default=96 / 72)
    args = parser.parse_args()
    assert args.repeats > 0
    inventory = {row["id"]: row for row in json.loads(args.inventory.read_text(encoding="utf-8"))["files"]}
    with args.pages.open(encoding="utf-8-sig", newline="") as stream:
        cases = list(csv.DictReader(stream))
    identity = {"exe_sha256": hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                "inventory_sha256": hashlib.sha256(args.inventory.read_bytes()).hexdigest(),
                "cases_sha256": hashlib.sha256(args.pages.read_bytes()).hexdigest(),
                "scale": args.scale, "repeats": args.repeats}
    output = {"identity": identity, "results": []}
    if args.output.exists():
        output = json.loads(args.output.read_text(encoding="utf-8"))
        if output["identity"] != identity:
            raise SystemExit("Existing timing checks use different inputs")
    completed = {(row["id"], row["page"]) for row in output["results"]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for case in cases:
        key = case["id"], int(case["page"])
        if key in completed:
            continue
        item = inventory[key[0]]
        command = [str(args.exe.resolve()), item["path"], "--page", str(key[1]),
                   "--scale", str(args.scale), "--timings"]
        row = {"id": key[0], "page": key[1], "reason": case.get("reason", "render"),
               "times_ms": [], "open_ms": []}
        try:
            for _ in range(args.repeats):
                result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=180)
                if result.returncode:
                    raise RuntimeError(result.stderr or result.stdout)
                result = json.loads(result.stdout)
                row["times_ms"].append(result["samples"][0]["ms"])
                row["open_ms"].append(result["open_ms"])
            row["median_ms"] = statistics.median(row["times_ms"])
            row["min_ms"] = min(row["times_ms"])
            row["max_ms"] = max(row["times_ms"])
            row["open_median_ms"] = statistics.median(row["open_ms"])
            row["load_and_page_ms"] = [round(open_ms + render_ms, 3) for open_ms, render_ms in zip(row["open_ms"], row["times_ms"])]
            if row["max_ms"] > 500:
                result = subprocess.run(command + ["--profile"], capture_output=True, text=True,
                                        encoding="utf-8", timeout=180)
                if result.returncode:
                    raise RuntimeError(result.stderr or result.stdout)
                result = json.loads(result.stdout)
                row["profile_open_ms"] = result["open_ms"]
                row["profile_render_ms"] = result["render_ms"]
                profile = result["samples"][0]["profile"]
                images = profile.pop("images", [])
                row["diagnostic_profile"] = profile
                row["image_filters"] = dict(collections.Counter(image["filter"] for image in images))
                row["warnings"] = result.get("warnings", [])
        except Exception as error:
            row["error"] = str(error)
        output["results"].append(row)
        temporary = args.output.with_suffix(".tmp")
        temporary.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
        temporary.replace(args.output)
        completed.add(key)
        print(json.dumps({key: value for key, value in row.items() if key != "diagnostic_profile"}), flush=True)


if __name__ == "__main__":
    main()
