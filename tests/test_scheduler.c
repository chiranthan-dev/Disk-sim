/*
 * Scheduler tests.
 *
 * Drive: 100 cylinders, 1 head, 100 sectors/track, 10 ms/rev, so
 * LBA(c, s) = c*100 + s. The textbook queue below (head at 50, requests at
 * cylinders 70, 20, 55, 90, 45, all arriving at t=0) has a service order
 * and total head movement for each policy that is easy to verify by hand.
 */
#include <stdlib.h>

#include "test.h"
#include "scheduler.h"
#include "sim.h"

#define LBA(c, s) ((uint64_t)(c) * 100 + (s))

static disk_t *open_disk(void)
{
    disk_config_t cfg;
    test_small_config(&cfg, 100, 1, 100);
    return disk_open(&cfg, NULL);
}

static void make_req(io_request_t *r, uint64_t id, double arrival, uint64_t lba)
{
    memset(r, 0, sizeof(*r));
    r->id = id;
    r->arrival_ms = arrival;
    r->lba = lba;
    r->count = 1;
    r->op = DISK_OP_READ;
}

/* Run `policy` on the textbook queue; write the served cylinders in order
 * and return the head movement. */
static uint64_t run_textbook(sched_policy_t policy, size_t qdepth, uint32_t order[5])
{
    static const uint32_t cyls[5] = { 70, 20, 55, 90, 45 };
    io_request_t r[5];
    for (int i = 0; i < 5; i++)
        make_req(&r[i], (uint64_t)i, 0.0, LBA(cyls[i], 0));

    disk_t *d = open_disk();
    if (!d)
        return 0;
    disk_timing_t t;
    disk_seek(d, 50, &t);   /* start position, not counted */

    sim_config_t sc;
    sim_config_default(&sc);
    sc.policy = policy;
    sc.queue_depth = qdepth;
    sim_stats_t st;
    CHECK_EQ(sim_run(d, r, 5, &sc, &st), DISK_OK);

    /* sort indices by start time to recover service order */
    int idx[5] = { 0, 1, 2, 3, 4 };
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (r[idx[j]].start_ms < r[idx[i]].start_ms) {
                int tmp = idx[i];
                idx[i] = idx[j];
                idx[j] = tmp;
            }
    for (int i = 0; i < 5; i++)
        order[i] = cyls[idx[i]];
    disk_close(d);
    return st.head_movement;
}

#define CHECK_ORDER(o, a, b, c, d, e)                                         \
    do {                                                                      \
        CHECK_EQ((o)[0], a); CHECK_EQ((o)[1], b); CHECK_EQ((o)[2], c);        \
        CHECK_EQ((o)[3], d); CHECK_EQ((o)[4], e);                             \
    } while (0)

static void textbook_orders(void)
{
    uint32_t o[5];

    /* FCFS: arrival order. 20+50+35+35+45 = 185 */
    CHECK_EQ(run_textbook(SCHED_FCFS, 0, o), 185);
    CHECK_ORDER(o, 70, 20, 55, 90, 45);

    /* SSTF: 55(5) 45(10) then 20 and 70 are both 25 away -> tie goes to
     * the earlier arrival (70), then 90(20), 20(70). Total 130. */
    CHECK_EQ(run_textbook(SCHED_SSTF, 0, o), 130);
    CHECK_ORDER(o, 55, 45, 70, 90, 20);

    /* SCAN (inward first): 55 70 90, sweep to edge 99, reverse: 45 20.
     * (99-50) + (99-20) = 49 + 79 = 128 */
    CHECK_EQ(run_textbook(SCHED_SCAN, 0, o), 128);
    CHECK_ORDER(o, 55, 70, 90, 45, 20);

    /* LOOK: same order, but reverse at 90. 40 + 70 = 110 */
    CHECK_EQ(run_textbook(SCHED_LOOK, 0, o), 110);
    CHECK_ORDER(o, 55, 70, 90, 45, 20);

    /* C-SCAN: 55 70 90, to edge 99, fly back to 0, then 20 45.
     * 49 + 99 + 45 = 193 */
    CHECK_EQ(run_textbook(SCHED_CSCAN, 0, o), 193);
    CHECK_ORDER(o, 55, 70, 90, 20, 45);

    /* C-LOOK: 55 70 90, jump to lowest request 20, then 45.
     * 40 + 70 + 25 = 135 */
    CHECK_EQ(run_textbook(SCHED_CLOOK, 0, o), 135);
    CHECK_ORDER(o, 55, 70, 90, 20, 45);

    /* Queue depth 1: the drive only ever sees one command, so any policy
     * degenerates to FCFS. This is why NCQ depth matters. */
    CHECK_EQ(run_textbook(SCHED_SSTF, 1, o), 185);
    CHECK_ORDER(o, 70, 20, 55, 90, 45);
    run_textbook(SCHED_SPTF, 1, o);
    CHECK_ORDER(o, 70, 20, 55, 90, 45);
}

