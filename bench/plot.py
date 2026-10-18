#!/usr/bin/env python3
"""plot.py - turn results/results.csv into results.md and PNG charts.

usage: python3 bench/plot.py RESULTS_DIR

Every number written here is read from results.csv; nothing is typed in by
hand. Charts use a validated colorblind-safe categorical order (blue,
orange, aqua, yellow) and are backed by the tables in results.md.
"""
import csv
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# ---- style ---------------------------------------------------------------
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e4e3df"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]

plt.rcParams.update({
    "figure.facecolor": SURFACE,
    "axes.facecolor": SURFACE,
    "savefig.facecolor": SURFACE,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK2,
    "axes.titlecolor": INK,
    "axes.titlesize": 11,
    "axes.titleweight": "bold",
    "axes.labelsize": 9,
    "xtick.color": INK2,
    "ytick.color": INK2,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "axes.grid": True,
    "axes.grid.axis": "y",
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "axes.spines.top": False,
    "axes.spines.right": False,
    "axes.axisbelow": True,
    "legend.frameon": False,
    "legend.fontsize": 8,
    "font.size": 9,
    "lines.linewidth": 2,
})

SCHEDS = ["fcfs", "sstf", "scan", "cscan", "look", "clook", "sptf", "sptf-aged"]
LABEL = {"fcfs": "FCFS", "sstf": "SSTF", "scan": "SCAN", "cscan": "C-SCAN",
         "look": "LOOK", "clook": "C-LOOK", "sptf": "SPTF", "sptf-aged": "SPTF+aging"}
WORKLOADS = ["seq", "random", "hotcold", "trace"]


