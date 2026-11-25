/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file findhub_verbs.c
 * @brief The Find Hub beacon's console verbs: `findhub_set`, `findhub_off`
 *        and `findhub_info`, registered by kaos_radio_start().
 *
 *   findhub_set eik="<64 hex>" pair_date=<unix s> [utp=on|off]   rotating EIDs
 *   findhub_set key="<40 hex>" [utp=on|off]                      one fixed EID
 *   findhub_off                                                  forget, stop
 *   findhub_info                                                 what is on the air
 *   findmy_set key="<56 hex>"     Apple Find My, OpenHaystack advertisement key
 *   findmy_off                    stop the Apple advert
 *
 * The key is a secret: `findhub_set` answers only on the USB cable, never on
 * BLE or MQTT, and `findhub_info` never prints it.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kdrv_ble_fhn.h"
#include "kfind.h"

#include "console_verbs.h"
#include "kverbs.h"
#include "kqp.h"

/** @brief Last Unix second a pairing date may name: 2106, where a u32 ends. */
#define FHV_DATE_MAX  0xFFFFFFFFUL

static const karg_spec_t A_FINDHUB_SET[] = {
    { .key = "eik",       .required = false, .type = KARG_STRING },
    { .key = "pair_date", .required = false, .type = KARG_UINT, .min = 1U, .max = FHV_DATE_MAX },
    { .key = "key",       .required = false, .type = KARG_STRING },
    { .key = "utp",       .required = false, .type = KARG_BOOL, .def = "on" },
};

static const karg_spec_t A_FINDMY_SET[] = {
    { .key = "key", .required = true, .type = KARG_STRING },
};

static const char *const FHV_MODE[] = { "off", "static", "rotating" };
static const char *const FHV_BATT[] = { "none", "normal", "low", "critical" };

/** @brief findhub_set: keep a key and start beaconing. */
static void h_findhub_set(const kverb_t *c, const karg_val_t *v)
{
    kdrv_ble_fhn_cfg_t cfg;
    bool const has_eik = v[0].present;
    bool const has_key = v[2].present;

    (void)c;
    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.utp = (!v[3].present) || v[3].b;
    if ( has_eik == has_key )
    {
        console_reply_err(KCLI_ERR_MISSING, "eik_or_key");
        return;
    }
    if ( has_eik )
    {
        if ( !v[1].present )
        {
            console_reply_err(KCLI_ERR_MISSING, "pair_date");
            return;
        }
        if ( !kfind_hex(v[0].s, cfg.eik, KFIND_EIK_LEN) )
        {
            console_reply_err(KCLI_ERR_VALUE, "eik");
            return;
        }
        cfg.mode      = KFIND_MODE_ROTATING;
        cfg.pair_date = v[1].u;
    }
    else
    {
        if ( !kfind_hex(v[2].s, cfg.key, KFIND_EID_LEN) )
        {
            console_reply_err(KCLI_ERR_VALUE, "key");
            return;
        }
        cfg.mode = KFIND_MODE_STATIC;
    }
    kode_status_t const rc = kdrv_ble_fhn_configure(&cfg);

    (void)memset(&cfg, 0, sizeof(cfg));
    if ( KODE_OK != rc )
    {
        console_reply_err(KCLI_ERR_REJECT, "store");
        return;
    }
    console_reply(">ok");
}

/** @brief findhub_off: forget the key; the beacon stops at its next period. */
static void h_findhub_off(const kverb_t *c, const karg_val_t *v)
{
    kdrv_ble_fhn_cfg_t cfg;

    (void)c;
    (void)v;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.mode = KFIND_MODE_OFF;
    if ( KODE_OK != kdrv_ble_fhn_configure(&cfg) )
    {
        console_reply_err(KCLI_ERR_REJECT, "store");
        return;
    }
    console_reply(">ok");
}

/** @brief findhub_info: one record of what goes on the air. */
static void h_findhub_info(const kverb_t *c, const karg_val_t *v)
{
    kdrv_ble_fhn_status_t st;
    char eid[(2U * KFIND_EID_LEN) + 1U];
    char addr[18];

    (void)c;
    (void)v;
    kdrv_ble_fhn_status(&st);
    (void)console_hex_text(eid, sizeof(eid), st.eid, KFIND_EID_LEN);
    (void)snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                   st.addr[0], st.addr[1], st.addr[2], st.addr[3], st.addr[4], st.addr[5]);
    if ( KFIND_MODE_OFF == st.mode )
    {
        (void)snprintf(eid, sizeof(eid), "-");
        (void)snprintf(addr, sizeof(addr), "-");
    }
    console_reply(">|findhub mode=%s utp=%s host=%s clock=%s beacon_t=%lu slot=%lu"
                  " next_s=%lu eid=%s addr=%s batt=%s sent=%lu skipped=%lu",
                  console_word(FHV_MODE, Q_DIM(FHV_MODE), (uint32_t)st.mode),
                  console_on_off(st.utp), st.host_up ? "up" : "down",
                  st.clock_wall ? "wall" : "kept",
                  (unsigned long)st.beacon_t, (unsigned long)st.slot,
                  (unsigned long)st.next_s, eid, addr,
                  console_word(FHV_BATT, Q_DIM(FHV_BATT), st.batt),
                  (unsigned long)st.sent, (unsigned long)st.skipped);
    (void)snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x", st.hay_addr[0],
                   st.hay_addr[1], st.hay_addr[2], st.hay_addr[3], st.hay_addr[4], st.hay_addr[5]);
    console_reply(">|findmy on=%s addr=%s sent=%lu skipped=%lu", console_on_off(st.hay_on),
                  st.hay_on ? addr : "-", (unsigned long)st.hay_sent, (unsigned long)st.hay_skipped);
    console_reply(">ok");
}

/** @brief findmy_set: send Apple's offline-finding advert for an OpenHaystack key. */
static void h_findmy_set(const kverb_t *c, const karg_val_t *v)
{
    uint8_t key[KFIND_HAYSTACK_KEY_LEN];

    (void)c;
    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    if ( !kfind_hex(v[0].s, key, sizeof(key)) )
    {
        console_reply_err(KCLI_ERR_VALUE, "key");
        return;
    }
    if ( KODE_OK != kdrv_ble_fhn_haystack(key) )
    {
        console_reply_err(KCLI_ERR_REJECT, "store");
        return;
    }
    console_reply(">ok");
}

/** @brief findmy_off: stop the Apple advert and forget its key. */
static void h_findmy_off(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    (void)v;
    if ( KODE_OK != kdrv_ble_fhn_haystack(NULL) )
    {
        console_reply_err(KCLI_ERR_REJECT, "store");
        return;
    }
    console_reply(">ok");
}

static const kverb_t FINDHUB_VERBS[] = {
    { "findhub_set",  A_FINDHUB_SET, KVERB_KEYS(A_FINDHUB_SET), h_findhub_set  },
    { "findhub_off",  NULL,          0U,                        h_findhub_off  },
    { "findhub_info", NULL,          0U,                        h_findhub_info },
    { "findmy_set",   A_FINDMY_SET,  KVERB_KEYS(A_FINDMY_SET),  h_findmy_set   },
    { "findmy_off",   NULL,          0U,                        h_findmy_off   },
};

const kverbs_t findhub_verbs = {
    .verbs = FINDHUB_VERBS, .n = sizeof(FINDHUB_VERBS) / sizeof(FINDHUB_VERBS[0]),
};