static void sweep_decisions(void)
{
    /* Direct calls: head at 90, only requests below it remain. */
    disk_t *d = open_disk();
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;
    disk_seek(d, 90, &t);
    io_request_t a, b;
    make_req(&a, 0, 0.0, LBA(20, 0));
    make_req(&b, 1, 0.0, LBA(45, 0));
    io_request_t *q[2] = { &a, &b };
    scheduler_t s;
    sched_decision_t dec;

    scheduler_init(&s, SCHED_SCAN, 0);
    dec = scheduler_pick(&s, q, 2, d, 0.0);
    CHECK_EQ(dec.action, SCHED_SWEEP);       /* SCAN goes to the edge first */
    CHECK_EQ(dec.sweep_to, 99);

    scheduler_init(&s, SCHED_LOOK, 0);
    dec = scheduler_pick(&s, q, 2, d, 0.0);
    CHECK_EQ(dec.action, SCHED_SERVE);       /* LOOK turns around at once */
    CHECK_EQ(dec.index, 1);                  /* 45 is nearest going down */
    CHECK_EQ(s.direction, -1);

    scheduler_init(&s, SCHED_CSCAN, 0);
    dec = scheduler_pick(&s, q, 2, d, 0.0);
    CHECK_EQ(dec.action, SCHED_SWEEP);
    CHECK_EQ(dec.sweep_to, 99);
    disk_seek(d, 99, &t);
    dec = scheduler_pick(&s, q, 2, d, 0.0);
    CHECK_EQ(dec.action, SCHED_SWEEP);       /* return trip */
    CHECK_EQ(dec.sweep_to, 0);

    disk_seek(d, 90, &t);
    scheduler_init(&s, SCHED_CLOOK, 0);
    dec = scheduler_pick(&s, q, 2, d, 0.0);
    CHECK_EQ(dec.action, SCHED_SERVE);
    CHECK_EQ(dec.index, 0);                  /* jumps to the lowest: 20 */
    disk_close(d);
}

static void sptf_uses_rotation(void)
{
    /* Head at cylinder 50, t = 0 (angle 0). Positioning times:
     *   A (50, s80): no seek, wait 0.80 rev           =  8.0 ms
     *   B (52, s10): seek 1+0.5*sqrt(2) = 1.707 ms -> angle .1707,
     *                sector 10 just passed: wait .9293 rev -> 11.0 ms
     *   C (60, s50): seek 1+0.5*sqrt(10) = 2.581 ms, then wait until
     *                angle .5                          =  5.0 ms
     * SSTF picks A (same cylinder). SPTF picks C: farther, but sooner. */
    disk_t *d = open_disk();
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;
    disk_seek(d, 50, &t);

    io_request_t a, b, c;
    make_req(&a, 0, 0.0, LBA(50, 80));
    make_req(&b, 1, 0.0, LBA(52, 10));
    make_req(&c, 2, 0.0, LBA(60, 50));
    io_request_t *q[3] = { &a, &b, &c };

    double pa, pb, pc;
    disk_estimate_access(d, a.lba, 0.0, &pa);
    disk_estimate_access(d, b.lba, 0.0, &pb);
    disk_estimate_access(d, c.lba, 0.0, &pc);
    CHECK_NEAR(pa, 8.0, 1e-9);
    CHECK_NEAR(pb, 11.0, 1e-9);
    CHECK_NEAR(pc, 5.0, 1e-9);

    scheduler_t s;
    scheduler_init(&s, SCHED_SSTF, 0);
    CHECK_EQ(scheduler_pick(&s, q, 3, d, 0.0).index, 0);
    scheduler_init(&s, SCHED_SPTF, 0);
    CHECK_EQ(scheduler_pick(&s, q, 3, d, 0.0).index, 2);

    /* Same queue 3 ms later (angle .3): A now costs 5.0 ms; C's seek ends
     * at angle .5581, just after sector 50 went by, so C costs 12.0 ms;
     * B costs 8.0 ms. SPTF switches to A: the best choice depends on WHEN
     * you ask, which only a rotation-aware scheduler can see. */
    CHECK_EQ(scheduler_pick(&s, q, 3, d, 3.0).index, 0);
    disk_close(d);
}

