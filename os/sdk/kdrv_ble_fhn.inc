/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_fhn.inc
 * @brief The Find Hub beacon's scheduler. Included by kdrv_ble.c, at its end,
 *        on purpose: the beacon shares the host's ONE legacy advertising set
 *        with the console, so it reads the advert's own state (s_ble, s_pulse,
 *        s_pulse_lock) and gives the set back exactly as it found it.
 *
 * How the set is shared, every KDRV_BLE_FHN_PERIOD_MS + jitter, on NimBLE's
 * host task:
 *
 *  - nothing on the air (idle, connected, or a pulsed console advert between
 *    two pulses): the beacon goes on the air for KDRV_BLE_FHN_BURST_MS;
 *  - a pulse on the air, or one due within the burst: try again just after;
 *  - a continuous console advert: stop it, send the burst, start it again
 *    with the time it had left.
 *
 * The burst is non-connectable and non-scannable, from its own address
 * (an NRPA derived from the EID, kfind_nrpa()). After it the console's advert
 * data is put back, so the next console advert carries the console's bytes.
 * A console advert that starts while a burst is on the air stops the burst
 * first (fhn_yield_locked()).
 */
#if CONFIG_KODEOS_FINDHUB

#include "kdrv_ble_fhn.h"

#define FHN_NVS_NS        "kfind"
#define FHN_NVS_CFG       "cfg"
#define FHN_NVS_CLOCK     "t"
#define FHN_NVS_HAY       "hay"     /**< Apple Find My advertisement key, 28 bytes. */
#define FHN_CFG_VER       1U
#define FHN_CFG_BYTES     (1U + 1U + 1U + KFIND_EIK_LEN + 4U + KFIND_EID_LEN)
#define FHN_CKPT_S        (6U * 3600U)   /**< Wall clock to NVS every 6 h.   */
#define FHN_RETRY_MS      120U           /**< A pulse on the air: wait it out. */
#define FHN_US_PER_S      1000000ULL

static struct {
    bool                   inited;      /**< The callout exists (this host's).  */
    bool                   loaded;      /**< NVS read once.                     */
    kdrv_ble_fhn_cfg_t     cfg;
    /* What is on the air, recomputed at each rotation. */
    bool                   have;        /**< adv/eid/addr hold a computed frame. */
    uint32_t               slot;
    uint32_t               jitter;
    uint8_t                eid[KFIND_EID_LEN];
    uint8_t                addr[KFIND_ADDR_LEN];  /**< MSB first.              */
    uint8_t                adv[KFIND_ADV_MAX];
    uint8_t                adv_len;
    uint8_t                batt;
    bool                   clock_wall;
    uint32_t               beacon_t;
    /* The air. */
    bool                   on_air;      /**< A burst is enabled now.            */
    bool                   paused;      /**< A continuous console advert waits for the burst's end. */
    int32_t                cons_left_ms;/**< Its time left; BLE_HS_FOREVER for none. */
    bool                   cons_starting; /**< kdrv_ble_advertise() is starting one. */
    /* The console's advert, as kdrv_ble_advertise() last set it. */
    bool                   cons_have;
    struct ble_hs_adv_fields cons_fields;
    struct ble_gap_adv_params cons_params;
    uint32_t               cons_t0;     /**< khal_time_ms() at its start.       */
    int32_t                cons_dur;    /**< Its duration; BLE_HS_FOREVER.      */
    /* The clock when the wall is not known. */
    uint64_t               ckpt_unix;
    uint64_t               ckpt_up_s;
    uint64_t               last_ckpt_unix;
    /* Counters. */
    uint32_t               sent;
    uint32_t               skipped;
    /* Apple Find My (OpenHaystack): one fixed advertisement key. */
    bool                   hay_on;
    uint8_t                hay_key[KFIND_HAYSTACK_KEY_LEN];
    uint8_t                hay_adv[KFIND_HAYSTACK_ADV_LEN];
    uint8_t                hay_addr[KFIND_ADDR_LEN];
    uint32_t               hay_sent;
    uint32_t               hay_skipped;
    bool                   then_apple;  /**< Apple's burst follows the one on the air. */
    /* The random address the controller holds now (MSB first). */
    bool                   rnd_valid;
    uint8_t                rnd[KFIND_ADDR_LEN];
    struct ble_npl_callout co;
} s_fhn;

