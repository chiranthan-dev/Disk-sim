# LEARNING.md - a guided walkthrough of disk-sim

This file is for you, the person who has to explain this project in an
interview. Read it top to bottom once, then use it as a reference. Every
number quoted here comes from `results/results.md` (produced by `make bench`).

Contents

1. [The HDD concepts behind each module](#1-the-hdd-concepts-behind-each-module)
2. [File-by-file, function-by-function tour](#2-file-by-file-function-by-function-tour)
3. [Design choices and their alternatives](#3-design-choices-and-their-alternatives)
4. [How to explain the project in 60 seconds and in 5 minutes](#4-how-to-explain-the-project)
5. [Resume bullets](#5-resume-bullets)
6. [Exercises to make it yours](#6-exercises-to-make-it-yours)

---

## 1. The HDD concepts behind each module

### 1.1 Geometry: platters, heads, tracks, cylinders, sectors

A drive has one or more **platters**, each with two recording **surfaces**.
Each surface has a **head** on an arm; all arms move together on one
**actuator**. A **track** is one ring on one surface. The set of tracks at the
same radius on every surface is a **cylinder**: once the actuator is at a
cylinder, any head can read its track without moving the arm. A track is
divided into **sectors** (512 B traditionally, 4096 B on "Advanced Format"
drives).

So a physical address is **CHS** = (cylinder, head, sector). The host does not
see CHS anymore; it sees a flat **LBA** (logical block address) from 0 to N-1.
The drive translates.

In the simplest layout (one fixed sectors-per-track `S`, `H` heads):

```
LBA = (cylinder * H + head) * S + sector
cylinder = LBA / (H*S),  head = (LBA / S) % H,  sector = LBA % S
```

Note the order: sector fastest, then head, then cylinder. Consecutive LBAs fill
a track, then switch heads (no arm movement), and only then move the arm one
cylinder. That ordering is what makes sequential I/O cheap.

**Cylinder 0 is the outer edge** and LBA 0 lives there.

### 1.2 Zoned bit recording (ZBR)

The platter spins at a constant angular speed, but an outer track is longer
than an inner one (circumference = 2 pi r). If every track held the same number of
sectors, the outer tracks would waste space. Drives therefore split the surface
into **zones**: bands of cylinders that share one sectors-per-track (SPT), with
the outer zones holding more sectors.

Consequences:

- The fixed CHS formula breaks; the drive needs a **zone table** (first
  cylinder, SPT, first LBA per zone) and searches it.
- One revolution moves more data on the outside: our default drive streams
  **77.56 MB/s at the start of the disk and 41.49 MB/s at the end**, exactly the
  1300 : 700 SPT ratio. This is why the first partition of a disk is the
  fastest one.

### 1.3 Seek time

Moving the arm to another cylinder is a **seek**. It has two parts:

- **Movement.** A voice-coil motor accelerates the arm for half the trip and
  brakes for the other half. With constant acceleration, distance grows with
  time squared, so **time grows with the square root of distance**.
- **Settle.** At the target, the arm rings; the servo system must lock onto the
  track before data can be read. This fixed cost dominates short seeks.

Model: `seek(d) = 0 if d == 0, else settle + k * sqrt(d)`.
Default: 0.8 + 0.068 sqrt(d) ms, so 0.87 ms for one cylinder and 16.0 ms for a
full stroke, which are realistic numbers for a 7200 rpm desktop drive.

### 1.4 Rotational latency

After the seek, the head waits for the right sector to rotate underneath it. At
7200 rpm one revolution is 60000/7200 = **8.333 ms**, so the average wait for a
random sector is half of that, ~4.17 ms. Our FCFS random run measures
4.19 ms of average rotational latency, which matches.

The important modelling decision: **we do not use "average half a revolution".**
The platter angle is computed from the simulated clock:

```
angle(t) = (t mod T) / T          (fraction of a revolution)
wait     = ((sector_angle - angle(t_after_seek)) mod 1) * T
```

This makes two real effects appear on their own:

- A sequential request issued the instant the previous one finishes waits
  **zero**: the next sector is right under the head.
- If you arrive just after your sector went by, you wait almost a full
  revolution (the unit test `single_request` checks an 8.5 ms wait).

### 1.5 Transfer time

Once the sector is under the head, reading `n` sectors of a track with `S`
sectors takes `n / S` of a revolution: `transfer = n / S * T`. A 4 KiB random
read (8 sectors) takes only 0.066 ms on our drive, so random I/O is >99%
mechanical overhead.

### 1.6 Track and cylinder crossings, and skew

A request can run off the end of a track. Our disk splits every request into
**extents**, one per track, and charges each extent its own positioning.

- **Head switch** (next surface, same cylinder): free in this model, and the
  next track's sector 0 arrives exactly when the previous track ends.
- **Cylinder switch**: a 1-cylinder seek (0.87 ms). If sector 0 of the next
  cylinder sits at the same angle as on this one, it has already passed when
  the arm settles, and you wait almost a full revolution. **Cylinder skew**
  rotates each cylinder's sector 0 by a fraction of a revolution (default 0.12,
  enough to cover the 0.104-revolution seek). Measured: **77.55 MB/s with skew
  vs 63.91 MB/s without (-17.6%).**

### 1.7 Defects: P-list and G-list

No platter is perfect. Drives keep two defect lists:

**P-list (primary, factory).** Found during factory format. Those sectors are
**slipped**: LBAs are assigned only to good sectors, so every following LBA
shifts by one. The LBA order still equals the physical order, so a sequential
read just passes over the bad sector (cost: one sector time, ~0.006 ms) with no
seek. Slipping is only possible at format time, because it moves the physical
location of every later LBA.

**G-list (grown).** Sectors that fail in the field. Slipping would move user
data, so the drive instead **reallocates** just that LBA to a **spare sector**
in a reserved area and records the mapping. Cheap to do, but now a sequential
read that crosses that LBA must seek to the spare area and back. Our spares are
the innermost 200 cylinders, so a hit from the outer zone costs two
near-full-stroke seeks plus rotational waits. The unit test `remap_timing`
shows a 5-sector read going from 6.25 ms to 16.25 ms because of one remap.

Measured: **20,000 grown defects (0.01% of LBAs) cut sequential throughput
from 77.55 to 44.94 MB/s (-42.1%)**, while random 4 KiB reads change by -0.1%.
Why the difference? A 327 MB sequential pass touches 76 remapped sectors; a
5000-request random run touches 2.

In real drives, SMART attribute 5 ("Reallocated Sectors Count") counts G-list
entries; a growing count is a classic early warning of a failing drive.

### 1.8 Scheduling

When several requests are waiting, the order you serve them in changes the
total mechanical time. The policies:

| Policy | Rule | Strength | Weakness |
|---|---|---|---|
| FCFS | arrival order | fair, predictable | no optimisation; 77.8 IOPS on random 4 KiB |
| SSTF | nearest cylinder | short seeks | starvation; ignores rotation |
| SCAN | sweep in one direction to the edge, reverse | bounded wait | wasted travel to the edge; middle cylinders served twice as often |
| C-SCAN | sweep inward only, then fly back to cylinder 0 | more uniform waits | the return trip is pure overhead |
| LOOK | SCAN, but reverse at the last request | no wasted edge travel | still blind to rotation |
| C-LOOK | C-SCAN, but jump back to the lowest request | uniform and efficient | still blind to rotation |
| SPTF | smallest seek + rotational latency | best throughput and mean latency | starvation; needs the platter angle |
| SPTF + aging | SPTF, but a request older than a limit goes first | bounds starvation | a limit that is too low turns it into FCFS |

**Starvation.** SSTF and SPTF are greedy: a request in an awkward spot can wait
while newer, cheaper requests keep jumping ahead. We report **max wait** to
expose it. Example: on the hot/cold workload SSTF's worst wait is **760 ms**,
LOOK's is **342 ms**. Even elevators are not immune: if new requests keep
landing on the head's current cylinder, LOOK never leaves it (the unit test
`starvation` demonstrates this "arm stickiness").

### 1.9 NCQ and why scheduling moved into the drive

The operating system can reorder by LBA (an elevator), but it **cannot know the
platter's angle**, so it cannot do SPTF. **Native Command Queuing** (SATA)
lets the host send up to **32** commands at once; the drive's firmware
reorders them by actual positioning time (rotational position ordering), then
reports completions out of order.

Our queue-depth sweep shows the effect (random 4 KiB, queue always full):

| queue depth | FCFS | SSTF | SPTF |
|---:|---:|---:|---:|
| 1 | 77.8 | 77.8 | 77.8 |
| 32 | 77.8 | 145.4 | 183.9 |
| 128 | 77.8 | 169.1 | 267.2 |

With one command visible there is nothing to choose from, so every policy is
FCFS. With 32, SPTF does 2.36x FCFS. The deeper the queue, the more likely
there is a request that is both close and about to rotate under the head.

### 1.10 Open vs closed workloads, and why FCFS "explodes"

Our generators use **open** arrivals: requests arrive at Poisson-distributed
times regardless of whether the disk is keeping up. If the arrival rate
(120 IOPS) exceeds what a policy can serve (FCFS: ~77), the queue grows for
the whole run, and FCFS response time reaches **12,132 ms** on average. That is
not a bug; it is queueing theory: when utilisation exceeds 1, waiting time grows
without bound. A **closed** workload (fixed number of outstanding requests, as
in `fio --iodepth`) is what the queue-depth sweep approximates.

---

## 2. File-by-file, function-by-function tour

Suggested reading order: `include/disksim.h` -> `timing.c` -> `geometry.c` ->
`defects.c` -> `disk.c` -> `scheduler.c` -> `sim.c` -> tests.

### include/disksim.h - the public API

The only header a linking project needs. Opaque `disk_t`, a `disk_config_t`
filled by `disk_config_default()`, request/timing/info structs, and functions:

- `disk_open(cfg, &err)` / `disk_close(d)` - build and free a disk.
- `disk_service_request(d, req, now_ms, &timing)` - the core call: serve a
  request starting at `now_ms`, move the head, return seek/rotate/transfer.
- `disk_estimate_access(d, lba, now_ms, &ms)` - what SPTF needs: positioning
  time without changing state.
- `disk_seek(d, cyl, &timing)` - arm movement only (SCAN sweeps).
- `disk_lba_to_chs` / `disk_chs_to_lba` - translation, remap-aware.
- `disk_add_grown_defect(d, lba)`, `disk_is_remapped(d, lba)`.
- `disk_get_info`, `disk_get_zone`, `disk_strerror`.

### src/timing.c - the formulas

- `timing_rev_ms(rpm)` - 60000 / rpm.
- `timing_seek_ms(settle, k, d)` - 0 or settle + k sqrt(d).
- `timing_angle_at(t, T)` - fmod(t, T) / T.
- `timing_rot_wait_ms(t, target, T)` - wait until the target angle; values
  within 1e-7 revolution of "exactly on it" are snapped to 0 so floating-point
  noise never costs a whole revolution (`TIMING_ANGLE_EPS`).
- `timing_transfer_ms(n, spt, T)` - n / spt * T.
- `timing_sector_angle(s, spt, skew)` - where sector s starts.

Pure functions, no state: easy to test against hand calculations.

### src/geometry.c - zones and PBA <-> CHS

- `geometry_init(g, cfg)` - builds the zone table: equal-width bands, SPT
  interpolated linearly from outer to inner with integer math (exact at both
  ends), and a prefix sum giving each zone's first PBA.
- `geometry_zone_of_cyl(g, cyl)` - binary search over zone start cylinders.
- `geometry_spt(g, cyl)` - SPT for a cylinder.
- `geometry_cyl_start_pba(g, cyl)` - used to find where the spare area starts.
- `geometry_pba_to_chs` / `geometry_chs_to_pba` - binary search for the zone,
  then the classic divide/modulo inside it.

PBA (physical) vs LBA (logical): geometry knows only PBAs.

### src/defects.c - slipping and reallocation

- `defects_init(...)` - copies, sorts and de-duplicates the P-list; rejects
  entries in the spare area.
- `defects_plist_count_le(df, pba)` - upper-bound binary search: how many
  factory defects are at or below a PBA.
- `defects_slip(df, lba)` - **the key algorithm**. We want the smallest PBA
  `p` with `p = lba + count_le(p)`. Start at `p = lba`, repeatedly push it forward
  by the number of defects stepped over, until it stops moving. The function is
  monotone, so this fixed-point iteration converges to the smallest solution,
  which is never itself a defect. O(log P) per iteration, usually 1-2
  iterations.
- `defects_glist_find(df, lba)` - binary search on the G-list.
- `defects_lba_to_pba(df, lba, &remapped)` - G-list first, then slipping.
- `defects_pba_to_lba(df, pba, &lba)` - the inverse; returns `DISK_EDEFECT`
  for slipped sectors, retired grown-defect homes, and unused spares.
- `defects_add_grown(df, lba)` - sorted insert with `memmove`; if the LBA was
  already remapped (its spare failed), it gets a fresh spare.

### src/disk.c - the media layer

- `disk_config_default` - the reference drive, with comments on why each
  number is realistic.
- `disk_open` - validates config, builds geometry and defects, `goto fail`
  cleanup (the standard C pattern for releasing partially built objects).
- `run_extent(d, e, &t, acc)` (static) - seek, rotational wait, transfer for one
  per-track extent; moves the head; accumulates timing.
- `disk_service_request` - walks the request's LBAs, maps each to a CHS, and
  grows the current extent while sectors stay on the same track in increasing
  order; otherwise flushes it with `run_extent` and starts a new one. Slipped
  sectors inside an extent are just extra transfer time.
- `disk_estimate_access` - same math as the first extent, without moving the
  head.
- `disk_seek`, `disk_lba_cylinder`, `disk_head_cylinder`, info functions.

### src/scheduler.c - the policies

- `scheduler_pick(s, queue, n, disk, now)` - one `switch` on the policy.
  Returns either SERVE(index) or SWEEP(cylinder) (SCAN/C-SCAN going to an
  edge).
- `pick_ahead(...)` - nearest request in a direction; the head's own cylinder
  counts as ahead in both directions.
- `pick_oldest`, `earlier` - deterministic tie-breaking by arrival, then id.
- SPTF calls `disk_estimate_access` for every queued request: O(queue length)
  per decision, which is fine for NCQ depths (32).
- SPTF-aged checks the oldest request first, then falls through to SPTF.

### src/sim.c - the event loop

- `sim_run(d, reqs, n, cfg, &stats)` - the loop described in the README:
  admit arrivals (respecting queue depth), idle-skip to the next arrival when
  empty, otherwise pick, serve, advance the clock. Sweeps advance time too and
  re-check arrivals.
- `sim_inject_grown_defects(d, n, seed)` - n distinct random G-list entries.
- `sim_random_plist(cfg, n, seed, &out)` - n distinct random P-list PBAs in the
  data area (opens a defect-free twin disk to learn the data-area size).

### src/workload.c - requests

- `workload_build` - seq (wraps at the end), random (uniform), hot/cold (80% in
  the first 20% of LBAs), Poisson arrival gaps via `rng_exp`.
- `workload_load_trace` - CSV reader: optional header, `#` comments, R/W in any
  spelling, strict number parsing with `strtod`/`strtoull` + end-pointer checks,
  bounds checks against capacity, errors with file:line, stable sort by arrival.
- `workload_write_trace` - the inverse (round-trip tested).

### src/metrics.c, src/rng.c, src/main.c, tools/gen_trace.c

- `metrics_compute` - response = finish - arrival, wait = start - arrival;
  nearest-rank percentiles; IOPS and MB/s over the makespan.
- `rng.c` - splitmix64, uniform, exponential.
- `main.c` - hand-written argument parsing (no POSIX `getopt`), CSV append with
  header-if-empty.
- `gen_trace.c` - the synthetic multi-stream trace.

### tests/

`test.h` is a tiny harness (no framework): `CHECK`, `CHECK_EQ`, `CHECK_NEAR`, `CHECK_CHS`.
Each expected value in `test_timing.c`, `test_geometry.c`, `test_defects.c` and
`test_scheduler.c` has its hand calculation in a comment next to it. Read
those comments: they are the best interview prep in the repository.

---

## 3. Design choices and their alternatives

**Angle from the clock vs random rotational latency.** The usual textbook
shortcut is "add half a revolution". It is fine for estimating averages, but it
cannot model SPTF (which needs the real angle), zero-latency sequential
transfers, or a missed revolution. Cost of our choice: the caller must pass the
time in.

**Library owns no clock.** `disk_service_request` takes `now_ms`. Alternative:
the disk keeps its own clock. Rejected because a second project (a cache or
firmware model) has its own event loop; two clocks would drift.

**Static library + opaque handle.** Alternative: a header-only library or
exposing structs. The opaque handle means we can change `struct disk` freely.

**Extents per track.** Alternative: compute the whole request as one transfer.
That is wrong across track boundaries, slipped sectors and remapped sectors.
With extents, one rule ("new track or backwards on the same track = new
positioning") handles all of them.

**Sorted arrays for defect lists.** Alternatives: linked list (O(n) lookup),
hash map (no ordered "count below X" query, which slipping needs), balanced
tree (more code, no benefit at these sizes). Firmware favours compact sorted
tables for the same reasons.

**Single spare pool at the inner edge.** Simplest, and it makes the cost of
reallocation obvious. Alternative: spares at the end of each zone or each
cylinder (shorter detours; what many real drives do). Listed as future work;
you can say "I measured the worst case on purpose".

**Workload generated up front, sorted.** Alternative: generate on the fly.
Rejected: each scheduler must see identical input for a fair comparison.

**Schedulers as a pure pick function.** Alternative: each policy keeps its own
data structure (e.g. a sorted tree for SSTF). Faster at huge queue lengths, but
queues here are tens of requests and readability wins. Interview point: "for
NCQ depth 32, a linear scan is 32 comparisons; a tree would be premature".

**SPTF aging as a hard cutoff.** Simple to explain and to test. The bench shows
its failure mode (collapse to FCFS when the limit is below the typical wait
under load). Alternative: weighted cost `positioning - w * waited_time`, which
degrades gracefully. Listed as future work.

**Hand-written test harness.** Alternative: a framework (Unity, CMocka,
Check). Avoids a dependency; the project builds with only gcc and make.

**`-Wall -Wextra -Werror -pedantic -Wshadow ...` and sanitizers.** Warnings as
errors keep the code clean; ASan catches buffer overruns and use-after-free;
UBSan catches signed overflow, bad shifts, misaligned access. `make asan` also
runs full simulations, not just unit tests.

---

## 4. How to explain the project

### 60 seconds

> "I built disk-sim, a hard-drive simulator in C. It models the mechanics of a
> drive: zoned geometry, seek time as settle plus k times square root of
> distance, rotational latency computed from the platter's actual angle at the
> current simulated time, and transfer time. It also models defect management:
> factory defects are slipped and grown defects are reallocated to a spare
> area. On top of that I implemented eight schedulers, from FCFS and the
> elevator algorithms up to SPTF, which is what drives do with NCQ.
>
> The interesting results: SPTF cut average latency by 39% versus SSTF on random
> reads, and at queue depth 32 it delivered 2.36 times the IOPS of FCFS. 0.01%
> of grown defects cost sequential reads 42% of their throughput but didn't
> affect random reads. And I found that adding a too-aggressive starvation
> limit to SPTF made it collapse into FCFS under bursty load. It's a static
> library with a clean API, 434 unit-test checks against hand calculations,
> and it's clean under AddressSanitizer."

### 5 minutes

Use this structure; each part is about a minute.

1. **Why.** "I wanted to understand what actually limits a hard drive's
   performance, and what drive firmware does about it. Hard drives are
   mechanical, so most of a random I/O's time is moving the arm and waiting for
   the platter; the transfer itself is under 1% of the time."

2. **The model.** Geometry with zones, and why zones break the CHS formula
   (zone table). Seek = settle + k sqrt(d), and why square root (constant
   acceleration). Rotation from the clock rather than an average: "that one
   decision is what lets me model SPTF and zero-latency sequential reads".
   Requests split into per-track extents; cylinder skew.

3. **Defects.** P-list slipping vs G-list reallocation, and the trade-off:
   slipping keeps LBAs physically ordered but can only be done at format;
   reallocation is instant but costs a seek round trip. The measurement:
   -42.1% sequential throughput at 20,000 grown defects, -0.1% on random.

4. **Scheduling.** The families: FCFS; seek-only (SSTF, SCAN, LOOK, C-variants);
   positioning-aware (SPTF). Show the queue-depth result (77.8 -> 183.9 IOPS at
   depth 32) and explain NCQ: only the drive knows the angle. Then starvation:
   max wait as a metric, SSTF's 760 ms vs LOOK's 342 ms on hot/cold, and the
   aging experiment: 500 ms limit cut SPTF max wait from 729 to 505 ms for
   free; 250 ms on a bursty trace collapsed it to 2,076 ms average. "That taught
   me that starvation protection has to be tuned to the load; a hard cutoff is
   fragile."

5. **Engineering.** Static library with an opaque handle and a caller-supplied
   clock, so another project can use it as its media layer. Deterministic RNG
   so every benchmark reproduces bit-for-bit. Unit tests against hand
   calculations (show one: the 16.25 ms remapped read). `-Werror` and
   ASan/UBSan. Close with the limitations: "it's a model, not firmware: no
   caching, no error recovery, idealised seek curve", and what you would add
   next (write cache, per-zone spares, weighted aging).

---

## 5. Resume bullets

All numbers are from `results/results.md`. Pick one or two.

- Built a hard-disk I/O simulator in C11 (static library + CLI) modelling zoned
  geometry, sqrt-distance seeks, clock-driven rotational latency and P-list/G-list
  defect management; implemented 8 schedulers including SPTF, which cut average
  random-read latency 39% vs SSTF (58.8 vs 96.4 ms).

- Quantified NCQ-style rotational-position ordering: SPTF reached 183.9 random
  4 KiB IOPS at queue depth 32, 2.36x FCFS (77.8), across a 121-run reproducible
  benchmark with automated CSV/Markdown/chart reporting (Python, matplotlib).

- Modelled grown-defect reallocation to a spare area and measured its cost:
  20,000 remapped sectors (0.01% of LBAs) reduced sequential throughput 42.1%
  (77.55 to 44.94 MB/s) while random throughput changed by 0.1%.

- Analysed scheduler starvation via max-wait metrics; showed SPTF aging cut
  worst-case wait 31% (729 to 505 ms) at equal mean latency, and identified a
  failure mode where a too-short limit degrades SPTF to FCFS under bursty load
  (2,076 vs 67 ms). 434 unit-test checks; clean under -Werror, ASan and UBSan.

---

## 6. Exercises to make it yours

Doing even one of these will make your answers sound like experience rather
than memorisation.

1. Work through `remap_timing` in `tests/test_defects.c` by hand on paper.
2. Change `--spare-cyls` or move the spare area to the middle of the disk (edit
   `disk_open`) and re-run the defect sweep. How much of the -42% comes back?
3. Run `./build/disksim --workload random --rate 120 --sched sptf --qdepth 32`
   and compare with unlimited depth. Why is the latency different?
4. Implement weighted aging in `scheduler.c` (`positioning - w * wait`) and
   compare it against the hard cutoff on the trace.
5. Add a head-switch time to `run_extent` and see sequential throughput drop;
   then add track skew to recover it.
