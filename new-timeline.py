#!/usr/bin/env python3
import os
import sys
import subprocess
import csv
import json
import tempfile
import argparse
import shutil
import re
from pathlib import Path
from datetime import datetime

PROFDATA_BIN = None
COV_BIN = None

def which_or_die(name):
    path = shutil.which(name)
    if not path:
        sys.exit(f"Error: '{name}' not found in PATH. Please install LLVM tools (llvm-profdata, llvm-cov) or add them to PATH.")
    return path

def parse_snapshot_number_from_name(name):
    # find last integer in the filename
    nums = re.findall(r"(\d+)", name)
    if not nums:
        return None
    return int(nums[-1])

def run(cmd, cwd=None):
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd=cwd)

def parse_export_json(s):
    data = json.loads(s)
    if not data.get("data"):
        return None
    totals = data["data"][0].get("totals", {})
    def perc(k):
        v = (totals.get(k) or {}).get("percent")
        return float(v) if v is not None else None
    return {
        "regioncov": perc("regions"),
        "funccov": perc("functions"),
        "linecov": perc("lines"),
        "branchcov": perc("branches"),
    }

def parse_report_text(text):
    for line in text.splitlines():
        if line.strip().startswith("TOTAL"):
            # regex matches 4 percentages in the TOTAL line: region, func, line, branch
            pcts = re.findall(r"(\d+(?:\.\d+)?)%", line)
            vals = [float(x) for x in pcts]
            regioncov = vals[0] if len(vals) >= 1 else None
            funccov = vals[1] if len(vals) >= 2 else None
            linecov = vals[2] if len(vals) >= 3 else None
            branchcov = vals[3] if len(vals) >= 4 else None
            return {
                "regioncov": regioncov,
                "funccov": funccov,
                "linecov": linecov,
                "branchcov": branchcov,
            }
    return None

def coverage_from_profraw(profraw_path, binary_path, log_fh=None):
    fd, profdata_path = tempfile.mkstemp(suffix='.profdata')
    os.close(fd)
    try:
        merge_cmd = [PROFDATA_BIN, 'merge', '--sparse', profraw_path, '-o', profdata_path]
        if log_fh: log_fh.write(f"{datetime.utcnow().isoformat()} - RUN: {' '.join(merge_cmd)}\n")
        r = run(merge_cmd)
        if r.returncode != 0:
            if log_fh:
                log_fh.write(f"MERGE FAILED (rc={r.returncode})\nSTDOUT:\n{r.stdout}\nSTDERR:\n{r.stderr}\n\n")
            raise RuntimeError(f"llvm-profdata merge failed: {r.stderr.strip()}")

        export_cmd = [COV_BIN, 'export', binary_path, f'--instr-profile={profdata_path}', '--summary-only']
        if log_fh: log_fh.write(f"{datetime.utcnow().isoformat()} - RUN: {' '.join(export_cmd)}\n")
        r = run(export_cmd)
        if r.returncode == 0 and r.stdout.strip():
            try:
                cov = parse_export_json(r.stdout)
                if cov:
                    if log_fh: log_fh.write(f"EXPORT JSON OK: {cov}\n\n")
                    return cov
            except json.JSONDecodeError as e:
                if log_fh:
                    log_fh.write(f"JSON decode error: {e}\nSTDOUT:\n{r.stdout}\nSTDERR:\n{r.stderr}\n\n")

        report_cmd = [COV_BIN, 'report', binary_path, f'--instr-profile={profdata_path}']
        if log_fh: log_fh.write(f"{datetime.utcnow().isoformat()} - RUN: {' '.join(report_cmd)}\n")
        r2 = run(report_cmd)
        if log_fh:
            log_fh.write(f"REPORT rc={r2.returncode}\nSTDOUT:\n{r2.stdout}\nSTDERR:\n{r2.stderr}\n\n")
        if r2.returncode == 0 and r2.stdout.strip():
            cov = parse_report_text(r2.stdout)
            return cov
        return None
    finally:
        try:
            os.remove(profdata_path)
        except OSError:
            pass

