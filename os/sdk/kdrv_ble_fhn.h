/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_fhn.h
 * @brief The Find Hub Network beacon: the Kode Dot on Google's crowd network,
 *        and (optionally) on Apple's Find My network the OpenHaystack way.
 *
 * Once configured, the board sends a Find Hub advert at least once every two
 * seconds while the BLE host is up, between the console's own adverts: the
 * NimBLE host here has one legacy advertising set, so the beacon borrows it
 * for one short non-connectable burst (~30 ms) and gives it back. Android
 * phones that hear the advert report its encrypted location to Google; the
 * owner decrypts it with the identity key (tools/kodefind in kode-findhub).
 *
 * The maths are kfind.h's. The configuration lives in NVS (namespace
 * "kfind") and survives a reboot; a factory reset of NVS forgets it.
 *
 * Every call here may come from any task: the state is guarded by the
 * advert's own lock, the one the console's pulses take.
 */
#ifndef KDRV_BLE_FHN_H_
#define KDRV_BLE_FHN_H_

#include <stdbool.h>
#include <stdint.h>

#include "kfind.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How often the beacon goes on the air, ms, before its jitter. */
#define KDRV_BLE_FHN_PERIOD_MS   1700U
/** @brief The most random delay added to each period, ms (stays under 2 s). */
#define KDRV_BLE_FHN_JITTER_MS   250U
/** @brief One burst on the air, ms: one or two advertising events. */
#define KDRV_BLE_FHN_BURST_MS    30U
/** @brief The burst's advertising interval, 0.625 ms units (20 ms). */
#define KDRV_BLE_FHN_ITVL        32U

/** @brief What the owner set. */
typedef struct {
    kfind_mode_t mode;                    /**< OFF, STATIC or ROTATING.        */
    bool         utp;                     /**< Frame 0x41 (UTP) instead of 0x40. */
    uint8_t      eik[KFIND_EIK_LEN];      /**< ROTATING: the identity key.     */
    uint32_t     pair_date;               /**< ROTATING: Unix s of the registration. */
    uint8_t      key[KFIND_EID_LEN];      /**< STATIC: the advertisement key.  */
} kdrv_ble_fhn_cfg_t;

/** @brief What the beacon is doing, for `findhub_info` and the settings card. */
typedef struct {
    kfind_mode_t mode;
    bool         utp;
    bool         clock_wall;   /**< The beacon clock follows the wall clock;
                                *   false: it runs from the last checkpoint. */
    uint32_t     beacon_t;     /**< Beacon clock, s (ROTATING).            */
    uint32_t     slot;         /**< Slot whose EID is on the air.          */
    uint32_t     next_s;       /**< Seconds until the next rotation.       */
    uint8_t      eid[KFIND_EID_LEN];
    uint8_t      addr[KFIND_ADDR_LEN];
    uint8_t      batt;         /**< kfind_batt_t on the air.               */
    uint32_t     sent;         /**< Bursts on the air since boot.          */
    uint32_t     skipped;      /**< Periods with no burst (radio busy, host down). */
    bool         host_up;      /**< The BLE host is synced.                */
    bool         hay_on;       /**< Apple Find My (OpenHaystack) key set.  */
    uint8_t      hay_addr[KFIND_ADDR_LEN]; /**< Its address (key bytes 0..5). */
    uint32_t     hay_sent;     /**< Apple bursts on the air since boot.    */
    uint32_t     hay_skipped;  /**< Apple bursts that could not go out.    */
} kdrv_ble_fhn_status_t;

/**
 * @brief Set the beacon and keep it in NVS. Takes effect at the next period.
 * @param cfg What to send. mode OFF forgets the keys.
 * @return KODE_OK; KODE_DENIED for NULL or a ROTATING config with a zero
 *         pairing date; KODE_ERROR when NVS refused.
 */
kode_status_t kdrv_ble_fhn_configure(const kdrv_ble_fhn_cfg_t *cfg);

/** @brief Read what the beacon is doing. @param out Filled. */
void kdrv_ble_fhn_status(kdrv_ble_fhn_status_t *out);

/**
 * @brief Set or clear the Apple Find My (OpenHaystack) key, kept in NVS.
 *
 * Its advert follows Google's in the same period (or goes alone), from the
 * static random address that carries the key's first 6 bytes.
 *
 * @param key The 28-byte advertisement key; NULL stops it.
 * @return KODE_OK, or KODE_ERROR when NVS refused.
 */
kode_status_t kdrv_ble_fhn_haystack(const uint8_t key[KFIND_HAYSTACK_KEY_LEN]);

/** @brief The battery, in percent, as AO_Power reads it: hashed flags bits 5-6. */
void kdrv_ble_fhn_battery(uint8_t pct);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_FHN_H_ */
