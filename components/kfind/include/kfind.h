/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kfind.h
 * @brief The Find Hub Network beacon of the Kode Dot, in pure C: the
 *        ephemeral identifier (EID), the hashed flags, the advert's bytes,
 *        the address that goes with them, and the beacon clock.
 *
 * Nothing here touches a radio, a clock or a file. The BLE driver
 * (kdrv_ble_fhn.c) asks this module what to put on the air and when to
 * change it; the host tests check every function against the Python
 * reference in tools/reference/fhn_ref.py.
 *
 * The algorithm is the one of Google's "Find Hub Network Accessory
 * Specification" (Fast Pair extension, v1.3), SECP160R1 variant:
 *
 *   data[0..10]  = 0xFF        data[16..26] = 0x00
 *   data[11]     = K (10)      data[27]     = K
 *   data[12..15] = TS (BE)     data[28..31] = TS
 *   TS  = beacon clock with its K lowest bits cleared
 *   r'  = AES-ECB-256(EIK, data)            (32 bytes, big-endian integer)
 *   r   = r' mod n                          (n = order of SECP160R1)
 *   EID = x coordinate of r * G             (20 bytes, big-endian)
 */
#ifndef KFIND_H_
#define KFIND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KFIND_EIK_LEN        32U    /**< Ephemeral identity key, bytes.          */
#define KFIND_EID_LEN        20U    /**< EID on SECP160R1, bytes.                */
#define KFIND_R_LEN          21U    /**< r, big-endian, as micro-ecc takes it.   */
#define KFIND_K              10U    /**< Rotation exponent.                      */
#define KFIND_ROTATION_S     1024U  /**< 2^K seconds between two EIDs.           */
#define KFIND_JITTER_MAX_S   204U   /**< Largest random delay of a rotation.     */
#define KFIND_ADDR_LEN       6U     /**< A BLE device address.                   */
#define KFIND_ADV_MAX        31U    /**< A legacy advert's payload.              */
#define KFIND_ADV_LEN        29U    /**< The FHN advert with hashed flags.       */
#define KFIND_SVC_UUID       0xFEAAU /**< The 16-bit service the frame is data of. */
#define KFIND_FRAME_NORMAL   0x40U  /**< Frame type: owner nearby.               */
#define KFIND_FRAME_UTP      0x41U  /**< Frame type: unwanted tracking protection. */

/** @brief The battery level the hashed flags carry (bits 5-6). */
typedef enum {
    KFIND_BATT_NONE     = 0,  /**< Not reported.     */
    KFIND_BATT_NORMAL   = 1,
    KFIND_BATT_LOW      = 2,
    KFIND_BATT_CRITICAL = 3
} kfind_batt_t;

/** @brief Where the EID of the advert comes from. */
typedef enum {
    KFIND_MODE_OFF = 0,    /**< No beacon.                                       */
    KFIND_MODE_STATIC,     /**< One fixed EID: the "advertisement key" that the
                            *   unmodified GoogleFindMyTools prints. Proven to
                            *   work, but the same identifier forever.           */
    KFIND_MODE_ROTATING    /**< The EIK and the pairing date are on the board:
                            *   a new EID every 1024 s, as the spec wants.       */
} kfind_mode_t;

/**
 * @brief Compute the EID for one beacon time.
 * @param eik   The 32-byte ephemeral identity key.
 * @param ts    The beacon clock, seconds; its K low bits are ignored.
 * @param eid   Out: 20 bytes.
 * @param r     Out when not NULL: r, KFIND_R_LEN bytes big-endian (for the
 *              hashed flags).
 * @return false when r came out 0 (one chance in 2^160) or an argument is NULL.
 */
bool kfind_eid(const uint8_t eik[KFIND_EIK_LEN], uint32_t ts,
               uint8_t eid[KFIND_EID_LEN], uint8_t r[KFIND_R_LEN]);

/**
 * @brief The hashed flags byte: the flags XOR the last byte of SHA-256(r).
 *
 * r is hashed as 20 bytes: its 21-byte big-endian form truncated to the curve
 * size (the low 160 bits). Battery in bits 5-6 (mask 0x60), UTP in bit 7 (0x80).
 *
 * @param r    r from kfind_eid(), KFIND_R_LEN bytes.
 * @param batt The battery level.
 * @param utp  Unwanted tracking protection mode.
 * @return The byte that goes last in the frame.
 */
uint8_t kfind_hashed_flags(const uint8_t r[KFIND_R_LEN], kfind_batt_t batt, bool utp);

/**
 * @brief Pack the advert: flags, then service data 0xFEAA with the frame.
 * @param out        At least KFIND_ADV_MAX bytes.
 * @param eid        The EID.
 * @param utp        Frame type 0x41 (true) or 0x40.
 * @param has_flags  Append the hashed flags byte.
 * @param hashed     That byte (kfind_hashed_flags()).
 * @return The bytes written: 29 with the flags byte, 28 without; 0 on NULL.
 */
size_t kfind_adv(uint8_t out[KFIND_ADV_MAX], const uint8_t eid[KFIND_EID_LEN],
                 bool utp, bool has_flags, uint8_t hashed);

/**
 * @brief The non-resolvable private address that goes with an EID.
 *
 * Derived from the EID (the first 6 bytes of SHA-256(EID)), so it changes
 * exactly when the EID changes and says nothing the EID does not. The two
 * most significant bits are 0b00, as an NRPA needs, and the value is never
 * all zeros or all ones.
 *
 * @param eid  The EID on the air.
 * @param addr Out: most significant byte first, as printed.
 */
void kfind_nrpa(const uint8_t eid[KFIND_EID_LEN], uint8_t addr[KFIND_ADDR_LEN]);

/**
 * @brief The beacon clock in rotating mode: seconds since the pairing date.
 * @param unix_s    Wall time, Unix seconds.
 * @param pair_date The pairing date the registration told Google.
 * @return unix_s - pair_date; 0 when the wall clock is before the pairing.
 */
uint32_t kfind_beacon_time(int64_t unix_s, uint32_t pair_date);

/**
 * @brief Which rotation slot is on the air at beacon time @p t.
 *
 * A new slot starts every 1024 s; the advert changes @p jitter_s seconds
 * later (1..KFIND_JITTER_MAX_S, picked again at every rotation), so an
 * observer cannot learn the board's clock from the moment of change.
 *
 * @param t        Beacon time.
 * @param jitter_s The delay of this rotation.
 * @return The slot start (a multiple of 1024) whose EID is advertised.
 */
uint32_t kfind_slot(uint32_t t, uint32_t jitter_s);

/**
 * @brief Read @p n bytes of hex (upper or lower case, no separators).
 * @return true when @p s is exactly 2n hex digits.
 */
bool kfind_hex(const char *s, uint8_t *out, size_t n);

/** @brief SHA-256, for the module and its tests. */
void kfind_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/** @brief One AES-256 block in ECB, for the module and its tests. */
void kfind_aes256_ecb(const uint8_t key[32], const uint8_t in[16], uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* KFIND_H_ */