static void fhn_timer(struct ble_npl_event *ev);
static int  fhn_gap_event(struct ble_gap_event *event, void *arg);

/* ---- Configuration -------------------------------------------------------- */

static void fhn_pack(const kdrv_ble_fhn_cfg_t *c, uint8_t out[FHN_CFG_BYTES])
{
    out[0] = (uint8_t)FHN_CFG_VER;
    out[1] = (uint8_t)c->mode;
    out[2] = c->utp ? 1U : 0U;
    (void)memcpy(&out[3], c->eik, KFIND_EIK_LEN);
    out[3U + KFIND_EIK_LEN]      = (uint8_t)(c->pair_date >> 24);
    out[3U + KFIND_EIK_LEN + 1U] = (uint8_t)(c->pair_date >> 16);
    out[3U + KFIND_EIK_LEN + 2U] = (uint8_t)(c->pair_date >> 8);
    out[3U + KFIND_EIK_LEN + 3U] = (uint8_t)c->pair_date;
    (void)memcpy(&out[7U + KFIND_EIK_LEN], c->key, KFIND_EID_LEN);
}

static bool fhn_unpack(const uint8_t in[FHN_CFG_BYTES], kdrv_ble_fhn_cfg_t *c)
{
    if ( ((uint8_t)FHN_CFG_VER != in[0]) || ((uint8_t)KFIND_MODE_ROTATING < in[1]) )
    {
        return false;
    }
    c->mode = (kfind_mode_t)in[1];
    c->utp  = (0U != in[2]);
    (void)memcpy(c->eik, &in[3], KFIND_EIK_LEN);
    c->pair_date = ((uint32_t)in[3U + KFIND_EIK_LEN] << 24)
                   | ((uint32_t)in[3U + KFIND_EIK_LEN + 1U] << 16)
                   | ((uint32_t)in[3U + KFIND_EIK_LEN + 2U] << 8)
                   | (uint32_t)in[3U + KFIND_EIK_LEN + 3U];
    (void)memcpy(c->key, &in[7U + KFIND_EIK_LEN], KFIND_EID_LEN);
    return true;
}

/** @brief Read NVS once; called with s_pulse_lock held. */
static void fhn_load_locked(void)
{
    uint8_t  blob[FHN_CFG_BYTES];
    size_t   len = sizeof(blob);
    uint64_t t   = 0U;

    if ( s_fhn.loaded )
    {
        return;
    }
    s_fhn.loaded = true;
    s_fhn.cfg.mode = KFIND_MODE_OFF;
    s_fhn.batt = (uint8_t)KFIND_BATT_NONE;
    if ( (KODE_OK == khal_nvs_get_blob(FHN_NVS_NS, FHN_NVS_CFG, blob, &len))
         && (sizeof(blob) == len) )
    {
        if ( !fhn_unpack(blob, &s_fhn.cfg) )
        {
            KLOG_W(TAG, "find hub: stored config not understood; beacon off");
            s_fhn.cfg.mode = KFIND_MODE_OFF;
        }
    }
    len = sizeof(blob);
    if ( (KODE_OK == khal_nvs_get_blob(FHN_NVS_NS, FHN_NVS_HAY, blob, &len))
         && (KFIND_HAYSTACK_KEY_LEN == len) )
    {
        (void)memcpy(s_fhn.hay_key, blob, KFIND_HAYSTACK_KEY_LEN);
        (void)kfind_haystack_adv(s_fhn.hay_adv, s_fhn.hay_key);
        kfind_haystack_addr(s_fhn.hay_key, s_fhn.hay_addr);
        s_fhn.hay_on = true;
    }
    if ( KODE_OK == khal_nvs_get_u64(FHN_NVS_NS, FHN_NVS_CLOCK, &t) )
    {
        s_fhn.ckpt_unix = t;
        s_fhn.ckpt_up_s = khal_time_us() / FHN_US_PER_S;
    }
    (void)memset(blob, 0, sizeof(blob));
}

