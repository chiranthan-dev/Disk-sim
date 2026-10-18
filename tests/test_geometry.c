/*
 * Mapping tests. Every expected CHS below was worked out by hand from
 *   PBA = (cylinder * heads + head) * spt + sector   (within a zone)
 */
#include "test.h"

static void fixed_geometry(void)
{
    /* 10 cylinders x 2 heads x 8 sectors = 160 PBAs; last cylinder = spares
     * -> 9*2*8 = 144 user LBAs. */
    disk_config_t cfg;
    test_small_config(&cfg, 10, 2, 8);
    cfg.spare_cylinders = 1;
    int err;
    disk_t *d = disk_open(&cfg, &err);
    CHECK(d != NULL);
    if (!d)
        return;

    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.capacity_lbas, 144);
    CHECK_EQ(in.total_pbas, 160);
    CHECK_EQ(in.spare_total, 16);

    disk_chs_t c;
    CHECK_EQ(disk_lba_to_chs(d, 0, &c), DISK_OK);     CHECK_CHS(c, 0, 0, 0); /* first */
    CHECK_EQ(disk_lba_to_chs(d, 7, &c), DISK_OK);     CHECK_CHS(c, 0, 0, 7); /* end of track */
    CHECK_EQ(disk_lba_to_chs(d, 8, &c), DISK_OK);     CHECK_CHS(c, 0, 1, 0); /* head switch */
    CHECK_EQ(disk_lba_to_chs(d, 15, &c), DISK_OK);    CHECK_CHS(c, 0, 1, 7); /* end of cylinder */
    CHECK_EQ(disk_lba_to_chs(d, 16, &c), DISK_OK);    CHECK_CHS(c, 1, 0, 0); /* cylinder crossing */
    CHECK_EQ(disk_lba_to_chs(d, 77, &c), DISK_OK);    CHECK_CHS(c, 4, 1, 5); /* 77 = (4*2+1)*8+5 */
    CHECK_EQ(disk_lba_to_chs(d, 143, &c), DISK_OK);   CHECK_CHS(c, 8, 1, 7); /* last LBA */
    CHECK_EQ(disk_lba_to_chs(d, 144, &c), DISK_ERANGE);                     /* one past */
    CHECK_EQ(disk_lba_to_chs(d, UINT64_MAX, &c), DISK_ERANGE);

    /* CHS -> LBA, including invalid coordinates. */
    uint64_t lba = 0;
    disk_chs_t q = { 1, 0, 0 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_OK);  CHECK_EQ(lba, 16);
    q = (disk_chs_t){ 8, 1, 7 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_OK);  CHECK_EQ(lba, 143);
    q = (disk_chs_t){ 9, 0, 0 };   /* unused spare */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_EDEFECT);
    q = (disk_chs_t){ 10, 0, 0 };  /* no such cylinder */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_ERANGE);
    q = (disk_chs_t){ 0, 2, 0 };   /* no such head */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_ERANGE);
    q = (disk_chs_t){ 0, 0, 8 };   /* no such sector */
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_ERANGE);

    /* Exhaustive round trip: every LBA maps to a CHS that maps back. */
    int ok = 1;
    for (uint64_t l = 0; l < in.capacity_lbas; l++) {
        uint64_t back = UINT64_MAX;
        if (disk_lba_to_chs(d, l, &c) != DISK_OK ||
            disk_chs_to_lba(d, &c, &back) != DISK_OK || back != l)
            ok = 0;
    }
    CHECK(ok);
    disk_close(d);
}

