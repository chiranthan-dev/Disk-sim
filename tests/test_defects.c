/*
 * Defect management: P-list slipping and G-list reallocation.
 *
 * Drive: 10 cylinders x 2 heads x 8 sectors, last cylinder = spare area
 *   data PBAs 0..143, spare PBAs 144..159 (cylinder 9).
 *   6000 rpm -> 10 ms/rev, 8 sectors/track -> 1.25 ms per sector.
 */
#include <stdlib.h>

#include "test.h"
#include "sim.h"

static disk_t *open_with_plist(const uint64_t *plist, size_t n)
{
    disk_config_t cfg;
    test_small_config(&cfg, 10, 2, 8);
    cfg.spare_cylinders = 1;
    cfg.plist = plist;
    cfg.plist_len = n;
    return disk_open(&cfg, NULL);
}

static void plist_slipping(void)
{
    /* Factory defects at PBAs 5, 6 and 20 (given unsorted, with a dup). */
    const uint64_t plist[] = { 20, 5, 6, 5 };
    disk_t *d = open_with_plist(plist, 4);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.plist_len, 3);            /* duplicate removed */
    CHECK_EQ(in.capacity_lbas, 144 - 3);  /* slipping costs capacity */

    /* LBA -> PBA:  0..4 -> 0..4,  5..17 -> 7..19 (skip 5,6),
     *              18.. -> 21..   (skip 20 too) */
    disk_chs_t c;
    disk_lba_to_chs(d, 4, &c);   CHECK_CHS(c, 0, 0, 4);
    disk_lba_to_chs(d, 5, &c);   CHECK_CHS(c, 0, 0, 7);   /* PBA 7 */
    disk_lba_to_chs(d, 17, &c);  CHECK_CHS(c, 1, 0, 3);   /* PBA 19 */
    disk_lba_to_chs(d, 18, &c);  CHECK_CHS(c, 1, 0, 5);   /* PBA 21 */
    disk_lba_to_chs(d, 140, &c); CHECK_CHS(c, 8, 1, 7);   /* last LBA -> PBA 143 */
    CHECK_EQ(disk_lba_to_chs(d, 141, &c), DISK_ERANGE);

    /* A slipped sector has no LBA. */
    uint64_t lba;
    disk_chs_t q = { 0, 0, 5 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_EDEFECT);
    q = (disk_chs_t){ 1, 0, 4 };   /* PBA 20 */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_EDEFECT);
    q = (disk_chs_t){ 0, 0, 7 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_OK);
    CHECK_EQ(lba, 5);

    int ok = 1;
    for (uint64_t l = 0; l < in.capacity_lbas; l++) {
        uint64_t back;
        if (disk_lba_to_chs(d, l, &c) || disk_chs_to_lba(d, &c, &back) || back != l)
            ok = 0;
    }
    CHECK(ok);

    /* Cost of a slip: LBAs 4-5 live at PBAs 4 and 7 on the same track.
     * One extent; the head passes over the two slipped sectors, so the
     * transfer is 4 sector-times (5 ms) instead of 2 - and no extra seek. */
    disk_timing_t t;
    disk_request_t r = { 4, 2, DISK_OP_READ };
    disk_service_request(d, &r, 5.0, &t);   /* t=5 -> angle 0.5 = sector 4 */
    CHECK_EQ(t.extents, 1);
    CHECK_NEAR(t.seek_ms, 0.0, 1e-9);
    CHECK_NEAR(t.rotate_ms, 0.0, 1e-9);
    CHECK_NEAR(t.transfer_ms, 5.0, 1e-9);
    disk_close(d);

    /* P-list entries must be in the data area. */
    const uint64_t in_spare[] = { 150 };
    CHECK(open_with_plist(in_spare, 1) == NULL);
}

static void glist_remapping(void)
{
    disk_t *d = open_with_plist(NULL, 0);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_chs_t c;
    uint64_t lba;

    /* LBA 10 lives at (0,1,2). Mark it grown-defective: it moves to the
     * first spare, PBA 144 = (9,0,0). */
    disk_lba_to_chs(d, 10, &c);  CHECK_CHS(c, 0, 1, 2);
    CHECK_EQ(disk_add_grown_defect(d, 10), DISK_OK);
    CHECK(disk_is_remapped(d, 10));
    CHECK(!disk_is_remapped(d, 11));
    disk_lba_to_chs(d, 10, &c);  CHECK_CHS(c, 9, 0, 0);
    disk_lba_to_chs(d, 11, &c);  CHECK_CHS(c, 0, 1, 3);   /* neighbours unaffected */

    disk_chs_t q = { 9, 0, 0 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_OK);
    CHECK_EQ(lba, 10);
    q = (disk_chs_t){ 0, 1, 2 };   /* old home: retired */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_EDEFECT);

    /* A lower LBA added later: G-list stays sorted, both lookups work. */
    CHECK_EQ(disk_add_grown_defect(d, 3), DISK_OK);
    disk_lba_to_chs(d, 3, &c);   CHECK_CHS(c, 9, 0, 1);
    disk_lba_to_chs(d, 10, &c);  CHECK_CHS(c, 9, 0, 0);

    /* The spare holding LBA 10 fails too: LBA 10 gets a new spare, the old
     * spare is retired. */
    CHECK_EQ(disk_add_grown_defect(d, 10), DISK_OK);
    disk_lba_to_chs(d, 10, &c);  CHECK_CHS(c, 9, 0, 2);
    q = (disk_chs_t){ 9, 0, 0 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_EDEFECT);

    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.glist_len, 2);
    CHECK_EQ(in.spare_used, 3);
    CHECK_EQ(disk_add_grown_defect(d, 144), DISK_ERANGE);

    /* Spare exhaustion: 16 spares total, 3 used. */
    for (uint64_t l = 20; l < 33; l++)
        CHECK_EQ(disk_add_grown_defect(d, l), DISK_OK);
    CHECK_EQ(disk_add_grown_defect(d, 40), DISK_ENOSPC);
    disk_close(d);
}