static void sptf_aging(void)
{
    /* Head at 50, now = 1000 ms (angle 0).
     *   X: arrived t=0,   cyl 99 sector 0: 4.5 ms seek + 5.5 ms wait = 10 ms
     *   Y: arrived t=900, cyl 50 sector 0: 0 ms (right under the head)
     * Pure SPTF always prefers Y. With a 500 ms age limit, X has waited
     * 1000 ms and is served first. */
    disk_t *d = open_disk();
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;
    disk_seek(d, 50, &t);
    io_request_t x, y;
    make_req(&x, 0, 0.0, LBA(99, 0));
    make_req(&y, 1, 900.0, LBA(50, 0));
    io_request_t *q[2] = { &x, &y };
    scheduler_t s;

    scheduler_init(&s, SCHED_SPTF, 0);
    CHECK_EQ(scheduler_pick(&s, q, 2, d, 1000.0).index, 1);
    scheduler_init(&s, SCHED_SPTF_AGED, 500.0);
    CHECK_EQ(scheduler_pick(&s, q, 2, d, 1000.0).index, 0);
    scheduler_init(&s, SCHED_SPTF_AGED, 5000.0);
    CHECK_EQ(scheduler_pick(&s, q, 2, d, 1000.0).index, 1);
    disk_close(d);
}

/* One far request at cylinder 99 followed by a steady stream close to the
 * head. Returns the far request's start time and whether it went last. */
static double starvation_run(sched_policy_t p, double age, int *served_last)
{
    enum { N = 51 };
    io_request_t *r = calloc(N, sizeof(*r));
    if (!r)
        return -1.0;
    make_req(&r[0], 0, 0.0, LBA(99, 0));
    for (int i = 1; i < N; i++)   /* one per ms, cylinder 1, varied sectors */
        make_req(&r[i], (uint64_t)i, (double)(i - 1), LBA(1, (i * 37) % 100));

    disk_t *d = open_disk();
    if (!d) {
        free(r);
        return -1.0;
    }
    sim_config_t sc;
    sim_config_default(&sc);
    sc.policy = p;
    sc.age_limit_ms = age;
    sim_stats_t st;
    sim_run(d, r, N, &sc, &st);

    *served_last = 1;
    for (int i = 1; i < N; i++)
        if (r[i].start_ms > r[0].start_ms)
            *served_last = 0;
    double start = r[0].start_ms;
    disk_close(d);
    free(r);
    return start;
}

static void starvation(void)
{
    int last;
    double s;

    /* FCFS serves the far request first. */
    s = starvation_run(SCHED_FCFS, 0, &last);
    CHECK_NEAR(s, 0.0, 1e-9);
    CHECK(!last);

    /* SSTF: nearby requests keep arriving faster than they are served, so
     * the far request waits until the stream is exhausted. */
    s = starvation_run(SCHED_SSTF, 0, &last);
    CHECK(last);
    CHECK(s > 49.0);

    /* LOOK starves it too: "arm stickiness". Requests on the head's own
     * cylinder count as ahead in the current direction, and new ones keep
     * arriving on cylinder 1, so the sweep never leaves cylinder 1 until
     * the stream ends. Elevators bound starvation only when requests do
     * not keep landing exactly where the arm already is. */
    s = starvation_run(SCHED_LOOK, 0, &last);
    CHECK(last);

    /* SPTF with aging: once the far request is older than 20 ms it is
     * served next, i.e. within 20 ms + one service time (< 12 ms). */
    s = starvation_run(SCHED_SPTF_AGED, 20.0, &last);
    CHECK(!last);
    CHECK(s < 32.0);
}

static void idle_gaps(void)
{
    /* Requests far apart in time never queue: each starts on arrival. */
    io_request_t r[3];
    make_req(&r[0], 0, 0.0, LBA(10, 0));
    make_req(&r[1], 1, 100.0, LBA(80, 0));
    make_req(&r[2], 2, 200.0, LBA(5, 0));
    disk_t *d = open_disk();
    CHECK(d != NULL);
    if (!d)
        return;
    sim_config_t sc;
    sim_config_default(&sc);
    sc.policy = SCHED_SPTF;
    sim_stats_t st;
    CHECK_EQ(sim_run(d, r, 3, &sc, &st), DISK_OK);
    CHECK_NEAR(r[0].start_ms, 0.0, 1e-9);
    CHECK_NEAR(r[1].start_ms, 100.0, 1e-9);
    CHECK_NEAR(r[2].start_ms, 200.0, 1e-9);
    CHECK_EQ(st.max_queue_len, 1);
    CHECK_EQ(st.head_movement, 10 + 70 + 75);
    disk_close(d);
}

static void names(void)
{
    sched_policy_t p;
    for (int i = 0; i < SCHED_COUNT; i++) {
        CHECK_EQ(scheduler_parse(scheduler_name((sched_policy_t)i), &p), 0);
        CHECK_EQ(p, i);
    }
    CHECK_EQ(scheduler_parse("elevator", &p), -1);
}

void suite_scheduler(void)
{
    textbook_orders();
    sweep_decisions();
    sptf_uses_rotation();
    sptf_aging();
    starvation();
    idle_gaps();
    names();
}
