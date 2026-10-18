# INTERVIEW.md - 25 questions about disk-sim, with model answers

Answers use numbers measured by `make bench` (see `results/results.md`). Speak
them in your own words; the goal is to understand, not to recite.

Sections: [HDD internals](#hdd-internals) (1-8) · [Scheduling](#scheduling-trade-offs) (9-15) ·
[Defect management](#defect-management) (16-19) · [C and engineering](#c-and-engineering-details) (20-25)

---

## HDD internals

### 1. What are the components of a disk access time, and which dominates?

Seek time (moving the arm to the cylinder), rotational latency (waiting for the
sector to come under the head) and transfer time (reading the sectors). For a
random 4 KiB read on my default 7200 rpm drive under FCFS, the averages were
8.86 ms seek, 4.19 ms rotation and 0.066 ms transfer. So positioning is over
99% of the time. For a large sequential read it flips: seeks and rotation
almost disappear and transfer dominates. That is why HDD optimisation is all
about avoiding or overlapping positioning.

### 2. Why do you model seek time as settle + k·sqrt(distance)?

The voice-coil actuator accelerates for the first half of the move and
decelerates for the second. Under constant acceleration, distance is
proportional to time squared, so time is proportional to the square root of
distance. The settle term is the fixed time for the servo to stop the arm
ringing and lock onto the track; it dominates short seeks. With 0.8 ms settle
and k = 0.068, I get 0.87 ms track-to-track and 16.0 ms full stroke, which are
realistic. Real long seeks hit the arm's maximum velocity and become closer to
linear, and real firmware uses a measured seek table; I list that as a
limitation.

### 3. How do you compute rotational latency?

From the platter's angle at the current simulated time: angle = (t mod T) / T,
with T = 60000 / rpm ms. After the seek, the wait is the angular distance from
the head to the target sector's start, times T. I deliberately did not use "half
a revolution on average", because then I could not model SPTF (it needs the
real angle), the zero wait of back-to-back sequential requests, or a missed
revolution. One subtlety: floating point. If the head is exactly on a sector
boundary, rounding can put it a hair past, which would cost a full revolution,
so I treat anything within 1e-7 of a revolution as zero.

### 4. What is zoned bit recording and how did it affect your mapping code?

Outer tracks are longer, so drives group cylinders into zones and give outer
zones more sectors per track. That breaks the classic formula LBA = (c·H + h)·S
+ s, since S depends on the cylinder. I keep a zone table with each zone's first
cylinder, SPT and first physical block, and binary-search it. A visible effect:
sequential throughput was 77.56 MB/s at the outer edge and 41.49 MB/s at the
inner edge, matching the 1300:700 sectors-per-track ratio.

### 5. What is the difference between a PBA and an LBA in your model?

A PBA numbers every physical sector, including defective ones and the spare
area. An LBA is what the host sees: PBAs with factory defects slipped out and
the spare area hidden, plus any reallocated LBAs redirected to spares. The
geometry module only knows PBA <-> CHS; the defect module does LBA <-> PBA. That
separation keeps each piece testable.

### 6. What is cylinder skew and why did you add it?

When a sequential transfer finishes the last track of a cylinder, the arm
needs a one-cylinder seek (0.87 ms, about 0.104 of a revolution). If sector 0
of the next cylinder were at the same angle, it would already have passed, and
we would wait almost a full revolution. Skew offsets sector 0 on each cylinder
by 0.12 revolution so it arrives just after the seek. Without it, my sequential
throughput fell from 77.55 to 63.91 MB/s, 17.6% lower. Real drives also use
track skew for head switches; I model head switches as free, which is a
limitation.

### 7. Why do sequential reads get zero rotational latency in your model?

Because the angle is derived from time. When the previous request finishes, the
head is exactly at the end of the last sector it read, which is the start of the
next LBA. If the next request is issued at that instant, the target angle equals
the current angle, so the wait is zero. A unit test checks exactly this (a
1.0 ms request that is pure transfer).

### 8. What is a 4Kn / Advanced Format drive and does your simulator support it?

Advanced Format drives use 4096-byte physical sectors instead of 512, which
improves format efficiency and allows stronger ECC per sector. My config accepts
512 or 4096-byte sectors; capacity and MB/s calculations use the configured size.
I did not model 512e (4K physical, 512 logical) read-modify-write behaviour,
which is a source of misaligned-write penalties on real drives.

---

## Scheduling trade-offs

### 9. Compare SSTF, SCAN and LOOK.

SSTF serves the nearest cylinder: short seeks, but greedy, so it can starve far
requests. SCAN sweeps in one direction to the edge of the disk, then reverses;
waits are bounded but it wastes travel to the edge and serves the middle
cylinders twice per cycle. LOOK reverses at the last pending request instead of
the edge. On random reads at 120 IOPS, SSTF averaged 96.4 ms, LOOK 97.0 ms and
SCAN 108 ms, while worst waits were 599, 454 and 513 ms. LOOK gave nearly
SSTF's mean with a much better tail.

### 10. Why have C-SCAN or C-LOOK at all?

Plain SCAN/LOOK serve the middle of the disk on both passes and the edges only
once, so requests at the edges wait longer. The circular versions serve only on
the inward pass and then return, which gives every cylinder the same treatment.
C-LOOK had the lowest maximum wait among the seek-only schedulers on random
reads: 383 ms vs 454 ms for LOOK, at a slightly higher mean (106 vs 97.0 ms).

### 11. What is SPTF and why is it better than SSTF?

Shortest Positioning Time First picks the request with the smallest seek plus
rotational latency, not just the nearest cylinder. A request two cylinders away
whose sector is about to arrive beats a request on the same cylinder that just
went by. My unit test has exactly that case: SSTF picks the same-cylinder
request (8 ms away), SPTF picks one ten cylinders away that is reachable in
5 ms. On random reads SPTF averaged 58.8 ms vs 96.4 ms for SSTF, 39% lower,
while moving the arm almost twice as far per request (6,911 vs 3,544
cylinders). The goal is minimum time, not minimum arm travel.

### 12. If SPTF is so good, why doesn't the operating system do it?

Because the OS does not know the platter's angular position, the drive's
internal geometry (zones, skew, defect remaps), or exact seek curves. Only the
drive firmware does. That is why NCQ (SATA) and TCQ (SCSI) let the host queue
many commands and leave the ordering to the drive, which uses rotational
position ordering. OS elevators still help by merging and roughly sorting, and
for SSDs the OS often uses a no-op or deadline scheduler.

### 13. How does queue depth affect throughput?

With only one outstanding command there is nothing to reorder, so every policy
equals FCFS: 77.8 IOPS in my sweep. With depth 32 (the SATA NCQ limit), SPTF
reached 183.9 IOPS (2.36x) and SSTF/LOOK about 145-146; at depth 128 SPTF hit
267.2. The deeper the queue, the more likely some request is both close and
about to rotate under the head. The cost is latency: each request waits behind
more others, so you trade response time for throughput.

### 14. What is starvation, how did you measure it, and how do you prevent it?

A request starves when greedy policies keep choosing newer, cheaper requests
over it. I measure maximum wait time (start minus arrival), not just averages,
because averages hide it. SSTF's worst wait on the hot/cold workload was
760 ms vs 342 ms for LOOK. A unit test builds a stream of requests on one
cylinder and shows SSTF and even LOOK leaving a far request for last ("arm
stickiness"). Prevention: aging. My SPTF+aging serves the oldest request first
once it exceeds a limit. With 500 ms, SPTF's max wait on random reads dropped
from 729 to 505 ms with the same 58.8 ms mean.

### 15. Was there any surprising result?

Yes: a too-short aging limit made things far worse. With 250 ms on the bursty
trace, bursts pushed many requests past the limit at once, so the scheduler kept
serving the oldest request, which is FCFS. FCFS cannot keep up with the trace,
so even more requests aged, and it never recovered: average response 2,076 ms
vs 67.0 ms for plain SPTF. At 500 ms it behaved well. Lesson: a hard cutoff has
a cliff; it must sit above the typical wait under load, or better, use a
weighted cost that blends positioning time and waiting time. Another smaller
surprise: greedy SPTF was 0.4% slower than FCFS on a pure sequential stream,
because near cylinder boundaries it occasionally grabs a request that is a
fraction of a millisecond cheaper right now and breaks the run.

---

## Defect management

### 16. What is the difference between the P-list and the G-list?

The P-list (primary) holds defects found at the factory. They are slipped: LBAs
are simply assigned to the next good sector, so LBA order still equals physical
order and a defect costs one sector-time. The G-list (grown) holds sectors that
fail in the field. Those are reallocated: the LBA is redirected to a spare
sector, and every access to it pays a seek to the spare area and back.

### 17. Why not slip grown defects too?

Slipping shifts the physical location of every following LBA, which means
moving all the user data after the defect. That is only acceptable during a
format, when there is no user data. Reallocation touches one LBA, so it can be
done online, instantly, with one table entry.

### 18. How big was the performance impact of grown defects?

With 20,000 grown defects (0.01% of all LBAs) and spares on the innermost
cylinders, sequential 64 KiB reads fell from 77.55 to 44.94 MB/s (-42.1%), and
with 100,000 to 19.62 MB/s. Random 4 KiB reads changed by only -0.1%, because a
5000-request random run touched just 2 remapped sectors while the sequential
run touched 76. One unit test shows a single remap turning a 6.25 ms read into
16.25 ms. Real drives reduce this by placing spares in each zone, close to the
data.

### 19. How did you implement LBA-to-PBA translation with slipped sectors efficiently?

The P-list is a sorted array. For an LBA I need the smallest PBA p with p = lba +
(number of defects <= p). I start with p = lba and repeatedly set p = lba +
count(defects <= p), using a binary search for the count, until it stops
changing. The function is monotone, so it converges to the smallest solution,
usually in one or two steps: O(log P). The reverse direction is pba - count
(defects below), unless the PBA is itself a defect. The G-list is checked first
with another binary search. I verify both directions exhaustively in tests,
including a disk with 50 random factory defects on 144 sectors.

---

## C and engineering details

### 20. Why is the disk an opaque type, and why does the caller pass in the time?

`disk_t` is declared in the header but defined only in `disk.c`, so a linking
project can only use the API; I can change the internals without breaking it.
The caller passes `now_ms` because the library should not own a clock: my
simulator, a unit test and another project's event loop each have their own
notion of time. Since rotational latency depends on time, passing it in also
makes `disk_estimate_access` a pure function, which SPTF calls for every queued
request.

### 21. How do you handle errors and cleanup in C?

Every public function returns an int status (`DISK_OK` or a negative
`DISK_E*`), and results come back through out-pointers; `disk_strerror` turns a
code into text. In `disk_open` and the trace parser I use the `goto fail` /
`goto done` pattern: one cleanup block that frees whatever was allocated so far.
It avoids duplicated cleanup code and leaks on early returns. ASan confirms no
leaks or overruns on the full simulation paths.

### 22. What do -Wall -Wextra -Werror and the sanitizers buy you?

Warnings as errors force me to fix implicit conversions, unused variables,
missing prototypes, shadowed names and switch fall-through bugs before they
ship. AddressSanitizer catches buffer overflows, use-after-free and leaks at
runtime; UndefinedBehaviorSanitizer catches signed overflow, invalid shifts and
misaligned access. `make asan` runs all unit tests plus five full simulations
(sequential with defects, SCAN sweeps, NCQ queue depth, the trace with remaps,
and a 4096-byte-sector drive). All clean.

### 23. How did you make the results reproducible?

Three things. I use my own PRNG (splitmix64) instead of `rand()`, whose
algorithm differs between C libraries. All workloads are generated up front
from a fixed seed and sorted by arrival, so every scheduler sees the identical
request stream. And the bench script fixes every seed, so `make bench`
reproduces the same CSV bit for bit. Every number in the README comes from that
CSV.

### 24. How did you test the timing model?

Against hand calculations on a drive with round numbers: 6000 rpm gives exactly
10 ms per revolution, and seek = 1 + 0.5 sqrt(d). For example, a 10-sector
read 9 cylinders away at t = 0: seek 1 + 0.5·3 = 2.5 ms, the platter is then at
angle 0.25, the sector is at 0.5, so 2.5 ms rotation, plus 1.0 ms transfer =
6.0 ms. The calculation is written in a comment next to each test. Other tests
cover track and cylinder crossings with and without skew, a missed revolution,
zone transfer rates, P-list slip cost and the remap round trip. The scheduler
tests use hand-computed service orders and head movement for every policy.
434 checks in total.

### 25. What are the limitations, and what would you add next?

It is a teaching model, not firmware. No write cache or read-ahead, reads and
writes cost the same, head switches are free, the seek curve is idealised, one
spare pool at the inner edge, no error recovery, and the sample trace is
synthetic. Next I would add a write-back cache with destaging (which is where
real firmware spends a lot of its scheduling effort), per-zone spare areas to
compare with my single pool, weighted SPTF aging to fix the cliff I found, and
support for real block traces. The library is already structured so a
firmware-level project can link it as its media layer.
