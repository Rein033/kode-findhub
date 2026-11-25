# Bluetooth 6.0 Channel Sounding with an RF module

Status: **design only, phase 2.** Nothing here is built or tested. It needs
hardware that does not exist for the Kode Dot yet.

## Why it does not run on the Dot as it is

Channel Sounding (CS) is part of the Bluetooth Core 6.0 link layer. The
**controller** (the radio chip) must support it. The host cannot add it.

| Radio | What it is | Channel Sounding |
|---|---|---|
| ESP32-C5 (built in) | Wi-Fi 6 + BLE 5 controller | no |
| Kode "Hacking module" | Wi-Fi 6, sub-GHz, dual nRF24L01 | no (nRF24 is not a BLE controller) |
| Kode LoRa / Meshtastic module | sub-GHz LoRa | no |
| **nRF54L15 / nRF54L10** (Nordic) | BLE 6.0 controller | **yes**, with nRF Connect SDK 2.9 or later |
| EFR32xG24 / xG26 (Silicon Labs) | BLE controller | yes, with Simplicity SDK |

So channel sounding needs an **RF module with a CS-capable chip** on the
expansion header (J3), for example an nRF54L15 module. Examples: Raytac
MDBT54Q, Seeed XIAO nRF54L15, Nordic nRF54L15-DK for development.

## What it gives you

CS measures the **distance** between two BLE devices, typically to 10-50 cm.
It uses phase-based ranging (PBR) over up to 72 channels, plus round-trip
time (RTT). It gives no direction. One side is the **initiator** and the
other is the **reflector**. The two devices must be bonded first.

Uses for a small device that you lose:

1. **Finding the Dot at close range.** Google's Find Hub "Precision Finding"
   lists BLE CS as a ranging method (spec data IDs 0x0A-0x0D). Today only
   accessories that Google certified get it, so this is the long-term goal.
2. **Dot to Dot, or Dot to a phone.** Android 16 has a ranging API
   (`android.ranging`) that does BLE CS on phones whose controller supports
   it. A small companion app (initiator) can range to the Dot's module
   (reflector) and show "1.4 m".
3. **Dot to any CS tag.** The Dot (initiator) shows the distance to another
   nRF54L15 board on its screen.

## How it fits next to Find Hub

```
            +------------------- Kode Dot -------------------+
  phone <-- | ESP32-C5: Find Hub beacon + console  (BLE 5)    |
            |                                                |
  phone <-->| J3 UART -- RF module (nRF54L15, BLE 6.0)       |
  / tag     |            Channel Sounding initiator/reflector|
            +------------------------------------------------+
```

The module runs its own BLE stack (Zephyr in nRF Connect SDK) and does the
whole CS procedure itself: bonding, CS configuration, the procedure, and the
distance estimate with Nordic's ranging library. The Dot only sends commands
and shows results. There is no second HCI host on the P4. This keeps the
Dot's BLE stack (NimBLE plus the C5) unchanged. The ESP-IDF v6.1 NimBLE host
has no stable CS support.

## The UART protocol (J3, 115200 8N1, lines ending in `\n`)

Dot to module:

| Line | Meaning |
|---|---|
| `hello` | Identify. Answer: `#hello fw=<version> cs=1 roles=init,refl` |
| `role init` / `role refl` | Become the initiator or the reflector |
| `scan ms=<n>` | Initiator: list CS-capable devices. Answers: `#dev addr=<aa:bb:..> rssi=<dBm> name="<n>"` then `#end` |
| `pair addr=<aa:bb:..>` | Bond with that device. Answer: `#paired` or `#err why=<word>` |
| `range addr=<aa:bb:..> hz=<1..10>` | Start ranging. A `#dist` line per procedure |
| `stop` | Stop ranging or advertising |
| `adv on` / `adv off` | Reflector: be connectable for an initiator |

Module to Dot, while ranging:

```
#dist mm=1432 pbr_mm=1420 rtt_mm=1610 q=good n=72
#lost why=timeout
```

`q` is `good`, `fair` or `poor`, from the module's quality estimate. `n` is
the number of channels used.

## The plan

1. **Module firmware** (`cs/module-fw/`): nRF Connect SDK samples
   `channel_sounding_ras_initiator` and `channel_sounding_ras_reflector`,
   joined into one image with a role switch, plus a UART line bridge
   (`uart_bridge.c`) that speaks the protocol above. Build with
   `west build -b nrf54l15dk/nrf54l15/cpuapp`.
2. **The Dot's side, as an app** ("Range"): it opens J3's UART with
   `kode_sys_exp_ctrl()`, shows the distance large on the screen, and
   buzzes faster as you get closer. It is an app rather than part of kodeOS
   because J3's pins are lent to apps (khal_exp). It uses the same calls as
   [Dot Desktop](https://github.com/Rein033/dot-desktop).
3. **Later, in kodeOS**: when kodeOS can own a module on J3 at boot, the OS
   can answer Find Hub's ranging requests (0x0A-0x0D) with the module as the
   CS reflector. That also needs the Fast Pair GATT service (Beacon Actions)
   in kodeOS, which the beacon in this repository does not have yet.

## What to buy to start

* 1x nRF54L15-DK (Nordic, about EUR 40), for the module firmware.
* For phone tests: an Android 16 phone whose Bluetooth controller supports
  CS. Check `BluetoothAdapter.isChannelSoundingSupported` or
  `RangingManager` capabilities. Support depends on the model.