kode_status_t kdrv_ble_fhn_configure(const kdrv_ble_fhn_cfg_t *cfg)
{
    uint8_t blob[FHN_CFG_BYTES];
    kode_status_t rc;

    if ( (NULL == cfg) || ((uint8_t)KFIND_MODE_ROTATING < (uint8_t)cfg->mode)
         || ((KFIND_MODE_ROTATING == cfg->mode) && (0U == cfg->pair_date)) )
    {
        return KODE_DENIED;
    }
    if ( KFIND_MODE_OFF == cfg->mode )
    {
        rc = khal_nvs_erase(FHN_NVS_NS, FHN_NVS_CFG);   /* missing is OK too */
    }
    else
    {
        fhn_pack(cfg, blob);
        rc = khal_nvs_set_blob(FHN_NVS_NS, FHN_NVS_CFG, blob, sizeof(blob));
        (void)memset(blob, 0, sizeof(blob));
    }
    if ( KODE_OK != rc )
    {
        return KODE_ERROR;
    }
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_lock(s_pulse_lock);
    }
    s_fhn.loaded = true;
    s_fhn.cfg    = *cfg;
    if ( KFIND_MODE_OFF == cfg->mode )
    {
        (void)memset(&s_fhn.cfg, 0, sizeof(s_fhn.cfg));
    }
    s_fhn.have = false;   /* recomputed at the next period */
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_unlock(s_pulse_lock);
    }
    KLOG_N(TAG, "find hub: beacon %s", (KFIND_MODE_OFF == cfg->mode) ? "off"
           : ((KFIND_MODE_STATIC == cfg->mode) ? "on (static key)" : "on (rotating)"));
    return KODE_OK;
}

kode_status_t kdrv_ble_fhn_haystack(const uint8_t key[KFIND_HAYSTACK_KEY_LEN])
{
    kode_status_t rc;

    if ( NULL == key )
    {
        rc = khal_nvs_erase(FHN_NVS_NS, FHN_NVS_HAY);
    }
    else
    {
        rc = khal_nvs_set_blob(FHN_NVS_NS, FHN_NVS_HAY, key, KFIND_HAYSTACK_KEY_LEN);
    }
    if ( KODE_OK != rc )
    {
        return KODE_ERROR;
    }
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_lock(s_pulse_lock);
    }
    fhn_load_locked();
    s_fhn.hay_on = (NULL != key);
    if ( NULL != key )
    {
        (void)memcpy(s_fhn.hay_key, key, KFIND_HAYSTACK_KEY_LEN);
        (void)kfind_haystack_adv(s_fhn.hay_adv, s_fhn.hay_key);
        kfind_haystack_addr(s_fhn.hay_key, s_fhn.hay_addr);
    }
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_unlock(s_pulse_lock);
    }
    KLOG_N(TAG, "find my (OpenHaystack): beacon %s", (NULL != key) ? "on" : "off");
    return KODE_OK;
}

void kdrv_ble_fhn_battery(uint8_t pct)
{
    uint8_t b = (uint8_t)KFIND_BATT_NORMAL;

    if ( 10U > pct )
    {
        b = (uint8_t)KFIND_BATT_CRITICAL;
    }
    else if ( 25U > pct )
    {
        b = (uint8_t)KFIND_BATT_LOW;
    }
    else
    {
        /* normal */
    }
    if ( b != s_fhn.batt )
    {
        s_fhn.batt = b;
        s_fhn.have = false;   /* the hashed flags change with it */
    }
}

/* ---- The frame -------------------------------------------------------------- */

