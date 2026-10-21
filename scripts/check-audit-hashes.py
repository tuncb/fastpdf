"""Find changed pages between a saved-image audit and a later pixel-hash audit."""
import argparse
import csv
import hashlib
import json
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-identical", action="store_true")
    args = parser.parse_args()
    old_run, new_run = read(args.previous / "run.json"), read(args.current / "run.json")
    for key in ("scale", "inventory_sha256"):
        if key not in old_run or old_run[key] != new_run.get(key):
            raise SystemExit(f"Audit inputs do not match: {key}")
    if hashlib.sha256(args.inventory.read_bytes()).hexdigest() != new_run["inventory_sha256"]:
        raise SystemExit("The selected inventory does not match the audited inventory")
    files = read(args.inventory)["files"]
    changes, matched, missing = [], 0, 0
    for item in files:
        previous = args.previous / item["id"] / "comparison.json"
        current = args.current / item["id"] / "render.json"
        before = {row["page"]: row for row in read(previous) if "page" in row} if previous.exists() else {}
        after = {row["page"]: row for row in read(current)["samples"]} if current.exists() else {}
        for page in range(1, item["pages"] + 1):
            old, new = before.get(page, {}), after.get(page, {})
            old_hash, new_hash = old.get("pixel_sha256"), new.get("pixel_sha256")
            if old_hash and new_hash and old_hash == new_hash and old.get("actual_size") == [new.get("width"), new.get("height")]:
                matched += 1
                continue
            reason = "changed_pixels"
            if not old_hash or not new_hash:
                reason = "missing_hash"
                missing += 1
            changes.append({"id": item["id"], "page": page, "file": item.get("relative_path", item["path"]),
                            "reason": reason, "previous_hash": old_hash, "current_hash": new_hash,
                            "error": new.get("error", old.get("error", ""))})
    summary = {"previous_exe_sha256": old_run["exe_sha256"], "current_exe_sha256": new_run["exe_sha256"],
               "expected_pages": sum(item["pages"] for item in files), "identical_pages": matched,
               "changed_pages": len(changes) - missing, "missing_hashes": missing, "changes": changes}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    with args.output.with_suffix(".csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["id", "page", "file", "reason", "previous_hash", "current_hash", "error"])
        writer.writeheader()
        writer.writerows(changes)
    print(json.dumps({key: value for key, value in summary.items() if key != "changes"}))
    if args.require_identical and changes:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