def load(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k, v in r.items():
            if k in ("tag", "workload", "scheduler"):
                continue
            r[k] = float(v)
    return rows


def pick(rows, **kw):
    out = [r for r in rows if all(r[k] == v for k, v in kw.items())]
    return out


def one(rows, **kw):
    m = pick(rows, **kw)
    if len(m) != 1:
        raise SystemExit(f"expected 1 row for {kw}, found {len(m)}")
    return m[0]


def fmt_ms(x):
    return f"{x:,.0f}" if x >= 100 else f"{x:.1f}"


def bar_labels(ax, bars, values, fmt):
    for b, v in zip(bars, values):
        ax.annotate(fmt(v), (b.get_x() + b.get_width() / 2, b.get_height()),
                    xytext=(0, 2), textcoords="offset points",
                    ha="center", va="bottom", fontsize=7, color=INK2)


def save(fig, out, name):
    path = os.path.join(out, name)
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    return name


# ---- charts --------------------------------------------------------------

def chart_latency(rows, out):
    """avg / p99 response and max wait per scheduler; FCFS is off-scale."""
    wls = [("random", "Random 4 KiB @ 120 IOPS"),
           ("hotcold", "Hot/cold 80/20 @ 120 IOPS"),
           ("trace", "Multi-stream trace")]
    scheds = [s for s in SCHEDS if s != "fcfs"]
    fig, axes = plt.subplots(1, 3, figsize=(13, 4.2), sharey=False)
    metrics = [("avg_response_ms", "avg response"),
               ("p99_response_ms", "p99 response"),
               ("max_wait_ms", "max wait (starvation)")]
    w = 0.26
    for ax, (wl, title) in zip(axes, wls):
        fc = one(rows, tag="matrix", workload=wl, scheduler="fcfs", grown_defects=0)
        for i, (key, lab) in enumerate(metrics):
            vals = [one(rows, tag="matrix", workload=wl, scheduler=s,
                        grown_defects=0)[key] for s in scheds]
            xs = [j + (i - 1) * (w + 0.02) for j in range(len(scheds))]
            ax.bar(xs, vals, width=w, color=SERIES[i], label=lab,
                   edgecolor=SURFACE, linewidth=1)
        ax.set_xticks(range(len(scheds)))
        ax.set_xticklabels([LABEL[s] for s in scheds], rotation=30, ha="right")
        ax.set_title(title, loc="left")
        ax.set_ylabel("ms")
        ax.text(0.99, 0.97, f"FCFS off-scale: avg {fc['avg_response_ms']/1000:.1f} s",
                transform=ax.transAxes, ha="right", va="top", fontsize=7.5, color=INK2)
    fig.suptitle("Response time and starvation by scheduler (5000 requests, no defects)",
                 x=0.01, ha="left", fontsize=12, fontweight="bold", color=INK)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper left", bbox_to_anchor=(0.01, 0.94), ncol=3)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    return save(fig, out, "chart_latency.png")


def chart_breakdown(rows, out):
    """Where service time goes: seek vs rotation vs transfer (random)."""
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(12, 4))
    scheds = SCHEDS
    data = [one(rows, tag="matrix", workload="random", scheduler=s, grown_defects=0)
            for s in scheds]
    x = range(len(scheds))
    bottom = [0.0] * len(scheds)
    parts = [("avg_seek_ms", "seek"), ("avg_rotate_ms", "rotational latency"),
             ("avg_transfer_ms", "transfer")]
    for i, (k, lab) in enumerate(parts):
        vals = [d[k] for d in data]
        a1.bar(x, vals, bottom=bottom, width=0.6, color=SERIES[i], label=lab,
               edgecolor=SURFACE, linewidth=1.5)
        bottom = [b + v for b, v in zip(bottom, vals)]
    for xi, tot in zip(x, bottom):
        a1.annotate(f"{tot:.2f}", (xi, tot), xytext=(0, 2), textcoords="offset points",
                    ha="center", fontsize=7, color=INK2)
    a1.set_xticks(list(x))
    a1.set_xticklabels([LABEL[s] for s in scheds], rotation=30, ha="right")
    a1.set_ylabel("ms per request")
    a1.set_title("Average service time breakdown (random 4 KiB)", loc="left")
    a1.legend(loc="upper right")
    a1.text(0.99, 0.70, f"transfer = {data[0]['avg_transfer_ms']:.3f} ms\n(too thin to see)",
            transform=a1.transAxes, ha="right", va="top", fontsize=7.5, color=INK2)

    dist = [d["avg_seek_distance_cyl"] for d in data]
    bars = a2.bar(x, dist, width=0.6, color=SERIES[0])
    bar_labels(a2, bars, dist, lambda v: f"{v:,.0f}")
    a2.set_xticks(list(x))
    a2.set_xticklabels([LABEL[s] for s in scheds], rotation=30, ha="right")
    a2.set_ylabel("cylinders per request")
    a2.set_title("Head movement per request (random 4 KiB)", loc="left")
    fig.tight_layout()
    return save(fig, out, "chart_breakdown.png")


def chart_qdepth(rows, out):
    fig, ax = plt.subplots(figsize=(7.5, 4.2))
    qds = sorted({r["queue_depth"] for r in pick(rows, tag="qdepth")})
    ends = []
    for i, s in enumerate(["fcfs", "sstf", "look", "sptf"]):
        ys = [one(rows, tag="qdepth", scheduler=s, queue_depth=q)["iops"] for q in qds]
        ax.plot(qds, ys, color=SERIES[i], marker="o", markersize=5,
                markeredgecolor=SURFACE, markeredgewidth=1.5, label=LABEL[s])
        ends.append((ys[-1], LABEL[s]))
    # Direct end labels; series that end at (nearly) the same value share one
    # label instead of printing on top of each other.
    ends.sort()
    groups = []
    for v, name in ends:
        if groups and abs(v - groups[-1][0]) < 8:
            groups[-1][1].append(name)
        else:
            groups.append((v, [name]))
    for v, names in groups:
        ax.annotate(f"{' / '.join(names)} {v:.0f}", (qds[-1], v), xytext=(6, 0),
                    textcoords="offset points", va="center", fontsize=8, color=INK)
    ax.set_xscale("log", base=2)
    ax.set_xticks(qds)
    ax.set_xticklabels([str(int(q)) for q in qds])
    ax.set_xlim(qds[0] * 0.8, qds[-1] * 2.2)
    ax.set_ylim(0, None)
    ax.set_xlabel("drive queue depth (commands visible to the scheduler)")
    ax.set_ylabel("IOPS")
    ax.set_title("Random 4 KiB IOPS vs queue depth (NCQ effect)", loc="left")
    ax.legend(loc="upper left")
    fig.tight_layout()
    return save(fig, out, "chart_qdepth.png")


