#!/usr/bin/env python3
"""
run_experiments.py  –  Flow Control ARQ Testing Harness  (Assignment 2)
=======================================================================

Launches receiver + sender as subprocesses for every combination of:
  • Protocol  : Stop-and-Wait (1), Go-Back-N (2), Selective Repeat (3)
  • Window N  : 1, 2, 4, 8  (N=1 is the same for all three at no error)
  • Error prob: 0.0, 0.1, 0.2, 0.3, 0.4, 0.5
  • Loss prob : half of error_prob  (per assignment spec "delay or loss")

For each run it parses the sender's stdout and collects:
  frames_sent, retransmits, total_time, efficiency

Outputs:
  results.csv          – raw data (all runs)
  results.xlsx         – Excel workbook with:
      "Raw Data"       sheet
      "Pivot: Efficiency" sheet
      "Pivot: Time"    sheet
  graphs/              – PNG plots for the report

Usage:
  cd "Assignment 2"
  python3 run_experiments.py          # full run  (~2-3 min)
  python3 run_experiments.py --quick  # quick smoke test (no errors only)
"""

import argparse
import csv
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

# ── optional imports (fail gracefully) ──────────────────────────────
try:
    import pandas as pd
    import matplotlib
    matplotlib.use("Agg")          # no display needed
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
    import openpyxl
    from openpyxl.styles import PatternFill, Font, Alignment, Border, Side
    from openpyxl.utils.dataframe import dataframe_to_rows
    HAS_PANDAS = True
except ImportError:
    HAS_PANDAS = False
    print("[WARN] pandas/matplotlib/openpyxl not found – CSV only mode.")
    print("       Install with:  pip install pandas matplotlib openpyxl")

# ═══════════════════════════════════════════════════════════════════
#  Configuration
# ═══════════════════════════════════════════════════════════════════

A2_DIR     = Path(__file__).parent
SENDER     = A2_DIR / "sender"
RECEIVER   = A2_DIR / "receiver"
INPUT_FILE = A2_DIR / "input.txt"
OUT_CSV    = A2_DIR / "results.csv"
OUT_XLSX   = A2_DIR / "results.xlsx"
GRAPHS_DIR = A2_DIR / "graphs"
RECV_PORT  = 9000

PROTOCOLS = {
    1: "Stop-and-Wait",
    2: "Go-Back-N",
    3: "Selective-Repeat",
}

ERROR_PROBS = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5]

# window sizes to test for windowed protocols
WINDOW_SIZES = [1, 2, 4, 8]

# ── column order for CSV / Excel ───────────────────────────────────
FIELDS = [
    "protocol", "window", "error_prob", "loss_prob",
    "frames_total",                   # original frames in file
    "frames_sent",                    # total transmissions (incl retransmits)
    "retransmits",
    "total_time_s",
    "efficiency_pct",
    "throughput_fps",                 # frames per second delivered
]


# ═══════════════════════════════════════════════════════════════════
#  Subprocess helpers
# ═══════════════════════════════════════════════════════════════════

def _port_in_use(port: int) -> bool:
    """Check via `ss` if a TCP port is currently bound."""
    r = subprocess.run(
        ["ss", "-tlnp", f"sport = :{port}"],
        capture_output=True, text=True
    )
    return f":{port}" in r.stdout


def wait_for_port(port: int, timeout: float = 8.0) -> bool:
    """Poll until receiver has bound the port."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if _port_in_use(port):
            return True
        time.sleep(0.05)
    return False


def wait_port_free(port: int, timeout: float = 8.0) -> bool:
    """Poll until the port is fully released by the previous receiver."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        if not _port_in_use(port):
            return True
        time.sleep(0.05)
    return False