/** @brief The beacon clock; false when there is none to trust. */
static bool fhn_clock(uint32_t *t)
{
    int64_t unix_s = 0;
    uint64_t const up_s = khal_time_us() / FHN_US_PER_S;

    if ( khal_time_wall_known() )
    {
        unix_s = (int64_t)(khal_time_wall_us() / FHN_US_PER_S);
        s_fhn.clock_wall = true;
        if ( (uint64_t)unix_s >= (s_fhn.last_ckpt_unix + FHN_CKPT_S) )
        {
            /* Seldom, and small: what a board whose battery was cut needs to
             * start again near the right EID (the spec asks for once a day). */
            s_fhn.last_ckpt_unix = (uint64_t)unix_s;
            s_fhn.ckpt_unix      = (uint64_t)unix_s;
            s_fhn.ckpt_up_s      = up_s;
            (void)khal_nvs_set_u64(FHN_NVS_NS, FHN_NVS_CLOCK, (uint64_t)unix_s);
        }
    }
    else if ( 0U != s_fhn.ckpt_unix )
    {
        unix_s = (int64_t)(s_fhn.ckpt_unix + (up_s - s_fhn.ckpt_up_s));
        s_fhn.clock_wall = false;
    }
    else
    {
        return false;
    }
    *t = kfind_beacon_time(unix_s, s_fhn.cfg.pair_date);
    return true;
}

/** @brief Make sure adv/eid/addr are this moment's. Host task, lock held. */
static bool fhn_refresh_locked(void)
{
    uint8_t r[KFIND_R_LEN];
    bool    flags = true;
    uint8_t hashed = 0U;

    if ( KFIND_MODE_STATIC == s_fhn.cfg.mode )
    {
        if ( !s_fhn.have )
        {
            (void)memcpy(s_fhn.eid, s_fhn.cfg.key, KFIND_EID_LEN);
            s_fhn.slot = 0U;
            /* No r without the EIK: the byte goes out as GoogleFindMyTools'
             * own firmware sends it, zero. */
            hashed = 0U;
            s_fhn.adv_len = (uint8_t)kfind_adv(s_fhn.adv, s_fhn.eid, s_fhn.cfg.utp, flags, hashed);
            kfind_nrpa(s_fhn.eid, s_fhn.addr);
            s_fhn.have = true;
        }
        return true;
    }

    uint32_t t = 0U;

    if ( !fhn_clock(&t) )
    {
        return false;
    }
    s_fhn.beacon_t = t;
    uint32_t const slot = kfind_slot(t, s_fhn.jitter);

    if ( s_fhn.have && (slot == s_fhn.slot) )
    {
        return true;
    }
    if ( !kfind_eid(s_fhn.cfg.eik, slot, s_fhn.eid, r) )
    {
        return false;
    }
    hashed = kfind_hashed_flags(r, (kfind_batt_t)s_fhn.batt, s_fhn.cfg.utp);
    (void)memset(r, 0, sizeof(r));
    s_fhn.adv_len = (uint8_t)kfind_adv(s_fhn.adv, s_fhn.eid, s_fhn.cfg.utp, flags, hashed);
    if ( s_fhn.have && (slot != s_fhn.slot) )
    {
        /* A rotation: the next one comes 1..204 s after its slot begins. */
        s_fhn.jitter = 1U + (esp_random() % KFIND_JITTER_MAX_S);
    }
    s_fhn.slot = slot;
    kfind_nrpa(s_fhn.eid, s_fhn.addr);
    s_fhn.have = true;
    return true;
}

/* ---- Sharing the advertising set ------------------------------------------- */

/**
 * @brief A console advert is about to start: take a burst off the air first.
 *        Called with s_pulse_lock held.
 */
static void fhn_yield_locked(void)
{
    if ( s_fhn.on_air )
    {
        (void)ble_gap_adv_stop();   /* no ADV_COMPLETE for a stop asked for */
        s_fhn.on_air = false;
    }
    s_fhn.paused = false;
    if ( s_fhn.cons_have )
    {
        (void)ble_gap_adv_set_fields(&s_fhn.cons_fields);
    }
}

/** @brief kdrv_ble_advertise() set these fields: keep them to put back after a burst. */
static void fhn_note_console(const struct ble_hs_adv_fields *f)
{
    khal_mutex_lock(s_pulse_lock);
    s_fhn.cons_fields = *f;
    s_fhn.cons_have   = true;
    khal_mutex_unlock(s_pulse_lock);
}

/** @brief A continuous console advert started with @p params for @p dur ms. */
static void fhn_note_console_start(const struct ble_gap_adv_params *params, int32_t dur)
{
    khal_mutex_lock(s_pulse_lock);
    s_fhn.cons_params   = *params;
    s_fhn.cons_dur      = dur;
    s_fhn.cons_t0       = khal_time_ms();
    s_fhn.cons_starting = false;
    khal_mutex_unlock(s_pulse_lock);
}

