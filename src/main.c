/*
 * main.c - command-line front end for the simulator.
 *
 * Argument parsing is hand-written (no getopt) so the program stays plain
 * C11 with no POSIX dependency.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disksim.h"
#include "metrics.h"
#include "scheduler.h"
#include "sim.h"
#include "workload.h"

static void usage(FILE *f)
{
    fprintf(f,
"usage: disksim [options]\n"
"\n"
"workload\n"
"  --workload seq|random|hotcold|trace   (default random)\n"
"  --trace FILE          CSV: arrival_time_ms,op,lba,length\n"
"  --requests N          number of requests (default 5000)\n"
"  --rate IOPS           mean Poisson arrival rate; 0 = all arrive at t=0 (default 100)\n"
"  --size SECTORS        request length (default 8)\n"
"  --start-frac F        seq: start at F * capacity (default 0)\n"
"  --write-frac F        fraction of writes (default 0)\n"
"  --seed N              RNG seed (default 42)\n"
"\n"
"scheduling\n"
"  --sched NAME          fcfs|sstf|scan|cscan|look|clook|sptf|sptf-aged (default fcfs)\n"
"  --age-ms MS           sptf-aged starvation limit (default 500)\n"
"  --qdepth N            drive queue depth, 0 = unlimited (default 0)\n"
"\n"
"defects\n"
"  --defects N           inject N random grown defects (G-list)\n"
"  --plist N             add N random factory defects (P-list, slipped)\n"
"  --defect-seed N       seed for defect placement (default 7)\n"
"\n"
"drive\n"
"  --cylinders N --heads N --spt N --sector-size 512|4096 --rpm N\n"
"  --zoned | --no-zoned  --zones N --spt-outer N --spt-inner N\n"
"  --settle MS --seek-k MS --skew FRAC --spare-cyls N\n"
"\n"
"output\n"
"  --csv FILE            append one result row (header written if file is new)\n"
"  --tag TEXT            label for the csv row (default 'run')\n"
"  --per-request FILE    write every request's timing as CSV\n"
"  --dump-trace FILE     write the generated workload as a trace CSV\n"
"  --info                print the drive geometry and exit\n"
"  --quiet               no human-readable summary\n");
}

/* Strict numeric parsing: the whole string must be a number. */
static int parse_u64(const char *s, unsigned long long *out)
{
    char *end;
    if (!s || !*s || *s == '-')
        return -1;
    *out = strtoull(s, &end, 10);
    return *end ? -1 : 0;
}

static int parse_dbl(const char *s, double *out)
{
    char *end;
    if (!s || !*s)
        return -1;
    *out = strtod(s, &end);
    return *end ? -1 : 0;
}

static int write_per_request(const char *path, const io_request_t *r, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "id,arrival_ms,start_ms,finish_ms,lba,count,op,seek_ms,rotate_ms,"
               "transfer_ms,cylinders_moved,extents,remapped_sectors\n");
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%llu,%.4f,%.4f,%.4f,%llu,%u,%c,%.4f,%.4f,%.4f,%llu,%u,%u\n",
                (unsigned long long)r[i].id, r[i].arrival_ms, r[i].start_ms,
                r[i].finish_ms, (unsigned long long)r[i].lba, r[i].count,
                r[i].op == DISK_OP_WRITE ? 'W' : 'R', r[i].timing.seek_ms,
                r[i].timing.rotate_ms, r[i].timing.transfer_ms,
                (unsigned long long)r[i].timing.cylinders_moved,
                r[i].timing.extents, r[i].timing.remapped_sectors);
    return fclose(f) == 0 ? 0 : -1;
}