def chart_defects(rows, out):
    fig, ax = plt.subplots(figsize=(7.5, 4))
    pts = sorted(pick(rows, tag="defects"), key=lambda r: r["grown_defects"])
    xs = [int(r["grown_defects"]) for r in pts]
    ys = [r["mb_per_s"] for r in pts]
    pos = list(range(len(xs)))
    ax.plot(pos, ys, color=SERIES[0], marker="o", markersize=5,
            markeredgecolor=SURFACE, markeredgewidth=1.5)
    for p, y in zip(pos, ys):
        ax.annotate(f"{y:.1f}", (p, y), xytext=(0, 7), textcoords="offset points",
                    ha="center", fontsize=7.5, color=INK2)
    ax.set_xticks(pos)
    ax.set_xticklabels([f"{x:,}" for x in xs])
    ax.set_ylim(0, max(ys) * 1.15)
    ax.set_xlabel("grown defects reallocated to the spare area (of 199.4 M LBAs)")
    ax.set_ylabel("MB/s")
    ax.set_title("Sequential 64 KiB read throughput vs grown defects", loc="left")
    fig.tight_layout()
    return save(fig, out, "chart_defects.png")


def chart_zones(rows, out):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 3.8),
                                 gridspec_kw={"width_ratios": [2.2, 1]})
    zs = sorted([r for r in rows if r["tag"].startswith("zone_")],
                key=lambda r: float(r["tag"][5:]))
    labels = [f"{float(r['tag'][5:]) * 100:.0f}%" for r in zs]
    vals = [r["mb_per_s"] for r in zs]
    bars = a1.bar(range(len(zs)), vals, width=0.6, color=SERIES[0])
    bar_labels(a1, bars, vals, lambda v: f"{v:.1f}")
    a1.set_xticks(range(len(zs)))
    a1.set_xticklabels(labels)
    a1.set_xlabel("start position (fraction of LBA space; 0% = outer edge)")
    a1.set_ylabel("MB/s")
    a1.set_title("Zoned recording: sequential MB/s by position", loc="left")

    sk = [one(rows, tag="skew_0.12"), one(rows, tag="skew_0")]
    vals = [r["mb_per_s"] for r in sk]
    bars = a2.bar([0, 1], vals, width=0.55, color=SERIES[0])
    bar_labels(a2, bars, vals, lambda v: f"{v:.1f}")
    a2.set_xticks([0, 1])
    a2.set_xticklabels(["skew 0.12 rev\n(default)", "no skew"])
    a2.set_ylabel("MB/s")
    a2.set_title("Cylinder skew", loc="left")
    fig.tight_layout()
    return save(fig, out, "chart_zones_skew.png")


def age_rows(rows, wl):
    out = []
    for r in rows:
        if r["tag"].startswith("age_") and r["workload"] == wl:
            out.append((float(r["tag"][4:]), r))
    return sorted(out, key=lambda t: t[0])


