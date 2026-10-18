/*
 * Timing tests against hand calculations.
 *
 * Reference drive (test_small_config): 6000 rpm -> T = 10 ms per rev,
 * seek(d) = 1 + 0.5*sqrt(d) ms, 100 sectors per track -> 0.1 ms per sector.
 * Platter angle at time t = (t mod 10) / 10.
 */
#include "test.h"
#include "timing.h"

#define EPS 1e-9

static void formulas(void)
{
    CHECK_NEAR(timing_rev_ms(7200), 8.333333333, 1e-6);
    CHECK_NEAR(timing_rev_ms(6000), 10.0, EPS);
    CHECK_NEAR(timing_rev_ms(15000), 4.0, EPS);

    CHECK_NEAR(timing_seek_ms(1.0, 0.5, 0), 0.0, EPS);     /* no move, no settle */
    CHECK_NEAR(timing_seek_ms(1.0, 0.5, 1), 1.5, EPS);
    CHECK_NEAR(timing_seek_ms(1.0, 0.5, 100), 6.0, EPS);   /* 1 + 0.5*10 */
    CHECK_NEAR(timing_seek_ms(1.0, 0.5, 10000), 51.0, EPS);
    /* default drive: 0.8 + 0.068*sqrt(49999) ~= 16.005 ms full stroke */
    CHECK_NEAR(timing_seek_ms(0.8, 0.068, 49999), 16.0049, 1e-3);

    CHECK_NEAR(timing_angle_at(0.0, 10.0), 0.0, EPS);
    CHECK_NEAR(timing_angle_at(2.5, 10.0), 0.25, EPS);
    CHECK_NEAR(timing_angle_at(12.5, 10.0), 0.25, EPS);
    CHECK_NEAR(timing_angle_at(1e6 + 7.5, 10.0), 0.75, 1e-9);

    CHECK_NEAR(timing_rot_wait_ms(0.0, 0.5, 10.0), 5.0, EPS);   /* half a rev ahead */
    CHECK_NEAR(timing_rot_wait_ms(6.0, 0.5, 10.0), 9.0, EPS);   /* just missed it   */
    CHECK_NEAR(timing_rot_wait_ms(5.0, 0.5, 10.0), 0.0, EPS);   /* exactly on it    */
    CHECK_NEAR(timing_rot_wait_ms(10.0, 0.0, 10.0), 0.0, EPS);  /* wrap boundary    */
    /* float noise just past the target must not cost a full revolution */
    CHECK_NEAR(timing_rot_wait_ms(5.0 + 1e-12, 0.5, 10.0), 0.0, 1e-6);

    CHECK_NEAR(timing_transfer_ms(8, 100, 10.0), 0.8, EPS);
    CHECK_NEAR(timing_transfer_ms(100, 100, 10.0), 10.0, EPS);  /* one full track */

    CHECK_NEAR(timing_sector_angle(25, 100, 0.0), 0.25, EPS);
    CHECK_NEAR(timing_sector_angle(90, 100, 0.2), 0.10, EPS);   /* wraps past 1 */
}

static disk_t *open_small(double skew)
{
    disk_config_t cfg;
    test_small_config(&cfg, 100, 2, 100);
    cfg.cyl_skew = skew;
    return disk_open(&cfg, NULL);
}

static void single_request(void)
{
    disk_t *d = open_small(0.0);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;

    /* Request at (cyl 9, head 0, sector 50), 10 sectors, issued at t = 0
     * with the head on cylinder 0:
     *   seek     = 1 + 0.5*sqrt(9) = 2.5 ms        -> t = 2.5, angle 0.25
     *   rotation = (0.50 - 0.25) * 10 = 2.5 ms      -> t = 5.0
     *   transfer = 10/100 * 10 = 1.0 ms             -> t = 6.0
     *   total    = 6.0 ms */
    disk_request_t r = { (9 * 2 + 0) * 100 + 50, 10, DISK_OP_READ };
    CHECK_EQ(disk_service_request(d, &r, 0.0, &t), DISK_OK);
    CHECK_NEAR(t.seek_ms, 2.5, EPS);
    CHECK_NEAR(t.rotate_ms, 2.5, EPS);
    CHECK_NEAR(t.transfer_ms, 1.0, EPS);
    CHECK_NEAR(t.total_ms, 6.0, EPS);
    CHECK_EQ(t.cylinders_moved, 9);
    CHECK_EQ(t.extents, 1);
    CHECK_EQ(disk_head_cylinder(d), 9);

    /* The very next sectors, issued the instant the last one finished:
     * no seek, sector 60 is right under the head -> pure transfer. This is
     * why sequential I/O is fast. */
    r.lba = (9 * 2 + 0) * 100 + 60;
    disk_service_request(d, &r, 6.0, &t);
    CHECK_NEAR(t.seek_ms, 0.0, EPS);
    CHECK_NEAR(t.rotate_ms, 0.0, EPS);
    CHECK_NEAR(t.total_ms, 1.0, EPS);

    /* Sector 55 at t = 7.0 (angle 0.70): it went by 1.5 ms ago, so we wait
     * (0.55 - 0.70 + 1) * 10 = 8.5 ms - almost a full revolution. */
    r.lba = (9 * 2 + 0) * 100 + 55;
    r.count = 1;
    disk_service_request(d, &r, 7.0, &t);
    CHECK_NEAR(t.rotate_ms, 8.5, EPS);
    CHECK_NEAR(t.total_ms, 8.6, EPS);

    /* Bad requests are rejected, head does not move. */
    r.lba = 100 * 2 * 100 - 5;   /* runs past the end */
    r.count = 10;
    CHECK_EQ(disk_service_request(d, &r, 0.0, &t), DISK_ERANGE);
    r.count = 0;
    CHECK_EQ(disk_service_request(d, &r, 0.0, &t), DISK_EINVAL);
    CHECK_EQ(disk_head_cylinder(d), 9);
    disk_close(d);
}