def parse_sender_output(stdout: str) -> dict:
    """Extract stats from sender's stdout."""
    result = {
        "frames_total"  : 0,
        "frames_sent"   : 0,
        "retransmits"   : 0,
        "total_time_s"  : 0.0,
        "efficiency_pct": 0.0,
        "throughput_fps": 0.0,
    }

    # "File: input.txt  (1850 bytes, 41 frames)"
    m = re.search(r"\((\d+) bytes,\s*(\d+) frames\)", stdout)
    if m:
        result["frames_total"] = int(m.group(2))

    # "Frames sent : 45  (retransmits: 4)"
    m = re.search(r"Frames sent\s*:\s*(\d+)\s*\(retransmits:\s*(\d+)\)", stdout)
    if m:
        result["frames_sent"]  = int(m.group(1))
        result["retransmits"]  = int(m.group(2))

    # "Total time  : 1.3081 s"
    m = re.search(r"Total time\s*:\s*([\d.]+)\s*s", stdout)
    if m:
        result["total_time_s"] = float(m.group(1))

    # "Efficiency  : 91.11%"
    m = re.search(r"Efficiency\s*:\s*([\d.]+)%", stdout)
    if m:
        result["efficiency_pct"] = float(m.group(1))

    # throughput = original frames / time
    t = result["total_time_s"]
    if t > 0:
        result["throughput_fps"] = round(result["frames_total"] / t, 2)

    return result