static void zoned_geometry(void)
{
    /* 12 cylinders, 2 heads, 3 zones, SPT 10 -> 6.
     *   zone 0: cyl 0-3,  spt 10, PBAs   0..79   (4*2*10 = 80)
     *   zone 1: cyl 4-7,  spt  8, PBAs  80..143  (4*2*8  = 64)
     *   zone 2: cyl 8-11, spt  6, PBAs 144..191  (4*2*6  = 48) */
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

    disk_zone_t z;
    CHECK_EQ(disk_get_zone(d, 0, &z), DISK_OK);
    CHECK_EQ(z.first_cylinder, 0); CHECK_EQ(z.num_cylinders, 4); CHECK_EQ(z.sectors_per_track, 10);
    CHECK_EQ(disk_get_zone(d, 1, &z), DISK_OK);
    CHECK_EQ(z.first_cylinder, 4); CHECK_EQ(z.sectors_per_track, 8);
    CHECK_EQ(disk_get_zone(d, 2, &z), DISK_OK);
    CHECK_EQ(z.first_cylinder, 8); CHECK_EQ(z.sectors_per_track, 6);
    CHECK_EQ(disk_get_zone(d, 3, &z), DISK_EINVAL);

    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.capacity_lbas, 192);

    disk_chs_t c;
    disk_lba_to_chs(d, 79, &c);  CHECK_CHS(c, 3, 1, 9);   /* last of zone 0 */
    disk_lba_to_chs(d, 80, &c);  CHECK_CHS(c, 4, 0, 0);   /* zone crossing  */
    disk_lba_to_chs(d, 88, &c);  CHECK_CHS(c, 4, 1, 0);   /* 8 sectors/track now */
    disk_lba_to_chs(d, 143, &c); CHECK_CHS(c, 7, 1, 7);
    disk_lba_to_chs(d, 144, &c); CHECK_CHS(c, 8, 0, 0);
    disk_lba_to_chs(d, 191, &c); CHECK_CHS(c, 11, 1, 5);  /* last LBA */
    CHECK_EQ(disk_lba_to_chs(d, 192, &c), DISK_ERANGE);

    /* Sector 6 exists in zone 1 (spt 8) but not in zone 2 (spt 6). */
    uint64_t lba;
    disk_chs_t q = { 5, 0, 7 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_OK);
    CHECK_EQ(lba, 80 + (1 * 2 + 0) * 8 + 7);
    q = (disk_chs_t){ 9, 0, 6 };
    CHECK_EQ(disk_chs_to_lba(d, &q, &lba), DISK_ERANGE);

    int ok = 1;
    for (uint64_t l = 0; l < in.capacity_lbas; l++) {
        uint64_t back = UINT64_MAX;
        if (disk_lba_to_chs(d, l, &c) != DISK_OK ||
            disk_chs_to_lba(d, &c, &back) != DISK_OK || back != l)
            ok = 0;
    }
    CHECK(ok);
    disk_close(d);

    /* Uneven split: 10 cylinders / 3 zones -> 3, 3, 4 (last takes the rest). */
    test_small_config(&cfg, 10, 1, 0);
    cfg.zoned = 1;
    cfg.num_zones = 3;
    cfg.spt_outer = 9;
    cfg.spt_inner = 3;
    d = disk_open(&cfg, NULL);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_get_zone(d, 2, &z);
    CHECK_EQ(z.first_cylinder, 6);
    CHECK_EQ(z.num_cylinders, 4);
    disk_get_zone(d, 1, &z);
    CHECK_EQ(z.sectors_per_track, 6);   /* 9 - (9-3)*1/2 */
    disk_close(d);
}

static void default_drive(void)
{
    /* Default drive, hand calculation:
     *   16 zones of 3125 cylinders, spt_i = 1300 - 40*i (1300 ... 700)
     *   sum(spt) = 16*1300 - 40*(0+..+15) = 20800 - 4800 = 16000
     *   total PBAs = 3125 * 4 heads * 16000 = 200,000,000
     *   spares = 200 cylinders * 4 * 700 = 560,000
     *   capacity = 199,440,000 LBAs -> last LBA at (49799, 3, 699) */
    disk_config_t cfg;
    disk_config_default(&cfg);
    disk_t *d = disk_open(&cfg, NULL);
    CHECK(d != NULL);
    if (!d)
        return;
    disk_info_t in;
    disk_get_info(d, &in);
    CHECK_EQ(in.total_pbas, 200000000ULL);
    CHECK_EQ(in.spare_total, 560000ULL);
    CHECK_EQ(in.capacity_lbas, 199440000ULL);

    disk_chs_t c;
    disk_lba_to_chs(d, 199439999ULL, &c);
    CHECK_CHS(c, 49799, 3, 699);
    /* first LBA of zone 1: 3125*4*1300 = 16,250,000 */
    disk_lba_to_chs(d, 16250000ULL, &c);
    CHECK_CHS(c, 3125, 0, 0);
    disk_lba_to_chs(d, 16249999ULL, &c);
    CHECK_CHS(c, 3124, 3, 1299);
    disk_close(d);
}

static void bad_configs(void)
{
    disk_config_t cfg;
    int err = 0;

    test_small_config(&cfg, 10, 2, 8);
    cfg.rpm = 0;
    CHECK(disk_open(&cfg, &err) == NULL);
    CHECK_EQ(err, DISK_EINVAL);

    test_small_config(&cfg, 10, 2, 8);
    cfg.sector_size = 1000;
    CHECK(disk_open(&cfg, &err) == NULL);

    test_small_config(&cfg, 10, 2, 8);
    cfg.sector_size = 4096;              /* 4Kn is allowed */
    disk_t *d = disk_open(&cfg, &err);
    CHECK(d != NULL);
    disk_close(d);

    test_small_config(&cfg, 10, 2, 0);   /* zero SPT */
    CHECK(disk_open(&cfg, &err) == NULL);

    test_small_config(&cfg, 4, 2, 8);
    cfg.zoned = 1;
    cfg.num_zones = 5;                   /* more zones than cylinders */
    cfg.spt_outer = 10;
    cfg.spt_inner = 5;
    CHECK(disk_open(&cfg, &err) == NULL);

    cfg.num_zones = 2;
    cfg.spt_inner = 20;                  /* inner denser than outer */
    CHECK(disk_open(&cfg, &err) == NULL);

    test_small_config(&cfg, 10, 2, 8);
    cfg.spare_cylinders = 10;            /* no room for data */
    CHECK(disk_open(&cfg, &err) == NULL);

    test_small_config(&cfg, 10, 2, 8);
    cfg.cyl_skew = 1.0;
    CHECK(disk_open(&cfg, &err) == NULL);
}

void suite_geometry(void)
{
    fixed_geometry();
    zoned_geometry();
    default_drive();
    bad_configs();
}
