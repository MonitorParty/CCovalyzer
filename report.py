import subprocess
import sys

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <profraw> <binary>")
    sys.exit(1)

profraw = sys.argv[1]
binary = sys.argv[2]

# Merge raw profile to .profdata
profdata = "merged.profdata"
subprocess.run([
    "llvm-profdata", "merge", "--sparse", profraw, "-o", profdata
], check=True)

# Get coverage summary
result = subprocess.run([
    "llvm-cov", "report", binary, f"--instr-profile={profdata}"
], capture_output=True, text=True, check=True)

# Parse summary line
lines = result.stdout.strip().split("\n")
summary = lines[-1]
print("Coverage summary line:", summary)

# Try to extract the % covered
try:
    coverage_percent = float(summary.split()[3].strip("%"))
    print(f"Coverage: {coverage_percent:.2f}%")
except Exception:
    print("Could not parse coverage percent.")

