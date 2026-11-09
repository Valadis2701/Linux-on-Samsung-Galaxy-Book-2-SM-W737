# AMOLED brightness on the Galaxy Book2 (SM-W737) under Linux

Date: 2026-11-09. Result: brightness is controlled through the standard GNOME
system slider.

## Why it did not work at first

The panel has neither PWM nor a backlight controller. Brightness is a set of TCON
registers sent over DP AUX through the SN65DSI86 bridge. Applying the tables from
the Galaxy Book 12 project (Teetoow/SamsungGalaxyBook12) produced a purple and
then a brown screen: the write sequence matched, but the **table data did not**.
The key table `valuesRegCPerBrightness` (slot `9B`) differs on 97 of 102 levels on
the W737.

The engine is therefore built on the **native** tables extracted from
`SAMOPanelARM.sys` — see `docs/ANALYSIS-panel.md` and
`tools/extract_w737_tables.py`.

## What the solution consists of

| Component | File | Role |
|---|---|---|
| `w737-fix10` | `src/fix10/` | holds `0x10 = 0x26`, otherwise the screen is black |
| `w737-brightness` | `setbrit.c` + `brightness.c` + native tables | computes and sends the brightness payload |
| `w737-brightnessd` | `brightnessd.c` | caches the panel calibration so an update costs ~100 ms instead of 1.4 s |
| `w737-brightness-set` | wrapper | single entry point, persists the level in `/var/lib/w737-brightness/level` |
| `w737-backlight.ko` | `src/backlight/` | creates `/sys/class/backlight/w737`, which the system slider drives |

Flow: GNOME → logind → `brightness` → `w737-backlight` → `w737-brightness-set`
→ `/run/w737-brightness/request` → `w737-brightnessd` → DP AUX → panel.

## Traps we already hit

1. **Re-sending the `PARAAUX-REG` handshake resets the gateway**: every `0x493`
   read starts returning zero. Send it only when `0x490 == 0`.
2. **`open()` under `call_usermodehelper` returns fd 0** (stdin is closed), and the
   code tested `if (!fd)`. Hence "open: Success" and exit code 1. Test `< 0`, and
   also pin fd 0 in the wrapper with `</dev/null`.
3. **Never read the calibration blindly**: if `reg2_26 == 0`, the data is garbage —
   the daemon re-initialises.
4. **The Book 12 tables do not fit**; `9B` differs on almost every level.
5. `0x490..0x493` is a command interface, not storage: reading back "what was
   written" is meaningless.

## Installing from scratch

```sh
# engine
cd /tmp/gb12w && gcc -O2 -o w737-brightnessd brightnessd.c brightness.c dpAux.c tables.c -lm
install -m 0755 w737-brightnessd /usr/local/sbin/w737-brightnessd
install -m 0755 setbrit_w737        /usr/local/sbin/w737-brightness
install -m 0755 w737-brightness-set /usr/local/sbin/w737-brightness-set
install -m 0644 w737-brightnessd.service w737-brightness.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now w737-brightnessd w737-brightness

# backlight shim
cd /tmp/w737bl && make -C /lib/modules/$(uname -r)/build M=$PWD modules
install -m 0644 w737-backlight.ko /lib/modules/$(uname -r)/updates/
depmod -a && modprobe w737-backlight
echo w737-backlight > /etc/modules-load.d/w737-backlight.conf

# access without root
install -m 0644 60-w737-drm-dp-aux.rules /etc/udev/rules.d/
install -d -m 0775 -g video /var/lib/w737-brightness
```

## Verification

```sh
cat /sys/class/backlight/w737/{brightness,max_brightness}
journalctl -u w737-brightnessd -b | tail
echo 30 > /sys/class/backlight/w737/brightness   # must change brightness
cat /run/w737-brightness/applied
```

## Limitations

- Bandwidth headroom: one DSI channel, 4 lanes, 615 MHz. Dual mode is a separate
  task (~100–150 lines of driver work plus DT changes).
- The `w737-brightness-gui` window remains as a standalone tool; it is no longer
  needed for everyday use.