/** @brief The console is starting a continuous advert: no burst until it has. */
static void fhn_console_starting(void)
{
    khal_mutex_lock(s_pulse_lock);
    fhn_yield_locked();
    s_fhn.cons_starting = true;
    khal_mutex_unlock(s_pulse_lock);
}

static void fhn_arm(uint32_t ms)
{
    if ( s_fhn.inited )
    {
        (void)ble_npl_callout_reset(&s_fhn.co, ble_npl_time_ms_to_ticks32(ms));
    }
}

static uint32_t fhn_period(void)
{
    return KDRV_BLE_FHN_PERIOD_MS + (esp_random() % KDRV_BLE_FHN_JITTER_MS);
}

/**
 * @brief Put the console's data back, and its continuous advert if paused. Lock held.
 * @return true when the console's advert ended meanwhile: the caller publishes
 *         IDLE once the lock is released, as ADV_COMPLETE would have.
 */
static bool fhn_give_back_locked(void)
{
    bool idle = false;

    if ( s_fhn.cons_have )
    {
        (void)ble_gap_adv_set_fields(&s_fhn.cons_fields);
    }
    if ( s_pulse.live && s_pulse.on_air )
    {
        /* A pulse that tried to start during the burst met EALREADY and
         * thinks it is on the air: start it for real. */
        (void)ble_gap_adv_start(s_ble.own_addr_type, NULL, (int32_t)KDRV_BLE_PULSE_ON_MS,
                                &s_pulse.params, &ble_gap_event, NULL);
    }
    if ( s_fhn.paused )
    {
        int32_t left = s_fhn.cons_left_ms;

        s_fhn.paused = false;
        if ( (BLE_HS_FOREVER == left) || (0 < left) )
        {
            if ( 0 != ble_gap_adv_start(s_ble.own_addr_type, NULL, left, &s_fhn.cons_params,
                                        &ble_gap_event, NULL) )
            {
                idle = true;
            }
        }
        else
        {
            idle = true;   /* its time ran out during the burst */
        }
    }
    return idle;
}

/** @brief Which beacon a burst carries. */
#define FHN_KIND_GOOGLE  1U
#define FHN_KIND_APPLE   2U

/**
 * @brief Put one burst on the air: its address, its data, 30 ms. Lock held.
 * @return true when NimBLE took it.
 */
static bool fhn_burst_locked(uint8_t kind)
{
    const uint8_t *addr = s_fhn.addr;
    const uint8_t *data = s_fhn.adv;
    int            len  = (int)s_fhn.adv_len;
    uint8_t        own  = BLE_OWN_ADDR_RANDOM;

    if ( FHN_KIND_APPLE == kind )
    {
        addr = s_fhn.hay_addr;
        data = s_fhn.hay_adv;
        len  = (int)KFIND_HAYSTACK_ADV_LEN;
    }
    if ( BLE_OWN_ADDR_PUBLIC != s_ble.own_addr_type )
    {
        /* The board's identity is itself a random address: changing it would
         * change the console's. Google's burst goes out under the identity;
         * Apple's cannot (its address carries key bytes) and is skipped. */
        if ( FHN_KIND_APPLE == kind )
        {
            return false;
        }
        own = s_ble.own_addr_type;
    }
    else if ( (!s_fhn.rnd_valid) || (0 != memcmp(s_fhn.rnd, addr, KFIND_ADDR_LEN)) )
    {
        uint8_t le[KFIND_ADDR_LEN];

        if ( 0 != ble_gap_disc_active() )
        {
            /* The controller refuses a new random address while scanning.
             * Google's burst may go out under the last one; Apple's may not. */
            if ( (FHN_KIND_APPLE == kind) || (!s_fhn.rnd_valid) )
            {
                return false;
            }
        }
        else
        {
            for ( uint8_t i = 0U; KFIND_ADDR_LEN > i; ++i )
            {
                le[i] = addr[(KFIND_ADDR_LEN - 1U) - i];
            }
            if ( 0 != ble_hs_id_set_rnd(le) )
            {
                return false;
            }
            (void)memcpy(s_fhn.rnd, addr, KFIND_ADDR_LEN);
            s_fhn.rnd_valid = true;
        }
    }
    else
    {
        /* this address is set already */
    }

    struct ble_gap_adv_params p;

    (void)memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_NON;
    p.itvl_min  = (uint16_t)KDRV_BLE_FHN_ITVL;
    p.itvl_max  = (uint16_t)KDRV_BLE_FHN_ITVL;
    if ( (0 != ble_gap_adv_set_data(data, len))
         || (0 != ble_gap_adv_start(own, NULL, (int32_t)KDRV_BLE_FHN_BURST_MS, &p,
                                    &fhn_gap_event, NULL)) )
    {
        return false;
    }
    s_fhn.on_air = true;
    if ( FHN_KIND_APPLE == kind )
    {
        ++s_fhn.hay_sent;
    }
    else
    {
        ++s_fhn.sent;
    }
    return true;
}

