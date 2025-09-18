import subprocess
import pandas as pd
import seaborn as sns
import numpy as np
from matplotlib import rcParams
from pathlib import Path

# ------------------------------
# Plot styling (from your script)
# ------------------------------
rcParams.update({
    "text.usetex": True,
    "text.latex.preamble": r"\usepackage[cochineal]{newtxmath}",
    "font.size": 18,
})

# ------------------------------
# Helper: run llvm-cov
# ------------------------------
def get_coverage_value(profraw: Path, cov_type: str = "branches") -> int:
    """Run llvm-cov report and return the total value for the given coverage type."""
    cmd = [
        "llvm-cov", "report", "../hb-shape-fuzzer",
        "-instr-profile", str(profraw),
    ]
    try:
        output = subprocess.check_output(cmd, text=True)
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"llvm-cov failed for {profraw}: {e}")

    for line in output.splitlines():
        if line.strip().startswith("TOTAL"):
            # Example TOTAL line:
            # TOTAL: 12345  678  54.9%  2345  100  95.7% ...
            parts = line.split()
            # Map coverage types to column indices
            mapping = {
                "regions-missed": 2,
                "regions": 3,
                "functions-missed": 5,
                "functions": 6,
                "lines-missed": 8,
                "lines": 9,
                "branches-missed": 11,
                "branches": 12,
            }
            if cov_type not in mapping:
                raise ValueError(f"Unsupported coverage type {cov_type}")
            return int(parts[mapping[cov_type]])
    raise ValueError(f"TOTAL line not found in report for {profraw}")
# ------------------------------
# Collect data
# ------------------------------
def collect_experiments(experiments, folder: Path, cov_type: str):
    records = []
    for exp in experiments:
        profraw = folder / f"merge-{exp}.profdata"
        if not profraw.exists():
            raise FileNotFoundError(f"Missing {profraw}")
        value = get_coverage_value(profraw, cov_type)
        records.append({"experiment": exp, cov_type: value})
    return pd.DataFrame(records)

# ------------------------------
# Plot
# ------------------------------
def plot_coverage(df: pd.DataFrame, cov_type: str, order, colors, output="figure.pdf"):
    ax = sns.barplot(
        data=df,
        x="experiment",
        y=cov_type,
        order=order,
        palette=colors,
        errorbar=None
    )
    ax.set_ylabel(f"{cov_type.capitalize()} covered")
    ax.set_xlabel("Experiment")
    ax.figure.tight_layout()
    ax.figure.savefig(output)
    print(f"Saved {output}")

# ------------------------------
# Example usage
# ------------------------------
if __name__ == "__main__":
    experiments = ["s-sf-a", "s-sf-q", "s-sl-a", "s-sl-q", "s-fb-a", "s-fb-q", "i-sf-a", "i-sf-q", "i-fb-a", "i-fb-q" ]
    cov_type = "branches-missed"  # could also be "functions", "lines", "regions"
    folder = Path("./")

    df = collect_experiments(experiments, folder, cov_type)

    order = experiments  # control order in plot
    colors = {exp: col for exp, col in zip(order, ["#4b858e", "#80bc00","#4b858e", "#80bc00","#4b858e", "#80bc00", "#36595f", "#6a9c00", "#36595f", "#6a9c00"])}

    plot_coverage(df, cov_type, order, colors, output="figure.pdf")