def chart_aging(rows, out):
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(12, 4))
    for i, (wl, lab) in enumerate([("random", "random @ 120 IOPS"),
                                   ("trace", "multi-stream trace")]):
        ar = age_rows(rows, wl)
        base = one(rows, tag="matrix", workload=wl, scheduler="sptf", grown_defects=0)
        xs = list(range(len(ar) + 1))
        avg = [r["avg_response_ms"] for _, r in ar] + [base["avg_response_ms"]]
        mw = [r["max_wait_ms"] for _, r in ar] + [base["max_wait_ms"]]
        for ax, ys in ((a1, avg), (a2, mw)):
            ax.plot(xs, ys, color=SERIES[i], marker="o", markersize=5,
                    markeredgecolor=SURFACE, markeredgewidth=1.5, label=lab)
        a2.annotate(f"{fmt_ms(mw[-1])} ms", (xs[-1], mw[-1]),
                    xytext=(0, 8 if i == 0 else -12), textcoords="offset points",
                    ha="center", fontsize=7.5, color=INK2)
        ticks = [f"{a:.0f}" for a, _ in ar] + ["none\n(pure SPTF)"]
    for ax, t in ((a1, "Average response time"), (a2, "Maximum wait (starvation)")):
        ax.set_yscale("log")
        ax.set_xticks(xs)
        ax.set_xticklabels(ticks)
        ax.set_xlabel("SPTF aging limit (ms)")
        ax.set_ylabel("ms (log scale)")
        ax.set_title(t, loc="left")
    a1.legend(loc="upper right")
    fig.suptitle("SPTF with aging: too short a limit collapses into FCFS",
                 x=0.01, ha="left", fontsize=12, fontweight="bold", color=INK)
    fig.tight_layout()
    return save(fig, out, "chart_aging.png")


# ---- markdown ------------------------------------------------------------

def md_table(header, rows):
    s = "| " + " | ".join(header) + " |\n"
    s += "|" + "|".join("---:" if i else "---" for i in range(len(header))) + "|\n"
    for r in rows:
        s += "| " + " | ".join(r) + " |\n"
    return s