static void remap_timing(void)
{
    /* Read LBAs 8..12 = (0,1,0..4) starting at t=0 (angle 0 = sector 0).
     * Clean drive: one extent, 5 sectors = 6.25 ms. */
    disk_t *d = open_with_plist(NULL, 0);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_timing_t t;
    disk_request_t r = { 8, 5, DISK_OP_READ };
    disk_service_request(d, &r, 0.0, &t);
    CHECK_EQ(t.extents, 1);
    CHECK_NEAR(t.total_ms, 6.25, 1e-9);

    /* Now LBA 10 is reallocated to (9,0,0). The read splits in three:
     *   (0,1,0..1)  rot 0,   xfer 2.50                       t =  2.50
     *   seek 9 cyl: 1 + 0.5*3 = 2.5                          t =  5.00 (angle .5)
     *   (9,0,0)     wait to angle 0 = 5.0, xfer 1.25         t = 11.25
     *   seek back:  2.5                                      t = 13.75 (angle .375)
     *   (0,1,3..4)  sector 3 is at .375 -> rot 0, xfer 2.50  t = 16.25
     * total 16.25 ms vs 6.25: one remapped sector cost 10 ms. */
    disk_seek(d, 0, &t);
    CHECK_EQ(disk_add_grown_defect(d, 10), DISK_OK);
    disk_service_request(d, &r, 0.0, &t);
    CHECK_EQ(t.extents, 3);
    CHECK_EQ(t.remapped_sectors, 1);
    CHECK_EQ(t.cylinders_moved, 18);
    CHECK_NEAR(t.seek_ms, 5.0, 1e-9);
    CHECK_NEAR(t.rotate_ms, 5.0, 1e-9);
    CHECK_NEAR(t.transfer_ms, 6.25, 1e-9);
    CHECK_NEAR(t.total_ms, 16.25, 1e-9);

    /* SPTF must see the remap: positioning to LBA 10 means going to cyl 9. */
    uint32_t cyl;
    disk_lba_cylinder(d, 10, &cyl);
    CHECK_EQ(cyl, 9);
    disk_close(d);
}

static void random_injection(void)
{
    disk_t *d = open_with_plist(NULL, 0);
    CHECK(d != NULL);
    if (!d)
        return;
    CHECK_EQ(sim_inject_grown_defects(d, 12, 1), DISK_OK);
    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.glist_len, 12);       /* 12 distinct LBAs */
    CHECK_EQ(in.spare_used, 12);
    CHECK_EQ(sim_inject_grown_defects(d, 5, 2), DISK_ENOSPC);  /* only 4 left */
    disk_close(d);

    /* Random P-list generation is distinct, sorted and inside the data area. */
    disk_config_t cfg;
    test_small_config(&cfg, 10, 2, 8);
    cfg.spare_cylinders = 1;
    uint64_t *pl = NULL;
    CHECK_EQ(sim_random_plist(&cfg, 50, 3, &pl), DISK_OK);
    int ok = pl != NULL;
    for (int i = 0; ok && i < 50; i++)
        if (pl[i] >= 144 || (i > 0 && pl[i] <= pl[i - 1]))
            ok = 0;
    CHECK(ok);
    cfg.plist = pl;
    cfg.plist_len = 50;
    d = disk_open(&cfg, NULL);
    CHECK(d != NULL);
    if (d) {
        disk_get_info(d, &in);
        CHECK_EQ(in.capacity_lbas, 94);
        /* round trip still exact with a heavily slipped surface */
        ok = 1;
        for (uint64_t l = 0; l < in.capacity_lbas; l++) {
            disk_chs_t c;
            uint64_t back;
            if (disk_lba_to_chs(d, l, &c) || disk_chs_to_lba(d, &c, &back) || back != l)
                ok = 0;
        }
        CHECK(ok);
        disk_close(d);
    }
    free(pl);
}

void suite_defects(void)
{
    plist_slipping();
    glist_remapping();
    remap_timing();
    random_injection();
}