static int fhn_gap_event(struct ble_gap_event *event, void *arg)
{
    bool idle = false;

    (void)arg;
    if ( BLE_GAP_EVENT_ADV_COMPLETE == event->type )
    {
        khal_mutex_lock(s_pulse_lock);
        if ( s_fhn.on_air )
        {
            s_fhn.on_air = false;
            if ( s_fhn.then_apple )
            {
                /* Google's burst is done: Apple's follows before the set goes back. */
                s_fhn.then_apple = false;
                if ( fhn_burst_locked(FHN_KIND_APPLE) )
                {
                    khal_mutex_unlock(s_pulse_lock);
                    return 0;
                }
                ++s_fhn.hay_skipped;
            }
            idle = fhn_give_back_locked();
        }
        khal_mutex_unlock(s_pulse_lock);
    }
    if ( idle )
    {
        /* What the console's own ADV_COMPLETE would have said. */
        ble_set_state(KDRV_BLE_IDLE);
    }
    return 0;
}

/** @brief One period: send the bursts the air allows. Host task. */
static void fhn_timer(struct ble_npl_event *ev)
{
    uint32_t next = fhn_period();
    bool     sent = false;
    bool     idle = false;

    (void)ev;
    khal_mutex_lock(s_pulse_lock);
    fhn_load_locked();
    if ( ((KFIND_MODE_OFF == s_fhn.cfg.mode) && (!s_fhn.hay_on)) || (!s_ble.synced) )
    {
        khal_mutex_unlock(s_pulse_lock);
        fhn_arm(next);   /* keeps looking: a config may come */
        return;
    }
    if ( s_fhn.on_air || s_fhn.cons_starting )
    {
        khal_mutex_unlock(s_pulse_lock);
        fhn_arm(FHN_RETRY_MS);
        return;
    }
    if ( s_pulse.live )
    {
        uint32_t const now = khal_time_ms();

        /* Two bursts may follow each other: leave room for both. */
        if ( s_pulse.on_air
             || (pulse_ms_left(s_pulse.due, now) < ((2U * KDRV_BLE_FHN_BURST_MS) + 30U)) )
        {
            khal_mutex_unlock(s_pulse_lock);
            fhn_arm(FHN_RETRY_MS);
            return;
        }
    }

    bool google = false;

    if ( KFIND_MODE_OFF != s_fhn.cfg.mode )
    {
        google = fhn_refresh_locked();
        if ( !google )
        {
            ++s_fhn.skipped;
        }
    }
    if ( (!google) && (!s_fhn.hay_on) )
    {
        khal_mutex_unlock(s_pulse_lock);
        fhn_arm(next);
        return;
    }

    bool const cons_cont = (!s_pulse.live) && (0 != ble_gap_adv_active());

    if ( cons_cont )
    {
        uint32_t const ran = khal_time_ms() - s_fhn.cons_t0;

        s_fhn.cons_left_ms = BLE_HS_FOREVER;
        if ( BLE_HS_FOREVER != s_fhn.cons_dur )
        {
            s_fhn.cons_left_ms = s_fhn.cons_dur - (int32_t)ran;
        }
        (void)ble_gap_adv_stop();
        s_fhn.paused = true;
    }

    s_fhn.then_apple = false;
    if ( google )
    {
        sent = fhn_burst_locked(FHN_KIND_GOOGLE);
        if ( sent )
        {
            s_fhn.then_apple = s_fhn.hay_on;
        }
        else
        {
            ++s_fhn.skipped;
        }
    }
    if ( (!sent) && s_fhn.hay_on )
    {
        sent = fhn_burst_locked(FHN_KIND_APPLE);
        if ( !sent )
        {
            ++s_fhn.hay_skipped;
        }
    }
    if ( !sent )
    {
        idle = fhn_give_back_locked();
    }
    khal_mutex_unlock(s_pulse_lock);
    if ( idle )
    {
        ble_set_state(KDRV_BLE_IDLE);
    }
    if ( !sent )
    {
        next = FHN_RETRY_MS * 4U;
    }
    fhn_arm(next);
}