def write_markdown(rows, out, charts):
    md = ["# disk-sim benchmark results\n",
          "Generated by `make bench` from `results.csv`. Drive: default config "
          "(`disksim --info`): 7200 rpm, 50,000 cylinders x 4 heads, 16 zones "
          "(1300 -> 700 sectors/track), 512 B sectors, 102.1 GB. "
          "Response = completion - arrival. MB = 10^6 bytes.\n"]

    descr = {
        "seq": "Sequential 64 KiB reads, host keeps the queue full (all 5000 requests "
               "issued at t=0, so response times measure queueing, not the drive)",
        "random": "Uniform random 4 KiB reads, Poisson arrivals at 120 IOPS",
        "hotcold": "4 KiB reads, 80% to the first 20% of LBAs, Poisson 120 IOPS",
        "trace": "traces/sample_trace.csv (synthetic: 4 sequential streams + "
                 "metadata + random, bursty arrivals, 5000 requests)",
    }
    md.append("## 1. Scheduler x workload (no defects)\n")
    for wl in WORKLOADS:
        md.append(f"### {wl}\n\n{descr[wl]}.\n")
        tr = []
        for s in SCHEDS:
            r = one(rows, tag="matrix", workload=wl, scheduler=s, grown_defects=0)
            tr.append([LABEL[s], f"{r['iops']:.1f}", f"{r['mb_per_s']:.2f}",
                       fmt_ms(r["avg_response_ms"]), fmt_ms(r["p95_response_ms"]),
                       fmt_ms(r["p99_response_ms"]), fmt_ms(r["max_wait_ms"]),
                       f"{r['avg_service_ms']:.2f}",
                       f"{r['avg_seek_distance_cyl']:,.0f}"])
        md.append(md_table(["scheduler", "IOPS", "MB/s", "avg ms", "p95 ms", "p99 ms",
                            "max wait ms", "service ms", "cyl/req"], tr))

    md.append("## 2. Impact of 20,000 grown defects (G-list)\n")
    tr = []
    for wl in WORKLOADS:
        for s in ["fcfs", "sptf"]:
            a = one(rows, tag="matrix", workload=wl, scheduler=s, grown_defects=0)
            b = one(rows, tag="matrix", workload=wl, scheduler=s, grown_defects=20000)
            tr.append([wl, LABEL[s], f"{a['mb_per_s']:.2f}", f"{b['mb_per_s']:.2f}",
                       f"{(b['mb_per_s'] / a['mb_per_s'] - 1) * 100:+.1f}%",
                       fmt_ms(a["avg_response_ms"]), fmt_ms(b["avg_response_ms"]),
                       f"{int(b['remapped_sectors'])}"])
    md.append(md_table(["workload", "scheduler", "MB/s clean", "MB/s defects", "change",
                        "avg ms clean", "avg ms defects", "remapped sectors read"], tr))

    md.append("## 3. Queue depth sweep (random 4 KiB, queue always full)\n")
    qds = sorted({r["queue_depth"] for r in pick(rows, tag="qdepth")})
    tr = []
    for q in qds:
        line = [str(int(q))]
        for s in ["fcfs", "sstf", "look", "sptf"]:
            line.append(f"{one(rows, tag='qdepth', scheduler=s, queue_depth=q)['iops']:.1f}")
        tr.append(line)
    md.append(md_table(["queue depth", "FCFS IOPS", "SSTF IOPS", "LOOK IOPS", "SPTF IOPS"], tr))

    md.append("## 4. Sequential throughput vs grown defects\n")
    tr = []
    pts = sorted(pick(rows, tag="defects"), key=lambda r: r["grown_defects"])
    base = pts[0]["mb_per_s"]
    for r in pts:
        tr.append([f"{int(r['grown_defects']):,}", f"{r['mb_per_s']:.2f}",
                   f"{(r['mb_per_s'] / base - 1) * 100:+.1f}%",
                   f"{int(r['remapped_sectors'])}"])
    md.append(md_table(["grown defects", "MB/s", "vs clean", "remapped sectors read"], tr))

    md.append("## 5. Zoned recording and cylinder skew (sequential 64 KiB)\n")
    tr = []
    for r in sorted([r for r in rows if r["tag"].startswith("zone_")],
                    key=lambda r: float(r["tag"][5:])):
        tr.append([f"{float(r['tag'][5:]) * 100:.0f}%", f"{r['mb_per_s']:.2f}"])
    md.append(md_table(["start position", "MB/s"], tr))
    a, b = one(rows, tag="skew_0.12"), one(rows, tag="skew_0")
    md.append(md_table(["cylinder skew", "MB/s"],
                       [["0.12 rev (default)", f"{a['mb_per_s']:.2f}"],
                        ["0 (none)", f"{b['mb_per_s']:.2f}"]]))

    md.append("## 6. SPTF aging limit\n")
    tr = []
    for wl in ["random", "trace"]:
        for age, r in age_rows(rows, wl):
            tr.append([wl, f"{age:.0f}", f"{r['iops']:.1f}", fmt_ms(r["avg_response_ms"]),
                       fmt_ms(r["p99_response_ms"]), fmt_ms(r["max_wait_ms"])])
        p = one(rows, tag="matrix", workload=wl, scheduler="sptf", grown_defects=0)
        tr.append([wl, "none (SPTF)", f"{p['iops']:.1f}", fmt_ms(p["avg_response_ms"]),
                   fmt_ms(p["p99_response_ms"]), fmt_ms(p["max_wait_ms"])])
    md.append(md_table(["workload", "age limit ms", "IOPS", "avg ms", "p99 ms",
                        "max wait ms"], tr))

    md.append("## Charts\n")
    for c in charts:
        md.append(f"![{c}]({c})\n")
    with open(os.path.join(out, "results.md"), "w", newline="\n") as f:
        f.write("\n".join(md))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "results"
    rows = load(os.path.join(out, "results.csv"))
    charts = [chart_latency(rows, out), chart_breakdown(rows, out),
              chart_qdepth(rows, out), chart_defects(rows, out),
              chart_zones(rows, out), chart_aging(rows, out)]
    write_markdown(rows, out, charts)
    print(f"plot.py: wrote {out}/results.md and {len(charts)} charts")


if __name__ == "__main__":
    main()
