/*
 * Metrics: percentile definition and the throughput arithmetic.
 */
#include "test.h"
#include "metrics.h"

static void percentiles(void)
{
    double v[100];
    for (int i = 0; i < 100; i++)
        v[i] = i + 1;   /* 1..100 */
    CHECK_NEAR(metrics_percentile(v, 100, 0.50), 50, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 100, 0.95), 95, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 100, 0.99), 99, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 100, 1.00), 100, 1e-12);
    /* nearest rank with 10 samples: p95 -> rank ceil(9.5) = 10 = max */
    CHECK_NEAR(metrics_percentile(v, 10, 0.95), 10, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 10, 0.50), 5, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 1, 0.99), 1, 1e-12);
    CHECK_NEAR(metrics_percentile(v, 0, 0.99), 0, 1e-12);
}

static void compute(void)
{
    /* Two 8-sector (4096 B) requests:
     *   A: arrive 0,  start 0,  finish 10   -> response 10, wait 0
     *   B: arrive 2,  start 10, finish 20   -> response 18, wait 8
     * makespan 20 ms -> 100 IOPS, 8192 B / 0.02 s = 0.4096 MB/s */
    io_request_t r[2];
    memset(r, 0, sizeof r);
    r[0].arrival_ms = 0;  r[0].start_ms = 0;  r[0].finish_ms = 10; r[0].count = 8;
    r[0].timing.seek_ms = 4; r[0].timing.rotate_ms = 5; r[0].timing.transfer_ms = 1;
    r[1].arrival_ms = 2;  r[1].start_ms = 10; r[1].finish_ms = 20; r[1].count = 8;
    r[1].timing.seek_ms = 6; r[1].timing.rotate_ms = 3; r[1].timing.transfer_ms = 1;
    sim_stats_t st;
    memset(&st, 0, sizeof st);
    st.head_movement = 300;

    metrics_t m;
    CHECK_EQ(metrics_compute(r, 2, 512, &st, &m), 0);
    CHECK_NEAR(m.makespan_ms, 20.0, 1e-12);
    CHECK_NEAR(m.iops, 100.0, 1e-9);
    CHECK_NEAR(m.mb_per_s, 0.4096, 1e-12);
    CHECK_NEAR(m.avg_response_ms, 14.0, 1e-12);
    CHECK_NEAR(m.p99_response_ms, 18.0, 1e-12);
    CHECK_NEAR(m.max_wait_ms, 8.0, 1e-12);
    CHECK_NEAR(m.avg_wait_ms, 4.0, 1e-12);
    CHECK_NEAR(m.avg_seek_ms, 5.0, 1e-12);
    CHECK_NEAR(m.avg_rotate_ms, 4.0, 1e-12);
    CHECK_NEAR(m.avg_service_ms, 10.0, 1e-12);
    CHECK_EQ(m.head_movement, 300);
    CHECK_NEAR(m.avg_seek_distance, 150.0, 1e-12);
}

void suite_metrics(void)
{
    percentiles();
    compute();
}
