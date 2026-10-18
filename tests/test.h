/*
 * test.h - a deliberately tiny test harness.
 *
 * No framework dependency: a check macro that records file:line on failure
 * and keeps going, so one run reports every broken expectation.
 */
#ifndef DISKSIM_TEST_H
#define DISKSIM_TEST_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "disksim.h"

extern int g_checks;
extern int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        unsigned long long va_ = (unsigned long long)(a);                    \
        unsigned long long vb_ = (unsigned long long)(b);                    \
        g_checks++;                                                          \
        if (va_ != vb_) {                                                    \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s == %s (%llu vs %llu)\n",       \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                   \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                \
    do {                                                                     \
        double va_ = (a), vb_ = (b);                                         \
        g_checks++;                                                          \
        if (fabs(va_ - vb_) > (eps)) {                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s ~= %s (%.9f vs %.9f)\n",       \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                   \
        }                                                                    \
    } while (0)

#define CHECK_CHS(chs, c, h, s)                                              \
    do {                                                                     \
        CHECK_EQ((chs).cylinder, (c));                                       \
        CHECK_EQ((chs).head, (h));                                           \
        CHECK_EQ((chs).sector, (s));                                         \
    } while (0)

/*
 * A small drive with round numbers so expected timings can be worked out
 * by hand: 6000 rpm -> exactly 10 ms per revolution; seek = 1 + 0.5*sqrt(d).
 */
static inline void test_small_config(disk_config_t *cfg, uint32_t cyls,
                                     uint32_t heads, uint32_t spt)
{
    disk_config_default(cfg);
    cfg->cylinders = cyls;
    cfg->heads = heads;
    cfg->sectors_per_track = spt;
    cfg->zoned = 0;
    cfg->rpm = 6000;
    cfg->seek_settle_ms = 1.0;
    cfg->seek_k_ms = 0.5;
    cfg->cyl_skew = 0.0;
    cfg->spare_cylinders = 0;
}

void suite_geometry(void);
void suite_timing(void);
void suite_defects(void);
void suite_scheduler(void);
void suite_workload(void);
void suite_metrics(void);

#endif