def process_experiment(exp_dir, binary_path, verbose=False):
    snapshots_dir = Path(exp_dir) / 'snapshots'
    if not snapshots_dir.is_dir():
        if verbose: print(f"Skipping {exp_dir}, no snapshots/") 
        return
    log_path = Path(exp_dir) / 'coverage_collection.log'
    with open(log_path, 'a') as log_fh:
        log_fh.write(f"\n=== run at {datetime.utcnow().isoformat()} ===\n")
        csv_path = Path(exp_dir) / 'coverage_report.csv'
        new_file = not csv_path.exists()
        with open(csv_path, 'a', newline='') as csvfile:
            fieldnames = ['number', 'profraw', 'regioncov', 'funccov', 'linecov', 'branchcov', 'error']
            writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
            if new_file:
                writer.writeheader()
            profraws = sorted([p for p in snapshots_dir.iterdir() if p.is_file() and p.suffix == '.profraw'], key=lambda p: p.name)
            if verbose: print(f"Found {len(profraws)} profraw files in {snapshots_dir}")
            for prof in profraws:
                snap_num = parse_snapshot_number_from_name(prof.name)
                if snap_num is None:
                    snap_num = -1
                if verbose: print(f"Processing {prof.name} (snapshot {snap_num})...") 
                try:
                    cov = coverage_from_profraw(str(prof), binary_path, log_fh=log_fh)
                except Exception as e:
                    log_fh.write(f"ERROR processing {prof}: {e}\n\n")
                    writer.writerow({'number': snap_num, 'profraw': prof.name, 'regioncov': None, 'funccov': None, 'linecov': None, 'branchcov': None, 'error': str(e)})
                    csvfile.flush()
                    continue
                if not cov:
                    writer.writerow({'number': snap_num, 'profraw': prof.name, 'regioncov': None, 'funccov': None, 'linecov': None, 'branchcov': None, 'error': 'no-coverage-data'})
                else:
                    writer.writerow({
                        'number': snap_num,
                        'profraw': prof.name,
                        'regioncov': cov.get('regioncov'),
                        'funccov': cov.get('funccov'),
                        'linecov': cov.get('linecov'),
                        'branchcov': cov.get('branchcov'),
                        'error': ''})
                csvfile.flush()
    if verbose: print(f"Wrote report for {exp_dir} -> {csv_path} (log -> {log_path})")

def main():
    parser = argparse.ArgumentParser(description='Generate per-experiment coverage CSVs from profraw files.')
    parser.add_argument('-b', '--binary', required=True, help='Path to instrumented binary (e.g. ../hb-shape-fuzzer)')
    parser.add_argument('-d', '--dir', default='.', help='Main directory containing experiment subdirs (default .)')
    parser.add_argument('--profdata-bin', default='llvm-profdata', help='Path to llvm-profdata (or name in PATH)')
    parser.add_argument('--cov-bin', default='llvm-cov', help='Path to llvm-cov (or name in PATH)')
    parser.add_argument('-v', '--verbose', action='store_true')
    args = parser.parse_args()

    global PROFDATA_BIN, COV_BIN
    PROFDATA_BIN = which_or_die(args.profdata_bin)
    COV_BIN = which_or_die(args.cov_bin)

    binary_path = args.binary
    if not Path(binary_path).exists():
        sys.exit(f"Error: binary '{binary_path}' not found. Provide correct --binary path.")

    main_dir = Path(args.dir).resolve()
    if args.verbose: print(f"Scanning {main_dir} for experiments (binary={binary_path})")

    for entry in sorted(main_dir.iterdir(), key=lambda p: p.name):
        if not entry.is_dir():
            continue
        if (entry / 'snapshots').is_dir():
            if args.verbose: print(f"Processing experiment {entry}") 
            process_experiment(entry, binary_path, verbose=args.verbose)

if __name__ == '__main__':
    main()

