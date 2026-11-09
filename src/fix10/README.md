# W737: why the screen is black, and how it is fixed without patching mainline

Date: 2026-11-09. Samsung Galaxy Book2 (SM-W737), SDM850, Debian forky/sid,
kernel `6.12.107+deb13-arm64`, device-tree boot.

## The problem

The TI SN65DSI86 bridge register `0x10` (`SN_DSI_LANES_REG`) describes the DSI
channel layout:

| bit(s) | field | meaning |
|---|---|---|
| 7 | `LEFT_RIGHT_PIXELS` | pixel order |
| 6:5 | `DSI_CHANNEL_MODE` | `00` = dual, `01` = single channel A |
| 4:3 | `CHA_DSI_LANES` | lane count on channel A |
| 2:1 | `CHB_DSI_LANES` | lane count on channel B |

UEFI leaves it at `0x80`: `LEFT_RIGHT_PIXELS = 1`, **dual** mode. The mainline
`ti-sn65dsi86` driver does this in `ti_sn_bridge_pre_enable()`:

```c
val = CHA_DSI_LANES(SN_MAX_DP_LANES - pdata->dsi->lanes);
regmap_update_bits(pdata->regmap, SN_DSI_LANES_REG, CHA_DSI_LANES_MASK, val);
```

that is, it touches **only bits 4:3**, and bits 7:5 keep the firmware value. The
panel then shows nothing — the image appears only if the whole register is
rewritten:

```
0x10 = 0x26   ; 0010 0110b
             ;   bit7  = 0   LEFT_RIGHT off
             ;   bits6:5 = 01  single channel A
             ;   bits4:3 = 00  4 lanes
             ;   bits2:1 = 11  (channel-B selector, unused in single mode)
```

Verified: writing `0x10 = 0x26` on a running system brings the picture back
immediately. After a reboot UEFI sets `0x80` again and the screen is black.

## Why not a driver patch

- The driver rewrites `0x10` on **every** enable (chip reset → re-init), so a
  one-shot fix at boot would be overwritten by any modeset or resume.
- The value has to be held by a watcher, not written once.

## The solution: `w737-fix10`

A small daemon (~150 lines of C, `i2c-dev` only, no libi2c) that reads `0x10`
through `/dev/i2c-3` (address `0x2c`) every 400 ms and writes it back if it
differs from `0x26`. Every correction goes to the journal.

Why userspace rather than a kernel module:

- a second i2c client on `0x2c` cannot be registered — the address belongs to
  `ti_sn65dsi86` (`-EBUSY`);
- a custom module would send the same two bytes through `i2c_transfer`, doing
  exactly the same work but needing a rebuild for every kernel.

## Installation

```sh
gcc -O2 -Wall -Wextra -o /tmp/w737-fix10 w737-fix10.c
install -m 0755 /tmp/w737-fix10 /usr/local/sbin/w737-fix10
install -m 0644 w737-fix10.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now w737-fix10.service
```

Check:

```sh
systemctl status w737-fix10
journalctl -u w737-fix10 -b | tail
```

Disable:

```sh
systemctl disable --now w737-fix10.service
```

## Beyond the bridge

- One DSI channel instead of two: half the bandwidth of dual mode. The current
  layout (4 lanes, `0x12 = 0x7B` ≈ 615 MHz) is enough for 2160×1440@60, but there
  is little headroom. Full dual operation is a separate task (~100–150 lines of
  driver work plus DT changes).
- AMOLED brightness is handled by a separate component; see
  `src/brightness/README.md`.
