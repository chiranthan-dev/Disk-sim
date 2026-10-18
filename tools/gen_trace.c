/*
 * gen_trace.c - write a synthetic multi-stream trace in the CSV format the
 * simulator replays (arrival_time_ms,op,lba,length).
 *
 * The built-in generators each test one thing in isolation. Real traces
 * mix patterns, so this produces something closer to a small file server
 * or a DVR: several concurrent sequential streams, small metadata I/O to a
 * hot region, some random reads, and bursty (on/off) arrivals. It is
 * SYNTHETIC, generated from a fixed seed; it is not a captured trace.
 *
 *   usage: gen_trace OUT.csv [requests] [seed]
 */
#include <stdio.h>
#include <stdlib.h>

#include "disksim.h"
#include "rng.h"
#include "workload.h"

#define STREAMS 4

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUT.csv [requests] [seed]\n", argv[0]);
        return 2;
    }
    char *end = NULL;
    size_t n = 5000;
    uint64_t seed = 2024;
    if (argc > 2) {
        n = (size_t)strtoull(argv[2], &end, 10);
        if (*end || n == 0) {
            fprintf(stderr, "gen_trace: bad request count '%s'\n", argv[2]);
            return 2;
        }
    }
    if (argc > 3) {
        seed = strtoull(argv[3], &end, 10);
        if (*end) {
            fprintf(stderr, "gen_trace: bad seed '%s'\n", argv[3]);
            return 2;
        }
    }

    /* Generate LBAs that fit the default drive. */
    disk_config_t cfg;
    disk_config_default(&cfg);
    disk_t *d = disk_open(&cfg, NULL);
    if (!d)
        return 1;
    disk_info_t info;
    disk_get_info(d, &info);
    disk_close(d);
    uint64_t cap = info.capacity_lbas;

    rng_t rng;
    rng_seed(&rng, seed);

    io_request_t *r = calloc(n, sizeof(*r));
    if (!r)
        return 1;

    /* Each stream reads its own file: a contiguous region somewhere on the
     * disk, 64 KiB at a time. */
    uint64_t stream_pos[STREAMS];
    for (int s = 0; s < STREAMS; s++)
        stream_pos[s] = rng_below(&rng, cap - 4000000);

    const uint64_t meta_end = cap / 100;   /* hot metadata: first 1% */
    double t = 0.0;
    double burst_end = rng_exp(&rng, 50.0);

    for (size_t i = 0; i < n; i++) {
        /* On/off arrivals: 400 IOPS inside a burst (~50 ms), then idle
         * (~100 ms). Bursts are what fill queues in real systems. */
        t += rng_exp(&rng, 1000.0 / 400.0);
        if (t > burst_end) {
            t += rng_exp(&rng, 100.0);
            burst_end = t + rng_exp(&rng, 50.0);
        }

        double u = rng_unit(&rng);
        r[i].id = i;
        r[i].arrival_ms = t;
        if (u < 0.60) {                      /* sequential stream chunk */
            int s = (int)rng_below(&rng, STREAMS);
            if (stream_pos[s] + 128 > cap)   /* long traces: wrap the file */
                stream_pos[s] = 0;
            r[i].lba = stream_pos[s];
            r[i].count = 128;
            r[i].op = DISK_OP_READ;
            stream_pos[s] += 128;
        } else if (u < 0.90) {               /* metadata: small, hot, 50% writes */
            r[i].lba = rng_below(&rng, meta_end - 8);
            r[i].count = 8;
            r[i].op = rng_unit(&rng) < 0.5 ? DISK_OP_WRITE : DISK_OP_READ;
        } else {                             /* random 4 KiB anywhere */
            r[i].lba = rng_below(&rng, cap - 8);
            r[i].count = 8;
            r[i].op = DISK_OP_READ;
        }
    }

    int rc = workload_write_trace(argv[1], r, n);
    free(r);
    if (rc != 0) {
        fprintf(stderr, "gen_trace: cannot write %s\n", argv[1]);
        return 1;
    }
    printf("gen_trace: wrote %zu requests (%.1f s of activity) to %s\n",
           n, t / 1000.0, argv[1]);
    return 0;
}
