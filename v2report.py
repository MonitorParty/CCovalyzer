import subprocess
import sys
import re

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <profraw> <binary>")
    sys.exit(1)

profraw = sys.argv[1]
binary = sys.argv[2]

profdata = "merged.profdata"
subprocess.run([
    "llvm-profdata", "merge", "--sparse", profraw, "-o", profdata
], check=True)

# Run llvm-cov report
result = subprocess.run([
    "llvm-cov", "report", binary, f"--instr-profile={profdata}"
], capture_output=True, text=True, check=True)

lines = result.stdout.strip().split("\n")

# Find header & total line
header_line = None
total_line = None
for i, line in enumerate(lines):
    if line.strip().startswith("TOTAL"):
        total_line = line
        if i > 0:
            header_line = lines[i - 1]
        break

if not header_line or not total_line:
    print("ERROR: Could not parse llvm-cov output")
    sys.exit(1)

# Extract columns
headers = header_line.split()
values = total_line.split()

# First column is "TOTAL", skip it for values
metric_names = headers[1:]
metric_values = values[1:]

# Some columns are "count" and "percent" pairs — keep only % ones
print("=== TOTAL COVERAGE ===")
for name, val in zip(metric_names, metric_values):
    if val.endswith("%"):
        print(f"{name} Coverage: {val}")

