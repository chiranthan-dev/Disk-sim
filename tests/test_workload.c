/*
 * Workload generators and the trace reader.
 */
#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "workload.h"

#define TMP_TRACE "disksim_test_trace.tmp.csv"

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs(text, f);
    fclose(f);
}

static void generators(void)
{
    workload_config_t wc;
    io_request_t *r = NULL;
    size_t n = 0;
    char err[256];
    const uint64_t cap = 1000000;

    /* Sequential: back-to-back LBAs, wrapping at the end of the disk. */
    workload_config_default(&wc);
    wc.kind = WL_SEQ;
    wc.num_requests = 10;
    wc.req_sectors = 128;
    wc.start_frac = 0.999;   /* starts at 999000: wraps after 7 requests */
    CHECK_EQ(workload_build(&wc, cap, &r, &n, err, sizeof err), 0);
    CHECK_EQ(n, 10);
    if (r) {
        CHECK_EQ(r[0].lba, 999000);
        CHECK_EQ(r[1].lba, 999128);
        CHECK_EQ(r[6].lba, 999768);   /* 999768 + 128 = 999896 <= cap */
        CHECK_EQ(r[7].lba, 0);        /* 999896 + 128 > cap -> wrap */
        CHECK_EQ(r[8].lba, 128);
    }
    free(r);

    /* Random: in range, deterministic for a seed, Poisson arrivals. */
    workload_config_default(&wc);
    wc.kind = WL_RANDOM;
    wc.num_requests = 20000;
    wc.rate_iops = 200.0;    /* mean gap 5 ms */
    CHECK_EQ(workload_build(&wc, cap, &r, &n, err, sizeof err), 0);
    io_request_t *r2 = NULL;
    size_t n2 = 0;
    workload_build(&wc, cap, &r2, &n2, err, sizeof err);
    int in_range = 1, same = 1, sorted = 1;
    for (size_t i = 0; r && r2 && i < n; i++) {
        if (r[i].lba + r[i].count > cap) in_range = 0;
        if (r[i].lba != r2[i].lba || r[i].arrival_ms != r2[i].arrival_ms) same = 0;
        if (i && r[i].arrival_ms < r[i - 1].arrival_ms) sorted = 0;
    }
    CHECK(in_range);
    CHECK(same);
    CHECK(sorted);
    if (r) {
        double mean_gap = r[n - 1].arrival_ms / (double)(n - 1);
        CHECK_NEAR(mean_gap, 5.0, 0.15);   /* ~3 sigma for 20k samples */
    }
    free(r);
    free(r2);

    /* Hot/cold: ~80% of requests in the first 20% of the LBA space. */
    workload_config_default(&wc);
    wc.kind = WL_HOTCOLD;
    wc.num_requests = 20000;
    CHECK_EQ(workload_build(&wc, cap, &r, &n, err, sizeof err), 0);
    size_t hot = 0;
    for (size_t i = 0; r && i < n; i++)
        if (r[i].lba < cap / 5)
            hot++;
    CHECK_NEAR((double)hot / (double)n, 0.80, 0.015);
    free(r);

    /* rate 0 = closed batch: everything arrives at t = 0 */
    workload_config_default(&wc);
    wc.rate_iops = 0.0;
    wc.num_requests = 5;
    workload_build(&wc, cap, &r, &n, err, sizeof err);
    CHECK(r && r[4].arrival_ms == 0.0);
    free(r);

    /* invalid */
    workload_config_default(&wc);
    wc.req_sectors = 0;
    CHECK_EQ(workload_build(&wc, cap, &r, &n, err, sizeof err), -1);
}

static void trace_reader(void)
{
    io_request_t *r = NULL;
    size_t n = 0;
    char err[256];

    /* Header, comment, blank line, mixed op spelling, out-of-order times. */
    write_file(TMP_TRACE,
               "arrival_time_ms,op,lba,length\n"
               "# a comment\n"
               "0.0, R, 100, 8\n"
               "\n"
               "5.5,write,2000,16\n"
               "2.25,r,50,1\n");
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), 0);
    CHECK_EQ(n, 3);
    if (r && n == 3) {
        CHECK_NEAR(r[0].arrival_ms, 0.0, 1e-12);
        CHECK_EQ(r[1].lba, 50);          /* sorted by arrival */
        CHECK_NEAR(r[1].arrival_ms, 2.25, 1e-12);
        CHECK_EQ(r[2].op, DISK_OP_WRITE);
        CHECK_EQ(r[2].count, 16);
        CHECK_EQ(r[2].id, 2);
    }

    /* Round trip through the writer. */
    if (r) {
        CHECK_EQ(workload_write_trace(TMP_TRACE, r, n), 0);
        io_request_t *back = NULL;
        size_t nb = 0;
        CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &back, &nb, err, sizeof err), 0);
        CHECK_EQ(nb, n);
        if (back && nb == n)
            CHECK(back[2].lba == r[2].lba && back[1].arrival_ms == r[1].arrival_ms);
        free(back);
    }
    free(r);
    r = NULL;

    /* Errors carry the line number. */
    write_file(TMP_TRACE, "0,R,1,1\n1,X,2,1\n");
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);
    CHECK(strstr(err, ":2:") != NULL);

    write_file(TMP_TRACE, "0,R,9995,10\n");          /* runs past capacity */
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);
    CHECK(strstr(err, "beyond capacity") != NULL);

    write_file(TMP_TRACE, "0,R,5,0\n");              /* zero length */
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);

    write_file(TMP_TRACE, "0,R,5\n");                /* missing field */
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);

    write_file(TMP_TRACE, "0,R,1,1\ngarbage,R,1,1\n"); /* non-number after data */
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);

    write_file(TMP_TRACE, "# only a comment\n");
    CHECK_EQ(workload_load_trace(TMP_TRACE, 10000, &r, &n, err, sizeof err), -1);

    CHECK_EQ(workload_load_trace("does/not/exist.csv", 10000, &r, &n, err, sizeof err), -1);
    remove(TMP_TRACE);

    workload_kind_t k;
    CHECK_EQ(workload_parse_kind("hotcold", &k), 0);
    CHECK_EQ(k, WL_HOTCOLD);
    CHECK_EQ(workload_parse_kind("zipf", &k), -1);
}

void suite_workload(void)
{
    generators();
    trace_reader();
}