static void print_info(const disk_t *d, const disk_config_t *cfg)
{
    disk_info_t in;
    disk_get_info(d, &in);
    printf("drive: %u cylinders x %u heads, %u rpm (%.3f ms/rev), %u-byte sectors\n",
           in.cylinders, in.heads, cfg->rpm, in.rev_ms, in.sector_size);
    printf("capacity: %llu LBAs = %.2f GB\n", (unsigned long long)in.capacity_lbas,
           (double)in.capacity_lbas * in.sector_size / 1e9);
    printf("seek: %.3f + %.4f*sqrt(d) ms -> 1 cyl %.3f ms, full stroke %.3f ms\n",
           cfg->seek_settle_ms, cfg->seek_k_ms,
           cfg->seek_settle_ms + cfg->seek_k_ms,
           cfg->seek_settle_ms + cfg->seek_k_ms * sqrt((double)(in.cylinders - 1)));
    printf("zones: %u\n", in.num_zones);
    for (uint32_t i = 0; i < in.num_zones; i++) {
        disk_zone_t z;
        disk_get_zone(d, i, &z);
        printf("  zone %2u: cyl %6u-%6u  spt %u  (%.1f MB/s media rate)\n", i,
               z.first_cylinder, z.first_cylinder + z.num_cylinders - 1,
               z.sectors_per_track,
               (double)z.sectors_per_track * in.sector_size / 1e6 / (in.rev_ms / 1000.0));
    }
    printf("spares: %llu sectors, P-list %zu, G-list %zu\n",
           (unsigned long long)in.spare_total, in.plist_len, in.glist_len);
}

