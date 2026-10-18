#include "test.h"

int g_checks = 0;
int g_failures = 0;

typedef struct {
    const char *name;
    void (*fn)(void);
} suite_t;

int main(void)
{
    const suite_t suites[] = {
        { "geometry  (LBA <-> CHS, zones)", suite_geometry },
        { "timing    (seek / rotation / transfer)", suite_timing },
        { "defects   (P-list slip, G-list remap)", suite_defects },
        { "scheduler (all policies)", suite_scheduler },
        { "workload  (generators, trace CSV)", suite_workload },
        { "metrics   (percentiles, throughput)", suite_metrics },
    };

    for (size_t i = 0; i < sizeof suites / sizeof suites[0]; i++) {
        int before = g_failures, checks = g_checks;
        suites[i].fn();
        printf("%-42s %4d checks  %s\n", suites[i].name, g_checks - checks,
               g_failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