def run_one(mode: int, window: int, error_prob: float, loss_prob: float,
            trial: int = 0) -> dict | None:
    """
    Launch receiver, then sender; collect and parse stats.
    Returns a result dict or None on failure.
    """
    proto_name = PROTOCOLS[mode]
    label = f"{proto_name} W={window} ep={error_prob:.2f} lp={loss_prob:.2f}"

    recv_out_file = A2_DIR / f"_recv_tmp_{trial}.bin"

    # ── Start receiver ──────────────────────────────────────────────
    recv_cmd = [str(RECEIVER), str(mode), str(window), str(recv_out_file)]
    recv_proc = subprocess.Popen(
        recv_cmd,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Wait for receiver to open its port
    if not wait_for_port(RECV_PORT, timeout=5.0):
        print(f"  [ERROR] Receiver did not open port {RECV_PORT} in time")
        recv_proc.kill()
        recv_proc.wait()
        return None

    # ── Run sender ──────────────────────────────────────────────────
    send_cmd = [
        str(SENDER),
        str(mode), str(window),
        f"{error_prob:.2f}", f"{loss_prob:.2f}",
        str(INPUT_FILE),
    ]
    try:
        send_result = subprocess.run(
            send_cmd,
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired:
        print(f"  [ERROR] Sender timed out for {label}")
        recv_proc.kill()
        recv_proc.wait()
        return None

    # Wait for receiver to finish
    try:
        recv_proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        recv_proc.kill()
        recv_proc.wait()

    # Clean up temp file
    if recv_out_file.exists():
        recv_out_file.unlink()

    # ── Parse output ────────────────────────────────────────────────
    stats = parse_sender_output(send_result.stdout)
    stats["protocol"]   = proto_name
    stats["window"]     = window
    stats["error_prob"] = error_prob
    stats["loss_prob"]  = loss_prob

    eff = stats["efficiency_pct"]
    print(f"  ✓  {label:50s}  sent={stats['frames_sent']:4d}  "
          f"retx={stats['retransmits']:3d}  "
          f"eff={eff:6.2f}%  "
          f"t={stats['total_time_s']:.3f}s")
    return stats


# ═══════════════════════════════════════════════════════════════════
#  Graph generation
# ═══════════════════════════════════════════════════════════════════

COLORS = {
    "Stop-and-Wait"   : "#E74C3C",
    "Go-Back-N"       : "#3498DB",
    "Selective-Repeat": "#2ECC71",
}
MARKERS = {
    "Stop-and-Wait"   : "o",
    "Go-Back-N"       : "s",
    "Selective-Repeat": "^",
}


def make_graphs(df: "pd.DataFrame"):
    GRAPHS_DIR.mkdir(exist_ok=True)

    # ── helper ──────────────────────────────────────────────────────
    def styled_ax(ax, title, xlabel, ylabel):
        ax.set_title(title, fontsize=13, fontweight="bold", pad=10)
        ax.set_xlabel(xlabel, fontsize=11)
        ax.set_ylabel(ylabel, fontsize=11)
        ax.grid(True, linestyle="--", alpha=0.5)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.legend(fontsize=9, framealpha=0.8)

    # ── 1. Efficiency vs Error Probability (fixed window = 4) ──────
    fig, ax = plt.subplots(figsize=(8, 5))
    sub = df[df["window"] == 4]
    for proto in PROTOCOLS.values():
        g = sub[sub["protocol"] == proto]
        if g.empty:
            continue
        g = g.groupby("error_prob")["efficiency_pct"].mean().reset_index()
        ax.plot(g["error_prob"], g["efficiency_pct"],
                label=proto, color=COLORS[proto], marker=MARKERS[proto],
                linewidth=2, markersize=7)
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=100))
    ax.set_xticks(ERROR_PROBS)
    styled_ax(ax, "Efficiency vs Error Probability  (Window=4)",
              "Error Probability", "Efficiency (%)")
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "efficiency_vs_error.png", dpi=150)
    plt.close(fig)
    print("  [graph] efficiency_vs_error.png")

    # ── 2. Frames sent vs Error Probability (fixed window = 4) ─────
    fig, ax = plt.subplots(figsize=(8, 5))
    sub = df[df["window"] == 4]
    for proto in PROTOCOLS.values():
        g = sub[sub["protocol"] == proto]
        if g.empty:
            continue
        g = g.groupby("error_prob")["frames_sent"].mean().reset_index()
        ax.plot(g["error_prob"], g["frames_sent"],
                label=proto, color=COLORS[proto], marker=MARKERS[proto],
                linewidth=2, markersize=7)
    ax.set_xticks(ERROR_PROBS)
    styled_ax(ax, "Total Frames Sent vs Error Probability  (Window=4)",
              "Error Probability", "Avg Frames Sent (incl. retransmits)")
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "frames_sent_vs_error.png", dpi=150)
    plt.close(fig)
    print("  [graph] frames_sent_vs_error.png")

    # ── 3. Efficiency vs Window Size (fixed error = 0.2) ───────────
    fig, ax = plt.subplots(figsize=(8, 5))
    sub = df[df["error_prob"] == 0.2]
    for proto in ["Go-Back-N", "Selective-Repeat"]:   # SAW window=1 only
        g = sub[sub["protocol"] == proto]
        if g.empty:
            continue
        g = g.groupby("window")["efficiency_pct"].mean().reset_index()
        ax.plot(g["window"], g["efficiency_pct"],
                label=proto, color=COLORS[proto], marker=MARKERS[proto],
                linewidth=2, markersize=7)
    ax.set_xticks(WINDOW_SIZES)
    ax.yaxis.set_major_formatter(ticker.PercentFormatter(xmax=100))
    styled_ax(ax, "Efficiency vs Window Size  (Error=0.2)",
              "Window Size (N)", "Efficiency (%)")
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "efficiency_vs_window.png", dpi=150)
    plt.close(fig)
    print("  [graph] efficiency_vs_window.png")

    # ── 4. Total time vs Error Probability (fixed window = 4) ──────
    fig, ax = plt.subplots(figsize=(8, 5))
    sub = df[df["window"] == 4]
    for proto in PROTOCOLS.values():
        g = sub[sub["protocol"] == proto]
        if g.empty:
            continue
        g = g.groupby("error_prob")["total_time_s"].mean().reset_index()
        ax.plot(g["error_prob"], g["total_time_s"],
                label=proto, color=COLORS[proto], marker=MARKERS[proto],
                linewidth=2, markersize=7)
    ax.set_xticks(ERROR_PROBS)
    styled_ax(ax, "Transmission Time vs Error Probability  (Window=4)",
              "Error Probability", "Total Time (s)")
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "time_vs_error.png", dpi=150)
    plt.close(fig)
    print("  [graph] time_vs_error.png")

    # ── 5. Retransmissions vs Error Probability (fixed window = 4) ─
    fig, ax = plt.subplots(figsize=(8, 5))
    sub = df[df["window"] == 4]
    for proto in PROTOCOLS.values():
        g = sub[sub["protocol"] == proto]
        if g.empty:
            continue
        g = g.groupby("error_prob")["retransmits"].mean().reset_index()
        ax.plot(g["error_prob"], g["retransmits"],
                label=proto, color=COLORS[proto], marker=MARKERS[proto],
                linewidth=2, markersize=7)
    ax.set_xticks(ERROR_PROBS)
    styled_ax(ax, "Retransmissions vs Error Probability  (Window=4)",
              "Error Probability", "Avg Retransmissions")
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "retransmits_vs_error.png", dpi=150)
    plt.close(fig)
    print("  [graph] retransmits_vs_error.png")

    # ── 6. Bar chart: efficiency at 0% vs 30% vs 50% error ─────────
    fig, axes = plt.subplots(1, 3, figsize=(14, 5), sharey=True)
    compare_probs = [0.0, 0.3, 0.5]
    for i, ep in enumerate(compare_probs):
        ax = axes[i]
        sub = df[(df["error_prob"] == ep) & (df["window"] == 4)]
        protos = list(PROTOCOLS.values())
        effs   = [
            sub[sub["protocol"] == p]["efficiency_pct"].mean()
            for p in protos
        ]
        bars = ax.bar(protos, effs,
                      color=[COLORS[p] for p in protos],
                      edgecolor="white", width=0.5)
        for bar, v in zip(bars, effs):
            ax.text(bar.get_x() + bar.get_width() / 2,
                    bar.get_height() + 1,
                    f"{v:.1f}%", ha="center", va="bottom", fontsize=9)
        ax.set_title(f"Error = {ep:.0%}", fontweight="bold")
        ax.set_ylim(0, 115)
        ax.set_ylabel("Efficiency (%)" if i == 0 else "")
        ax.grid(axis="y", linestyle="--", alpha=0.4)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.set_xticks(range(len(protos)))
        ax.set_xticklabels(["SAW", "GBN", "SR"], fontsize=10)
    fig.suptitle("Protocol Efficiency Comparison  (Window=4)",
                 fontsize=14, fontweight="bold", y=1.01)
    fig.tight_layout()
    fig.savefig(GRAPHS_DIR / "efficiency_bar_comparison.png",
                dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("  [graph] efficiency_bar_comparison.png")


# ═══════════════════════════════════════════════════════════════════
#  Excel generation
# ═══════════════════════════════════════════════════════════════════

def make_excel(df: "pd.DataFrame"):
    wb = openpyxl.Workbook()

    # ── colour helpers ──────────────────────────────────────────────
    HEADER_FILL = PatternFill("solid", fgColor="1A1A2E")
    HEADER_FONT = Font(color="FFFFFF", bold=True, size=11)
    ALT_FILL    = PatternFill("solid", fgColor="F0F4FF")
    PROTO_FILLS = {
        "Stop-and-Wait"   : PatternFill("solid", fgColor="FFE0E0"),
        "Go-Back-N"       : PatternFill("solid", fgColor="E0EEFF"),
        "Selective-Repeat": PatternFill("solid", fgColor="E0FFE8"),
    }
    thin = Side(style="thin", color="CCCCCC")
    border = Border(left=thin, right=thin, top=thin, bottom=thin)

    def style_header(ws, col_names):
        for col_idx, name in enumerate(col_names, 1):
            cell = ws.cell(row=1, column=col_idx, value=name)
            cell.fill = HEADER_FILL
            cell.font = HEADER_FONT
            cell.alignment = Alignment(horizontal="center", vertical="center")
            cell.border = border

    # ── Sheet 1: Raw Data ───────────────────────────────────────────
    ws1 = wb.active
    ws1.title = "Raw Data"
    ws1.row_dimensions[1].height = 22

    col_labels = [
        "Protocol", "Window", "Error Prob", "Loss Prob",
        "Frames (total)", "Frames Sent", "Retransmits",
        "Time (s)", "Efficiency (%)", "Throughput (fps)",
    ]
    style_header(ws1, col_labels)
    ws1.freeze_panes = "A2"

    col_widths = [18, 8, 11, 10, 15, 13, 13, 10, 15, 16]
    for i, w in enumerate(col_widths, 1):
        ws1.column_dimensions[openpyxl.utils.get_column_letter(i)].width = w

    for row_idx, row in enumerate(df.itertuples(index=False), 2):
        vals = [
            row.protocol, row.window,
            row.error_prob, row.loss_prob,
            row.frames_total, row.frames_sent, row.retransmits,
            round(row.total_time_s, 4),
            round(row.efficiency_pct, 2),
            round(row.throughput_fps, 2),
        ]
        fill = PROTO_FILLS.get(row.protocol, ALT_FILL)
        for col_idx, v in enumerate(vals, 1):
            cell = ws1.cell(row=row_idx, column=col_idx, value=v)
            cell.border = border
            cell.alignment = Alignment(horizontal="center")
            if row_idx % 2 == 0 and fill == ALT_FILL:
                cell.fill = ALT_FILL
            else:
                cell.fill = fill

    # ── Sheet 2: Pivot – Efficiency ─────────────────────────────────
    ws2 = wb.create_sheet("Efficiency by Error Prob")
    pivot_eff = (df[df["window"] == 4]
                 .groupby(["protocol", "error_prob"])["efficiency_pct"]
                 .mean()
                 .unstack("error_prob")
                 .round(2))
    pivot_eff.reset_index(inplace=True)

    headers = ["Protocol"] + [f"ep={ep:.1f}" for ep in ERROR_PROBS]
    style_header(ws2, headers)
    ws2.column_dimensions["A"].width = 20
    for col_idx in range(2, len(headers) + 1):
        ws2.column_dimensions[
            openpyxl.utils.get_column_letter(col_idx)
        ].width = 10

    for r_idx, r in enumerate(pivot_eff.itertuples(index=False), 2):
        proto = r[0]
        fill  = PROTO_FILLS.get(proto, ALT_FILL)
        for c_idx, v in enumerate(r, 1):
            cell = ws2.cell(row=r_idx, column=c_idx, value=v)
            cell.border = border
            cell.alignment = Alignment(horizontal="center")
            cell.fill = fill

    # ── Sheet 3: Pivot – Retransmissions ────────────────────────────
    ws3 = wb.create_sheet("Retransmits by Error Prob")
    pivot_rx = (df[df["window"] == 4]
                .groupby(["protocol", "error_prob"])["retransmits"]
                .mean()
                .unstack("error_prob")
                .round(1))
    pivot_rx.reset_index(inplace=True)

    style_header(ws3, headers)
    ws3.column_dimensions["A"].width = 20
    for col_idx in range(2, len(headers) + 1):
        ws3.column_dimensions[
            openpyxl.utils.get_column_letter(col_idx)
        ].width = 10

    for r_idx, r in enumerate(pivot_rx.itertuples(index=False), 2):
        proto = r[0]
        fill  = PROTO_FILLS.get(proto, ALT_FILL)
        for c_idx, v in enumerate(r, 1):
            cell = ws3.cell(row=r_idx, column=c_idx, value=v)
            cell.border = border
            cell.alignment = Alignment(horizontal="center")
            cell.fill = fill

    # ── Sheet 4: Pivot – Time ────────────────────────────────────────
    ws4 = wb.create_sheet("Time by Error Prob")
    pivot_t = (df[df["window"] == 4]
               .groupby(["protocol", "error_prob"])["total_time_s"]
               .mean()
               .unstack("error_prob")
               .round(4))
    pivot_t.reset_index(inplace=True)

    style_header(ws4, headers)
    ws4.column_dimensions["A"].width = 20
    for col_idx in range(2, len(headers) + 1):
        ws4.column_dimensions[
            openpyxl.utils.get_column_letter(col_idx)
        ].width = 12

    for r_idx, r in enumerate(pivot_t.itertuples(index=False), 2):
        proto = r[0]
        fill  = PROTO_FILLS.get(proto, ALT_FILL)
        for c_idx, v in enumerate(r, 1):
            cell = ws4.cell(row=r_idx, column=c_idx, value=v)
            cell.border = border
            cell.alignment = Alignment(horizontal="center")
            cell.fill = fill

    wb.save(OUT_XLSX)
    print(f"  Excel saved → {OUT_XLSX}")


# ═══════════════════════════════════════════════════════════════════
#  Main
# ═══════════════════════════════════════════════════════════════════

def build_test_matrix(quick: bool) -> list[tuple]:
    """
    Returns list of (mode, window, error_prob) tuples.
    loss_prob = error_prob / 2  inside run_one().
    """
    matrix = []
    probs = [0.0, 0.1] if quick else ERROR_PROBS

    for ep in probs:
        # Stop-and-Wait: window is always 1
        matrix.append((1, 1, ep))

        windows = [4] if quick else WINDOW_SIZES
        for w in windows:
            matrix.append((2, w, ep))  # Go-Back-N
            matrix.append((3, w, ep))  # Selective Repeat

    return matrix


def main():
    parser = argparse.ArgumentParser(description="ARQ Protocol Experiment Runner")
    parser.add_argument("--quick", action="store_true",
                        help="Run a small subset (fast smoke test)")
    parser.add_argument("--trials", type=int, default=3,
                        help="Number of trials per configuration (default 3)")
    args = parser.parse_args()

    # Sanity checks
    for exe in [SENDER, RECEIVER]:
        if not exe.exists():
            sys.exit(f"[ERROR] Binary not found: {exe}\n"
                     "        Run 'make' first.")
    if not INPUT_FILE.exists():
        sys.exit(f"[ERROR] Input file not found: {INPUT_FILE}")

    matrix = build_test_matrix(args.quick)
    total  = len(matrix) * args.trials
    print(f"\n{'='*65}")
    print(f"  ARQ Experiment Runner")
    print(f"  Configurations: {len(matrix)}   Trials each: {args.trials}")
    print(f"  Total runs    : {total}")
    print(f"{'='*65}\n")

    rows = []
    run_no = 0

    for (mode, window, ep) in matrix:
        lp = round(ep / 2, 2)
        for trial in range(args.trials):
            run_no += 1
            print(f"[{run_no:3d}/{total}] ", end="", flush=True)
            result = run_one(mode, window, ep, lp, trial=run_no)
            if result:
                rows.append({f: result[f] for f in FIELDS})
            # Wait for OS to fully release the port before next run
            wait_port_free(RECV_PORT, timeout=6.0)
            time.sleep(0.05)

    if not rows:
        sys.exit("[ERROR] No results collected.")

    # ── Write CSV ───────────────────────────────────────────────────
    with open(OUT_CSV, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n✓ CSV saved  → {OUT_CSV}  ({len(rows)} rows)")

    # ── Graphs + Excel ──────────────────────────────────────────────
    if HAS_PANDAS:
        df = pd.DataFrame(rows)
        print("\nGenerating graphs …")
        make_graphs(df)
        print("\nGenerating Excel …")
        make_excel(df)
        print(f"\n✓ All done.")
        print(f"  CSV   : {OUT_CSV}")
        print(f"  Excel : {OUT_XLSX}")
        print(f"  Graphs: {GRAPHS_DIR}/")
    else:
        print("\n[INFO] Install pandas/matplotlib/openpyxl for graphs + Excel.")

    # ── Print summary table ─────────────────────────────────────────
    if HAS_PANDAS:
        print("\n" + "="*65)
        print("  SUMMARY  (window=4, averaged over all trials)")
        print("="*65)
        df4 = df[df["window"] == 4]
        summary = (df4.groupby(["protocol", "error_prob"])
                   .agg(eff=("efficiency_pct", "mean"),
                        retx=("retransmits", "mean"),
                        t=("total_time_s", "mean"))
                   .round(2))
        print(summary.to_string())
        print("="*65)


if __name__ == "__main__":
    main()
