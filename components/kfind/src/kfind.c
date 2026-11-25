/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kfind.c
 * @brief EID, hashed flags, advert, address and clock of the Find Hub beacon.
 */
#include "kfind.h"

#include <string.h>

#include "uECC.h"

/** @brief The order n of SECP160R1 (SEC 2), big-endian: 161 bits. */
static const uint8_t s_n[KFIND_R_LEN] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0xF4, 0xC8, 0xF9, 0x27, 0xAE, 0xD3, 0xCA, 0x75, 0x22, 0x57
};

#define AD_FLAGS_LEN   0x02U   /**< Length byte of the Flags AD.           */
#define AD_FLAGS       0x01U
#define AD_FLAGS_VAL   0x06U   /**< LE General Discoverable, no BR/EDR.     */
#define AD_SVC_DATA    0x16U   /**< Service Data, 16-bit UUID.              */
#define HASH_LEN       32U
#define R_HASHED       20U     /**< r is hashed truncated to the curve size. */

/** @brief Compare two big-endian numbers of @p n bytes: <0, 0, >0. */
static int be_cmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        if ( a[i] != b[i] )
        {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

/** @brief a -= b, both big-endian @p n bytes, a >= b. */
static void be_sub(uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned borrow = 0U;

    for ( size_t i = n; i > 0U; --i )
    {
        int const d = (int)a[i - 1U] - (int)b[i - 1U] - (int)borrow;

        borrow  = (d < 0) ? 1U : 0U;
        a[i - 1U] = (uint8_t)d;
    }
}

/**
 * @brief r = x mod n, x a 256-bit big-endian number. Bit by bit: the beacon
 *        does this once per rotation, so clarity wins over speed.
 */
static void mod_n(const uint8_t x[32], uint8_t r[KFIND_R_LEN])
{
    /* One byte more than n, so the shift never loses the top bit. */
    uint8_t rem[KFIND_R_LEN + 1U];
    uint8_t n_ext[KFIND_R_LEN + 1U];

    (void)memset(rem, 0, sizeof(rem));
    n_ext[0] = 0U;
    (void)memcpy(&n_ext[1], s_n, KFIND_R_LEN);
    for ( uint32_t bit = 0U; 256U > bit; ++bit )
    {
        unsigned const in = (x[bit / 8U] >> (7U - (bit % 8U))) & 1U;
        unsigned carry = in;

        for ( size_t i = sizeof(rem); i > 0U; --i )
        {
            unsigned const top = (rem[i - 1U] >> 7) & 1U;

            rem[i - 1U] = (uint8_t)((uint8_t)(rem[i - 1U] << 1) | carry);
            carry = top;
        }
        if ( 0 <= be_cmp(rem, n_ext, sizeof(rem)) )
        {
            be_sub(rem, n_ext, sizeof(rem));
        }
    }
    (void)memcpy(r, &rem[1], KFIND_R_LEN);
}

bool kfind_eid(const uint8_t eik[KFIND_EIK_LEN], uint32_t ts,
               uint8_t eid[KFIND_EID_LEN], uint8_t r[KFIND_R_LEN])
{
    uint8_t data[32];
    uint8_t rd[32];
    uint8_t rr[KFIND_R_LEN];
    uint8_t pub[2U * KFIND_EID_LEN];
    uint8_t zero[KFIND_R_LEN];
    uint32_t const masked = ts & ~((1UL << KFIND_K) - 1UL);
    bool ok = false;

    if ( (NULL == eik) || (NULL == eid) )
    {
        return false;
    }
    (void)memset(data, 0xFF, 11U);
    data[11] = (uint8_t)KFIND_K;
    data[12] = (uint8_t)(masked >> 24);
    data[13] = (uint8_t)(masked >> 16);
    data[14] = (uint8_t)(masked >> 8);
    data[15] = (uint8_t)masked;
    (void)memset(&data[16], 0x00, 11U);
    data[27] = (uint8_t)KFIND_K;
    (void)memcpy(&data[28], &data[12], 4U);

    kfind_aes256_ecb(eik, &data[0], &rd[0]);
    kfind_aes256_ecb(eik, &data[16], &rd[16]);
    mod_n(rd, rr);

    (void)memset(zero, 0, sizeof(zero));
    if ( 0 != be_cmp(rr, zero, sizeof(rr)) )
    {
        if ( 1 == uECC_compute_public_key(rr, pub, uECC_secp160r1()) )
        {
            (void)memcpy(eid, pub, KFIND_EID_LEN);
            if ( NULL != r )
            {
                (void)memcpy(r, rr, KFIND_R_LEN);
            }
            ok = true;
        }
    }
    (void)memset(rd, 0, sizeof(rd));
    (void)memset(rr, 0, sizeof(rr));
    return ok;
}

uint8_t kfind_hashed_flags(const uint8_t r[KFIND_R_LEN], kfind_batt_t batt, bool utp)
{
    uint8_t h[HASH_LEN];
    uint8_t flags = (uint8_t)(((uint8_t)batt & 0x03U) << 5);

    if ( utp )
    {
        flags = (uint8_t)(flags | 0x80U);
    }
    if ( NULL == r )
    {
        return flags;
    }
    kfind_sha256(&r[KFIND_R_LEN - R_HASHED], R_HASHED, h);
    return (uint8_t)(flags ^ h[HASH_LEN - 1U]);
}

size_t kfind_adv(uint8_t out[KFIND_ADV_MAX], const uint8_t eid[KFIND_EID_LEN],
                 bool utp, bool has_flags, uint8_t hashed)
{
    size_t at = 0U;

    if ( (NULL == out) || (NULL == eid) )
    {
        return 0U;
    }
    out[at++] = AD_FLAGS_LEN;
    out[at++] = AD_FLAGS;
    out[at++] = AD_FLAGS_VAL;
    /* type, UUID (2), frame type, EID, [hashed flags] */
    out[at++] = (uint8_t)(1U + 2U + 1U + KFIND_EID_LEN + (has_flags ? 1U : 0U));
    out[at++] = AD_SVC_DATA;
    out[at++] = (uint8_t)(KFIND_SVC_UUID & 0xFFU);
    out[at++] = (uint8_t)(KFIND_SVC_UUID >> 8);
    out[at++] = utp ? (uint8_t)KFIND_FRAME_UTP : (uint8_t)KFIND_FRAME_NORMAL;
    (void)memcpy(&out[at], eid, KFIND_EID_LEN);
    at += KFIND_EID_LEN;
    if ( has_flags )
    {
        out[at++] = hashed;
    }
    return at;
}

void kfind_nrpa(const uint8_t eid[KFIND_EID_LEN], uint8_t addr[KFIND_ADDR_LEN])
{
    uint8_t h[HASH_LEN];
    bool    zeros = true;
    bool    ones  = true;

    kfind_sha256(eid, KFIND_EID_LEN, h);
    (void)memcpy(addr, h, KFIND_ADDR_LEN);
    addr[0] = (uint8_t)(addr[0] & 0x3FU);   /* 0b00: non-resolvable private */
    for ( size_t i = 0U; KFIND_ADDR_LEN > i; ++i )
    {
        /* The random part is the 46 bits under the two type bits. */
        uint8_t const mask = (0U == i) ? 0x3FU : 0xFFU;

        zeros = zeros && (0U == (addr[i] & mask));
        ones  = ones && (mask == (addr[i] & mask));
    }
    if ( zeros || ones )
    {
        addr[KFIND_ADDR_LEN - 1U] ^= 0x01U;
    }
}

uint32_t kfind_beacon_time(int64_t unix_s, uint32_t pair_date)
{
    if ( unix_s <= (int64_t)pair_date )
    {
        return 0U;
    }
    return (uint32_t)(unix_s - (int64_t)pair_date);
}

uint32_t kfind_slot(uint32_t t, uint32_t jitter_s)
{
    uint32_t const slot = t - (t % KFIND_ROTATION_S);

    if ( ((t % KFIND_ROTATION_S) < jitter_s) && (slot >= KFIND_ROTATION_S) )
    {
        return slot - KFIND_ROTATION_S;   /* this rotation has not happened yet */
    }
    return slot;
}

size_t kfind_haystack_adv(uint8_t out[KFIND_HAYSTACK_ADV_LEN], const uint8_t key[KFIND_HAYSTACK_KEY_LEN])
{
    if ( (NULL == out) || (NULL == key) )
    {
        return 0U;
    }
    out[0] = 0x1EU;   /* length 30 */
    out[1] = 0xFFU;   /* manufacturer specific data */
    out[2] = 0x4CU;   /* Apple, little-endian */
    out[3] = 0x00U;
    out[4] = 0x12U;   /* offline finding */
    out[5] = 0x19U;   /* its length, 25 */
    out[6] = 0x00U;   /* status */
    (void)memcpy(&out[7], &key[6], 22U);
    out[29] = (uint8_t)(key[0] >> 6);
    out[30] = 0x00U;  /* hint */
    return KFIND_HAYSTACK_ADV_LEN;
}

void kfind_haystack_addr(const uint8_t key[KFIND_HAYSTACK_KEY_LEN], uint8_t addr[KFIND_ADDR_LEN])
{
    (void)memcpy(addr, key, KFIND_ADDR_LEN);
    addr[0] = (uint8_t)(addr[0] | 0xC0U);   /* static random */
}

static int hex_val(char c)
{
    if ( (c >= '0') && (c <= '9') )
    {
        return c - '0';
    }
    if ( (c >= 'a') && (c <= 'f') )
    {
        return (c - 'a') + 10;
    }
    if ( (c >= 'A') && (c <= 'F') )
    {
        return (c - 'A') + 10;
    }
    return -1;
}

bool kfind_hex(const char *s, uint8_t *out, size_t n)
{
    if ( (NULL == s) || (NULL == out) || (strlen(s) != (2U * n)) )
    {
        return false;
    }
    for ( size_t i = 0U; i < n; ++i )
    {
        int const hi = hex_val(s[2U * i]);
        int const lo = hex_val(s[(2U * i) + 1U]);

        if ( (hi < 0) || (lo < 0) )
        {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