int main(int argc, char **argv)
{
    disk_config_t cfg;
    workload_config_t wc;
    sim_config_t sc;
    disk_config_default(&cfg);
    workload_config_default(&wc);
    sim_config_default(&sc);

    unsigned long long grown = 0, plist_n = 0, defect_seed = 7;
    const char *csv_path = NULL, *per_req_path = NULL, *dump_path = NULL;
    const char *tag = "run";
    int quiet = 0, info_only = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        unsigned long long u = 0;
        double x = 0.0;
        int used = 1;   /* does this option consume a value? */
        int bad = 0;

#define OPT(name) (strcmp(a, name) == 0)
        if (OPT("--help") || OPT("-h")) { usage(stdout); return 0; }
        else if (OPT("--quiet")) { quiet = 1; used = 0; }
        else if (OPT("--info")) { info_only = 1; used = 0; }
        else if (OPT("--zoned")) { cfg.zoned = 1; used = 0; }
        else if (OPT("--no-zoned")) { cfg.zoned = 0; used = 0; }
        else if (!v) { fprintf(stderr, "disksim: %s needs a value (or unknown option)\n", a); return 2; }
        else if (OPT("--workload")) bad = workload_parse_kind(v, &wc.kind);
        else if (OPT("--trace")) { wc.trace_path = v; wc.kind = WL_TRACE; }
        else if (OPT("--requests")) { bad = parse_u64(v, &u) || u == 0; wc.num_requests = (size_t)u; }
        else if (OPT("--rate")) { bad = parse_dbl(v, &x) || x < 0; wc.rate_iops = x; }
        else if (OPT("--size")) { bad = parse_u64(v, &u) || u == 0 || u > UINT32_MAX; wc.req_sectors = (uint32_t)u; }
        else if (OPT("--start-frac")) { bad = parse_dbl(v, &x); wc.start_frac = x; }
        else if (OPT("--write-frac")) { bad = parse_dbl(v, &x); wc.write_frac = x; }
        else if (OPT("--seed")) { bad = parse_u64(v, &u); wc.seed = u; }
        else if (OPT("--sched")) bad = scheduler_parse(v, &sc.policy);
        else if (OPT("--age-ms")) { bad = parse_dbl(v, &x) || x < 0; sc.age_limit_ms = x; }
        else if (OPT("--qdepth")) { bad = parse_u64(v, &u); sc.queue_depth = (size_t)u; }
        else if (OPT("--defects")) { bad = parse_u64(v, &u) || u > UINT32_MAX; grown = u; }
        else if (OPT("--plist")) { bad = parse_u64(v, &u) || u > UINT32_MAX; plist_n = u; }
        else if (OPT("--defect-seed")) bad = parse_u64(v, &defect_seed);
        else if (OPT("--cylinders")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.cylinders = (uint32_t)u; }
        else if (OPT("--heads")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.heads = (uint32_t)u; }
        else if (OPT("--spt")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.sectors_per_track = (uint32_t)u; cfg.zoned = 0; }
        else if (OPT("--sector-size")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.sector_size = (uint32_t)u; }
        else if (OPT("--rpm")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.rpm = (uint32_t)u; }
        else if (OPT("--zones")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.num_zones = (uint32_t)u; }
        else if (OPT("--spt-outer")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.spt_outer = (uint32_t)u; }
        else if (OPT("--spt-inner")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.spt_inner = (uint32_t)u; }
        else if (OPT("--settle")) bad = parse_dbl(v, &cfg.seek_settle_ms);
        else if (OPT("--seek-k")) bad = parse_dbl(v, &cfg.seek_k_ms);
        else if (OPT("--skew")) bad = parse_dbl(v, &cfg.cyl_skew);
        else if (OPT("--spare-cyls")) { bad = parse_u64(v, &u) || u > UINT32_MAX; cfg.spare_cylinders = (uint32_t)u; }
        else if (OPT("--csv")) csv_path = v;
        else if (OPT("--tag")) tag = v;
        else if (OPT("--per-request")) per_req_path = v;
        else if (OPT("--dump-trace")) dump_path = v;
        else {
            fprintf(stderr, "disksim: unknown option %s\n", a);
            usage(stderr);
            return 2;
        }
#undef OPT
        if (bad) {
            fprintf(stderr, "disksim: bad value for %s: '%s'\n", a, v);
            return 2;
        }
        i += used;
    }

    /* --- build the drive --- */
    uint64_t *plist = NULL;
    int rc = sim_random_plist(&cfg, (uint32_t)plist_n, defect_seed ^ 0x5157ULL, &plist);
    if (rc != DISK_OK) {
        fprintf(stderr, "disksim: P-list: %s\n", disk_strerror(rc));
        return 1;
    }
    cfg.plist = plist;
    cfg.plist_len = (size_t)plist_n;
    disk_t *d = disk_open(&cfg, &rc);
    free(plist);
    if (!d) {
        fprintf(stderr, "disksim: cannot open disk: %s\n", disk_strerror(rc));
        return 1;
    }
    if (grown) {
        rc = sim_inject_grown_defects(d, (uint32_t)grown, defect_seed);
        if (rc != DISK_OK) {
            fprintf(stderr, "disksim: injecting %llu grown defects: %s\n",
                    grown, disk_strerror(rc));
            disk_close(d);
            return 1;
        }
    }
    if (info_only) {
        print_info(d, &cfg);
        disk_close(d);
        return 0;
    }

    disk_info_t info;
    disk_get_info(d, &info);

    /* --- build the workload --- */
    io_request_t *reqs = NULL;
    size_t n = 0;
    char err[256];
    if (workload_build(&wc, info.capacity_lbas, &reqs, &n, err, sizeof err) != 0) {
        fprintf(stderr, "disksim: %s\n", err);
        disk_close(d);
        return 1;
    }
    if (dump_path && workload_write_trace(dump_path, reqs, n) != 0)
        fprintf(stderr, "disksim: cannot write %s\n", dump_path);

    /* --- run --- */
    sim_stats_t st;
    rc = sim_run(d, reqs, n, &sc, &st);
    if (rc != DISK_OK) {
        fprintf(stderr, "disksim: simulation failed: %s\n", disk_strerror(rc));
        free(reqs);
        disk_close(d);
        return 1;
    }

    metrics_t m;
    if (metrics_compute(reqs, n, info.sector_size, &st, &m) != 0) {
        fprintf(stderr, "disksim: out of memory\n");
        free(reqs);
        disk_close(d);
        return 1;
    }

    const char *wl_name = workload_kind_name(wc.kind);
    if (!quiet) {
        printf("disksim: workload=%s sched=%s requests=%zu rate=%.1f IOPS qdepth=%zu "
               "grown_defects=%llu plist=%llu\n",
               wl_name, scheduler_name(sc.policy), n, wc.rate_iops, sc.queue_depth,
               grown, plist_n);
        metrics_print(stdout, &m);
    }

    if (csv_path) {
        /* Write the header only if the file is new/empty, so many runs can
         * append to one results file. */
        FILE *probe = fopen(csv_path, "r");
        int need_header = 1;
        if (probe) {
            need_header = fgetc(probe) == EOF;
            fclose(probe);
        }
        FILE *f = fopen(csv_path, "a");
        if (!f) {
            fprintf(stderr, "disksim: cannot open %s\n", csv_path);
        } else {
            if (need_header)
                metrics_csv_header(f);
            metrics_csv_row(f, tag, wl_name, scheduler_name(sc.policy), (uint32_t)grown,
                            (uint32_t)plist_n, sc.queue_depth, wc.rate_iops, &m);
            fclose(f);
        }
    }
    if (per_req_path && write_per_request(per_req_path, reqs, n) != 0)
        fprintf(stderr, "disksim: cannot write %s\n", per_req_path);

    free(reqs);
    disk_close(d);
    return 0;
}
