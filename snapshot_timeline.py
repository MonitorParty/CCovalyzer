import os
import json
import subprocess
import csv
import re
import tempfile

# Path to your instrumented binary (compiled with -fprofile-instr-generate -fcoverage-mapping)
BINARY_PATH = "/data/playground/5CC/hb-shape-fuzzer"

SNAPSHOT_PREFIX = "snap_"
OUTPUT_FILENAME = "coverage_report.csv"

# Helper: run command and return stdout or raise on error

def run_cmd(cmd):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"Command failed: {' '.join(cmd)}\nSTDERR:\n{result.stderr}")
    return result.stdout

# Extract coverage percentages from `llvm-cov export -summary-only` JSON

def parse_export_json(s):
    data = json.loads(s)
    if not data.get("data"):
        return None
    totals = data["data"][0].get("totals", {})
    linecov = (totals.get("lines") or {}).get("percent")
    funccov = (totals.get("functions") or {}).get("percent")
    # Branch totals may not be present depending on compiler/flags
    branchcov = (totals.get("branches") or {}).get("percent")
    return {
        "linecov": linecov,
        "branchcov": branchcov,
        "funccov": funccov,
    }

# Process ONE .profraw file by merging ONLY that file (merge flag required), then exporting totals

def coverage_from_profraw(profraw_path):
    # Make a temporary .profdata for this single profraw
    with tempfile.NamedTemporaryFile(suffix=".profdata", delete=False) as tmp:
        profdata_path = tmp.name
    try:
        # IMPORTANT: merge ONLY this single profraw
        run_cmd(["llvm-profdata", "merge", "--sparse", profraw_path, "-o", profdata_path])
        export_json = run_cmd([
            "llvm-cov", "export", BINARY_PATH, f"-instr-profile={profdata_path}", "--summary-only"
        ])
        cov = parse_export_json(export_json)
        return cov
    finally:
        # Clean up the temporary profdata
        try:
            os.remove(profdata_path)
        except OSError:
            pass

# Write one CSV per experiment, appending rows as we evaluate each profraw in order of snapshots

def process_experiment(exp_path):
    snapshots_path = os.path.join(exp_path, "snapshots")
    if not os.path.isdir(snapshots_path):
        return

    # Prepare output CSV for this experiment
    output_csv = os.path.join(exp_path, OUTPUT_FILENAME)
    with open(output_csv, "w", newline="") as csvfile:
        fieldnames = ["number", "linecov", "branchcov", "funccov"]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        # Enumerate snapshots: snapshot_0, snapshot_1, ... numerically
        def snap_key(name):
            try:
                return int(name.replace(SNAPSHOT_PREFIX, ""))
            except Exception:
                return float("inf")

        for snap in sorted(
            [d for d in os.listdir(snapshots_path) if d.startswith(SNAPSHOT_PREFIX)], key=snap_key
        ):
            snap_num = snap_key(snap)
            if snap_num == float("inf"):
                continue
            snap_dir = os.path.join(snapshots_path, snap)
            if not os.path.isdir(snap_dir):
                continue

            # For THIS snapshot: evaluate EACH .profraw INDIVIDUALLY (no multi-file merge!)
            profraws = sorted(
                [os.path.join(snap_dir, f) for f in os.listdir(snap_dir) if f.endswith(".profraw")]
            )
            if not profraws:
                continue

            for profraw in profraws:
                cov = coverage_from_profraw(profraw)
                if not cov:
                    continue
                row = {
                    "number": snap_num,
                    "linecov": cov.get("linecov"),
                    "branchcov": cov.get("branchcov"),
                    "funccov": cov.get("funccov"),
                }
                writer.writerow(row)


    print(f"Written report for {exp_path} -> {output_csv}")


def main():
    # Run from main directory: scan for subdirs containing a 'snapshots' folder
    cwd = os.getcwd()
    for entry in sorted(os.listdir(cwd)):
        exp_dir = os.path.join(cwd, entry)
        if not os.path.isdir(exp_dir):
            continue
        if os.path.isdir(os.path.join(exp_dir, "snapshots")):
            process_experiment(exp_dir)


if __name__ == "__main__":
    main()

