/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests of components/kfind: build and run with `make -C tests/host`. */
#include <stdio.h>
#include <string.h>

#include "kfind.h"
#include "vectors.h"

static int s_fail;
static int s_pass;

#define CHECK(c_) do { if ( c_ ) { ++s_pass; } else { ++s_fail; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c_); } } while (0)

static void test_aes(void)
{
    /* FIPS-197 appendix C.3 */
    uint8_t key[32];
    uint8_t pt[16];
    uint8_t ct[16];
    static const uint8_t want[16] = {
        0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
        0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89
    };

    for ( int i = 0; i < 32; ++i ) { key[i] = (uint8_t)i; }
    for ( int i = 0; i < 16; ++i ) { pt[i] = (uint8_t)(i * 0x11); }
    kfind_aes256_ecb(key, pt, ct);
    CHECK(0 == memcmp(ct, want, 16));
}

static void test_sha(void)
{
    uint8_t h[32];
    static const uint8_t abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    static const uint8_t two[32] = {   /* "abcdbcde...nopq", 56 bytes: two blocks */
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1
    };
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    kfind_sha256((const uint8_t *)"abc", 3U, h);
    CHECK(0 == memcmp(h, abc, 32));
    kfind_sha256((const uint8_t *)m2, strlen(m2), h);
    CHECK(0 == memcmp(h, two, 32));
}

static void test_vectors(void)
{
    for ( size_t i = 0U; i < sizeof(s_vec) / sizeof(s_vec[0]); ++i )
    {
        const vec_t *v = &s_vec[i];
        uint8_t eid[KFIND_EID_LEN];
        uint8_t r[KFIND_R_LEN];
        uint8_t adv[KFIND_ADV_MAX];
        uint8_t addr[KFIND_ADDR_LEN];

        CHECK(kfind_eid(v->eik, v->ts, eid, r));
        CHECK(0 == memcmp(eid, v->eid, KFIND_EID_LEN));
        uint8_t const h = kfind_hashed_flags(r, (kfind_batt_t)v->batt, 0U != v->utp);
        CHECK(h == v->hashed);
        CHECK(KFIND_ADV_LEN == kfind_adv(adv, eid, 0U != v->utp, true, h));
        CHECK(0 == memcmp(adv, v->adv, KFIND_ADV_LEN));
        kfind_nrpa(eid, addr);
        CHECK(0 == memcmp(addr, v->addr, KFIND_ADDR_LEN));
        CHECK(0U == (addr[0] & 0xC0U));
        if ( 0 != s_fail ) { printf("  vector %zu\n", i); }
    }
}

static void test_clock(void)
{
    CHECK(0U == kfind_beacon_time(100, 200));
    CHECK(5000U == kfind_beacon_time(1700005000LL, 1700000000U));
    CHECK(1024U == kfind_slot(2047U, 0U));
    CHECK(1024U == kfind_slot(2048U + 10U, 50U));   /* rotation not yet done */
    CHECK(2048U == kfind_slot(2048U + 60U, 50U));
    CHECK(0U == kfind_slot(5U, 100U));              /* nothing before slot 0 */
}

static void test_misc(void)
{
    uint8_t b[3];
    uint8_t adv[KFIND_ADV_MAX];
    uint8_t eid[KFIND_EID_LEN];

    CHECK(kfind_hex("0aFf10", b, 3U));
    CHECK((0x0aU == b[0]) && (0xffU == b[1]) && (0x10U == b[2]));
    CHECK(!kfind_hex("0aFf1", b, 3U));
    CHECK(!kfind_hex("0aFg10", b, 3U));
    (void)memset(eid, 0x11, sizeof(eid));
    CHECK(28U == kfind_adv(adv, eid, false, false, 0U));
    CHECK(0x18U == adv[3]);
    CHECK(0x40U == adv[7]);
    CHECK(0x80U == kfind_hashed_flags(NULL, KFIND_BATT_NONE, true));
}

static void test_haystack(void)
{
    for ( size_t i = 0U; i < sizeof(s_hvec) / sizeof(s_hvec[0]); ++i )
    {
        uint8_t adv[KFIND_HAYSTACK_ADV_LEN];
        uint8_t addr[KFIND_ADDR_LEN];

        CHECK(KFIND_HAYSTACK_ADV_LEN == kfind_haystack_adv(adv, s_hvec[i].key));
        CHECK(0 == memcmp(adv, s_hvec[i].adv, sizeof(adv)));
        kfind_haystack_addr(s_hvec[i].key, addr);
        CHECK(0 == memcmp(addr, s_hvec[i].addr, sizeof(addr)));
        CHECK(0xC0U == (addr[0] & 0xC0U));
    }
}

int main(void)
{
    test_aes();
    test_sha();
    test_vectors();
    test_clock();
    test_misc();
    test_haystack();
    printf("%d passed, %d failed\n", s_pass, s_fail);
    return (0 == s_fail) ? 0 : 1;
}
