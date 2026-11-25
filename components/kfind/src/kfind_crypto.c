/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kfind_crypto.c
 * @brief The two primitives the EID needs, small and self-contained: AES-256
 *        (encryption of one block) and SHA-256.
 *
 * Own code rather than mbedTLS: ESP-IDF v6.1 ships mbedTLS 4, PSA only, and
 * the same file has to build for the host tests. Speed does not matter: the
 * beacon computes one EID every 17 minutes. Checked against FIPS-197 C.3 and
 * FIPS 180-2 vectors in tests/host.
 */
#include "kfind.h"

#include <string.h>

/* ---- AES-256 ------------------------------------------------------------ */

static const uint8_t s_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
};

#define AES_ROUNDS     14U   /**< AES-256. */
#define AES_KEY_WORDS  8U
#define AES_SCHED      (4U * (AES_ROUNDS + 1U))

static uint8_t aes_xtime(uint8_t x)
{
    return (uint8_t)((uint8_t)(x << 1) ^ ((0U != (x & 0x80U)) ? 0x1bU : 0x00U));
}

static void aes_expand(const uint8_t key[32], uint8_t w[AES_SCHED][4])
{
    uint8_t rcon = 0x01U;

    for ( uint32_t i = 0U; AES_KEY_WORDS > i; ++i )
    {
        (void)memcpy(w[i], &key[4U * i], 4U);
    }
    for ( uint32_t i = AES_KEY_WORDS; AES_SCHED > i; ++i )
    {
        uint8_t t[4];

        (void)memcpy(t, w[i - 1U], 4U);
        if ( 0U == (i % AES_KEY_WORDS) )
        {
            uint8_t const t0 = t[0];

            t[0] = (uint8_t)(s_sbox[t[1]] ^ rcon);
            t[1] = s_sbox[t[2]];
            t[2] = s_sbox[t[3]];
            t[3] = s_sbox[t0];
            rcon = aes_xtime(rcon);
        }
        else if ( 4U == (i % AES_KEY_WORDS) )
        {
            for ( uint32_t j = 0U; 4U > j; ++j )
            {
                t[j] = s_sbox[t[j]];
            }
        }
        else
        {
            /* the plain xor below */
        }
        for ( uint32_t j = 0U; 4U > j; ++j )
        {
            w[i][j] = (uint8_t)(w[i - AES_KEY_WORDS][j] ^ t[j]);
        }
    }
}

static void aes_add_key(uint8_t s[16], uint8_t w[AES_SCHED][4], uint32_t round)
{
    for ( uint32_t c = 0U; 4U > c; ++c )
    {
        for ( uint32_t r = 0U; 4U > r; ++r )
        {
            s[(4U * c) + r] ^= w[(4U * round) + c][r];
        }
    }
}

static void aes_sub_shift(uint8_t s[16])
{
    uint8_t t[16];

    for ( uint32_t c = 0U; 4U > c; ++c )
    {
        for ( uint32_t r = 0U; 4U > r; ++r )
        {
            /* row r moves left by r columns */
            t[(4U * c) + r] = s_sbox[s[(4U * ((c + r) % 4U)) + r]];
        }
    }
    (void)memcpy(s, t, sizeof(t));
}

static void aes_mix(uint8_t s[16])
{
    for ( uint32_t c = 0U; 4U > c; ++c )
    {
        uint8_t *const col = &s[4U * c];
        uint8_t const a0 = col[0];
        uint8_t const a1 = col[1];
        uint8_t const a2 = col[2];
        uint8_t const a3 = col[3];
        uint8_t const all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);

        col[0] = (uint8_t)(a0 ^ all ^ aes_xtime((uint8_t)(a0 ^ a1)));
        col[1] = (uint8_t)(a1 ^ all ^ aes_xtime((uint8_t)(a1 ^ a2)));
        col[2] = (uint8_t)(a2 ^ all ^ aes_xtime((uint8_t)(a2 ^ a3)));
        col[3] = (uint8_t)(a3 ^ all ^ aes_xtime((uint8_t)(a3 ^ a0)));
    }
}

void kfind_aes256_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16])
{
    uint8_t w[AES_SCHED][4];
    uint8_t s[16];

    aes_expand(key, w);
    (void)memcpy(s, in, sizeof(s));
    aes_add_key(s, w, 0U);
    for ( uint32_t round = 1U; AES_ROUNDS > round; ++round )
    {
        aes_sub_shift(s);
        aes_mix(s);
        aes_add_key(s, w, round);
    }
    aes_sub_shift(s);
    aes_add_key(s, w, AES_ROUNDS);
    (void)memcpy(out, s, sizeof(s));
    (void)memset(w, 0, sizeof(w));   /* the schedule is the key */
    (void)memset(s, 0, sizeof(s));
}

/* ---- SHA-256 ------------------------------------------------------------ */

static const uint32_t s_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static uint32_t sha_ror(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static void sha_block(uint32_t h[8], const uint8_t b[64])
{
    uint32_t w[64];
    uint32_t v[8];

    for ( uint32_t i = 0U; 16U > i; ++i )
    {
        w[i] = ((uint32_t)b[4U * i] << 24) | ((uint32_t)b[(4U * i) + 1U] << 16)
               | ((uint32_t)b[(4U * i) + 2U] << 8) | (uint32_t)b[(4U * i) + 3U];
    }
    for ( uint32_t i = 16U; 64U > i; ++i )
    {
        uint32_t const s0 = sha_ror(w[i - 15U], 7U) ^ sha_ror(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
        uint32_t const s1 = sha_ror(w[i - 2U], 17U) ^ sha_ror(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);

        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    (void)memcpy(v, h, sizeof(v));
    for ( uint32_t i = 0U; 64U > i; ++i )
    {
        uint32_t const s1  = sha_ror(v[4], 6U) ^ sha_ror(v[4], 11U) ^ sha_ror(v[4], 25U);
        uint32_t const ch  = (v[4] & v[5]) ^ ((~v[4]) & v[6]);
        uint32_t const t1  = v[7] + s1 + ch + s_k[i] + w[i];
        uint32_t const s0  = sha_ror(v[0], 2U) ^ sha_ror(v[0], 13U) ^ sha_ror(v[0], 22U);
        uint32_t const maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t const t2  = s0 + maj;

        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }
    for ( uint32_t i = 0U; 8U > i; ++i )
    {
        h[i] += v[i];
    }
}

void kfind_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    uint8_t  tail[128];
    size_t   full = len - (len % 64U);
    size_t   rest = len - full;
    size_t   tail_len = 64U;
    uint64_t const bits = (uint64_t)len * 8U;

    for ( size_t at = 0U; at < full; at += 64U )
    {
        sha_block(h, &data[at]);
    }
    (void)memset(tail, 0, sizeof(tail));
    if ( 0U < rest )
    {
        (void)memcpy(tail, &data[full], rest);
    }
    tail[rest] = 0x80U;
    if ( 56U <= rest )
    {
        tail_len = 128U;
    }
    for ( uint32_t i = 0U; 8U > i; ++i )
    {
        tail[(tail_len - 1U) - i] = (uint8_t)(bits >> (8U * i));
    }
    sha_block(h, tail);
    if ( 128U == tail_len )
    {
        sha_block(h, &tail[64]);
    }
    for ( uint32_t i = 0U; 8U > i; ++i )
    {
        out[4U * i]        = (uint8_t)(h[i] >> 24);
        out[(4U * i) + 1U] = (uint8_t)(h[i] >> 16);
        out[(4U * i) + 2U] = (uint8_t)(h[i] >> 8);
        out[(4U * i) + 3U] = (uint8_t)h[i];
    }
}
