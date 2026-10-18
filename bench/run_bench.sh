#!/bin/sh
# run_bench.sh - run every experiment and collect one CSV.
#
#   usage: sh bench/run_bench.sh path/to/disksim OUTDIR
#
# Every run appends one row to OUTDIR/results.csv. The `tag` column says
# which experiment the row belongs to; bench/plot.py splits on it.
#
# All runs use the default drive (disksim --info) and fixed seeds, so the
# results are reproducible bit-for-bit.
set -eu

SIM=${1:-build/disksim}
OUT=${2:-results}
CSV="$OUT/results.csv"
mkdir -p "$OUT"
rm -f "$CSV"

SCHEDS="fcfs sstf scan cscan look clook sptf sptf-aged"
N=5000            # requests per run
RATE=120          # random / hot-cold arrival rate (IOPS): above FCFS's
                  # ~77 IOPS capacity, below what reordering can sustain
GROWN=20000       # grown defects for the "with defects" runs (0.01% of LBAs)
AGE=500           # sptf-aged starvation limit, ms (see experiment 6)

run() { "$SIM" --quiet --csv "$CSV" "$@"; }

echo "[1/6] scheduler x workload matrix (with and without grown defects)"
for d in 0 $GROWN; do
  for s in $SCHEDS; do
    # sequential 64 KiB reads, host keeps the queue full (streaming)
    run --tag matrix --sched $s --age-ms $AGE --defects $d \
        --workload seq --size 128 --rate 0 --requests $N
    run --tag matrix --sched $s --age-ms $AGE --defects $d \
        --workload random --size 8 --rate $RATE --requests $N
    run --tag matrix --sched $s --age-ms $AGE --defects $d \
        --workload hotcold --size 8 --rate $RATE --requests $N
    run --tag matrix --sched $s --age-ms $AGE --defects $d \
        --trace traces/sample_trace.csv
  done
done

echo "[2/6] queue depth sweep (NCQ): random 4 KiB, queue always full"
for q in 1 2 4 8 16 32 64 128; do
  for s in fcfs sstf look sptf; do
    run --tag qdepth --sched $s --qdepth $q --workload random --size 8 \
        --rate 0 --requests 2000
  done
done

echo "[3/6] grown defect sweep: sequential throughput"
for d in 0 1000 2000 5000 10000 20000 50000 100000; do
  run --tag defects --sched fcfs --defects $d --workload seq --size 128 \
      --rate 0 --requests $N
done

echo "[4/6] zoned bit recording: sequential throughput by position"
for f in 0.0 0.25 0.5 0.75 0.99; do
  run --tag zone_$f --sched fcfs --workload seq --size 128 --rate 0 \
      --requests 2000 --start-frac $f
done

echo "[5/6] cylinder skew on/off: sequential throughput"
run --tag skew_0.12 --sched fcfs --workload seq --size 128 --rate 0 --requests $N
run --tag skew_0 --sched fcfs --workload seq --size 128 --rate 0 --requests $N --skew 0

echo "[6/6] sptf-aged: starvation limit sweep (random and trace)"
for a in 50 100 250 500 1000; do
  run --tag age_$a --sched sptf-aged --age-ms $a --workload random --size 8 \
      --rate $RATE --requests $N
  run --tag age_$a --sched sptf-aged --age-ms $a --trace traces/sample_trace.csv
done

echo "wrote $CSV ($(($(wc -l < "$CSV") - 1)) runs)"