/** @brief The host came up (kdrv_ble_start()): arm the beacon's period. */
static void fhn_host_up(void)
{
    s_fhn.on_air        = false;
    s_fhn.paused        = false;
    s_fhn.cons_starting = false;
    s_fhn.rnd_valid     = false;
    (void)memset(&s_fhn.co, 0, sizeof(s_fhn.co));
    (void)ble_npl_callout_init(&s_fhn.co, nimble_port_get_dflt_eventq(), &fhn_timer, NULL);
    s_fhn.inited = true;
    fhn_arm(fhn_period());
}

/** @brief The controller reset under the host: nothing of ours is on the air. */
static void fhn_host_reset(void)
{
    s_fhn.on_air   = false;
    s_fhn.paused   = false;
}

/** @brief The host is going (kdrv_ble_stop()): the callout goes with its pool. */
static void fhn_host_down(void)
{
    if ( s_fhn.inited )
    {
        ble_npl_callout_stop(&s_fhn.co);
    }
    s_fhn.inited = false;
    s_fhn.on_air = false;
    s_fhn.paused = false;
}

void kdrv_ble_fhn_status(kdrv_ble_fhn_status_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    (void)memset(out, 0, sizeof(*out));
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_lock(s_pulse_lock);
    }
    fhn_load_locked();
    out->mode       = s_fhn.cfg.mode;
    out->utp        = s_fhn.cfg.utp;
    out->clock_wall = s_fhn.clock_wall;
    out->beacon_t   = s_fhn.beacon_t;
    out->slot       = s_fhn.slot;
    if ( KFIND_MODE_ROTATING == s_fhn.cfg.mode )
    {
        uint32_t const due = s_fhn.slot + KFIND_ROTATION_S + s_fhn.jitter;

        out->next_s = (due > s_fhn.beacon_t) ? (due - s_fhn.beacon_t) : 0U;
    }
    (void)memcpy(out->eid, s_fhn.eid, KFIND_EID_LEN);
    (void)memcpy(out->addr, s_fhn.addr, KFIND_ADDR_LEN);
    out->batt    = s_fhn.batt;
    out->sent    = s_fhn.sent;
    out->skipped = s_fhn.skipped;
    out->host_up = s_ble.synced;
    out->hay_on  = s_fhn.hay_on;
    (void)memcpy(out->hay_addr, s_fhn.hay_addr, KFIND_ADDR_LEN);
    out->hay_sent    = s_fhn.hay_sent;
    out->hay_skipped = s_fhn.hay_skipped;
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_unlock(s_pulse_lock);
    }
}

#else /* !CONFIG_KODEOS_FINDHUB */

static inline void fhn_yield_locked(void) { }
static inline void fhn_note_console(const struct ble_hs_adv_fields *f) { (void)f; }
static inline void fhn_note_console_start(const struct ble_gap_adv_params *p, int32_t d) { (void)p; (void)d; }
static inline void fhn_console_starting(void) { }
static inline void fhn_host_up(void) { }
static inline void fhn_host_down(void) { }
static inline void fhn_host_reset(void) { }

#endif /* CONFIG_KODEOS_FINDHUB */