static void track_and_cylinder_crossing(void)
{
    disk_t *d = open_small(0.0);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;

    /* Head switch: (9,0,95..99) then (9,1,0..4), starting at t = 19.5
     * (angle 0.95, right on sector 95). Head switches are free in this
     * model, and sector 0 of the next surface arrives exactly when the
     * last sector of this one ends: 0.5 + 0.5 = 1.0 ms, 2 extents. */
    disk_seek(d, 9, &t);
    disk_request_t r = { (9 * 2 + 0) * 100 + 95, 10, DISK_OP_READ };
    disk_service_request(d, &r, 19.5, &t);
    CHECK_NEAR(t.rotate_ms, 0.0, 1e-9);
    CHECK_NEAR(t.total_ms, 1.0, 1e-9);
    CHECK_EQ(t.extents, 2);

    /* Cylinder crossing WITHOUT skew: (0,1,95..99) then (1,0,0..4).
     *   transfer 5 sectors       0.5 ms   t = 10.0, angle 0.0
     *   seek 1 cylinder          1.5 ms   t = 11.5, angle 0.15
     *   sector 0 is at angle 0   -> wait 0.85 rev = 8.5 ms
     *   transfer 5 sectors       0.5 ms
     *   total 11.0 ms (a lost revolution) */
    disk_seek(d, 0, &t);
    r.lba = (0 * 2 + 1) * 100 + 95;
    disk_service_request(d, &r, 9.5, &t);
    CHECK_NEAR(t.seek_ms, 1.5, 1e-9);
    CHECK_NEAR(t.rotate_ms, 8.5, 1e-9);
    CHECK_NEAR(t.total_ms, 11.0, 1e-9);
    disk_close(d);

    /* Same request WITH cylinder skew 0.2: cylinder 1's sector 0 sits at
     * angle 0.2, so after the 1.5 ms seek (angle 0.15) we wait only 0.5 ms.
     * total = 0.5 + 1.5 + 0.5 + 0.5 = 3.0 ms. */
    d = open_small(0.2);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_service_request(d, &r, 9.5, &t);
    CHECK_NEAR(t.rotate_ms, 0.5, 1e-9);
    CHECK_NEAR(t.total_ms, 3.0, 1e-9);
    disk_close(d);
}

static void zoned_transfer(void)
{
    /* Zone 0 has 10 sectors/track, zone 2 has 6 (see geometry test).
     * Transferring a whole track always takes exactly one revolution, so
     * outer zones move 10/6 times more data per ms. */
    disk_config_t cfg;
    test_small_config(&cfg, 12, 2, 0);
    cfg.zoned = 1;
    cfg.num_zones = 3;
    cfg.spt_outer = 10;
    cfg.spt_inner = 6;
    disk_t *d = disk_open(&cfg, NULL);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;
    disk_request_t r = { 0, 10, DISK_OP_READ };       /* full outer track */
    disk_service_request(d, &r, 0.0, &t);
    CHECK_NEAR(t.transfer_ms, 10.0, EPS);
    CHECK_NEAR(t.total_ms, 10.0, EPS);

    r.lba = 144;  /* (8,0,0): zone 2 */
    r.count = 6;  /* full inner track */
    disk_seek(d, 8, &t);
    disk_service_request(d, &r, 0.0, &t);
    CHECK_NEAR(t.transfer_ms, 10.0, EPS);

    r.count = 3;  /* half an inner track = 3 sectors = 5 ms */
    disk_service_request(d, &r, 0.0, &t);
    CHECK_NEAR(t.transfer_ms, 5.0, EPS);
    disk_close(d);
}

static void estimate_and_seek(void)
{
    disk_t *d = open_small(0.0);
    CHECK(d != NULL);
    if (!d)
        return;
    double pos = -1.0;

    /* Estimate for (9,0,50) at t=0 equals seek + rotation of the real
     * service (2.5 + 2.5) and does not move the head. */
    CHECK_EQ(disk_estimate_access(d, (9 * 2) * 100 + 50, 0.0, &pos), DISK_OK);
    CHECK_NEAR(pos, 5.0, EPS);
    CHECK_EQ(disk_head_cylinder(d), 0);
    CHECK_EQ(disk_estimate_access(d, 1u << 30, 0.0, &pos), DISK_ERANGE);

    disk_timing_t t;
    CHECK_EQ(disk_seek(d, 64, &t), DISK_OK);   /* 1 + 0.5*8 = 5 ms */
    CHECK_NEAR(t.seek_ms, 5.0, EPS);
    CHECK_NEAR(t.rotate_ms, 0.0, EPS);
    CHECK_EQ(t.cylinders_moved, 64);
    CHECK_EQ(disk_head_cylinder(d), 64);
    CHECK_EQ(disk_seek(d, 100, &t), DISK_EINVAL);
    disk_close(d);
}

void suite_timing(void)
{
    formulas();
    single_request();
    track_and_cylinder_crossing();
    zoned_transfer();
    estimate_and_seek();
}
