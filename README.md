# Linux on Samsung Galaxy Book 2 (SM-W737)

Notes and working code from running Debian on a Samsung Galaxy Book2 tablet
(SM-W737NZSBKOO, Qualcomm Snapdragon 850). The device boots from a device tree
blob rather than ACPI, and the internal AMOLED panel has no PWM backlight and no
ACPI video opregion, so the display path had to be built from scratch and
reverse-engineered from vendor firmware.

This document describes what currently works and how each piece is put
together. It is a living document: components are added as they start working.

## Working components

| Component | Status | Notes |
|---|---|---|
| Display bridge (TI SN65DSI86) | **Working** | 2160×1440 internal AMOLED over a single DSI channel; brightness controlled from the GNOME slider |
| Audio (WCD9340 + 2x WSA8815) | **Working** | speakers play; ADSP is brought up by a unit after `local-fs.target`, routing comes from an ALSA UCM profile, treble is tamed by a PipeWire EQ |

## Components in detail

### Display bridge — TI SN65DSI86

#### Topology

```
SDM850 DPU ── DSI0, 4 lanes ──> SN65DSI86 ── eDP ──> Samsung SDCA029 AMOLED
                                i2c 3-002c          2160×1440, 60 Hz
                                IRQ  TLMM GPIO 29
```

The bridge is a DSI-to-eDP converter. It is a plain I2C device on bus `i2c-3`
(address `0x2c`) and exposes a DP AUX channel, which on Linux appears both as
`/dev/drm_dp_aux0` and as the panel's command interface.

Panel identity, read back from EDID: manufacturer `SDC`, product `0xA029`,
"Color LCD", 25×17 cm, EDID 1.4, 2160×1440 at 206.57 MHz pixel clock, 60.00 Hz.

#### Why the panel stayed black

Register `0x10` of the bridge (`SN_DSI_LANES_REG`) describes the DSI channel
layout:

| bits | field | meaning |
|---|---|---|
| 7 | `LEFT_RIGHT_PIXELS` | pixel order |
| 6:5 | `DSI_CHANNEL_MODE` | `00` = dual channel, `01` = single channel A |
| 4:3 | `CHA_DSI_LANES` | lane count on channel A |
| 2:1 | `CHB_DSI_LANES` | lane count on channel B |

UEFI leaves the register at `0x80`: `LEFT_RIGHT_PIXELS = 1` and **dual**
channel mode. The mainline `ti-sn65dsi86` driver only updates bits 4:3 in
`ti_sn_bridge_pre_enable()`:

```c
val = CHA_DSI_LANES(SN_MAX_DP_LANES - pdata->dsi->lanes);
regmap_update_bits(pdata->regmap, SN_DSI_LANES_REG, CHA_DSI_LANES_MASK, val);
```

so bits 7:5 keep whatever the firmware left behind and the panel never gets a
usable stream. Writing the whole register fixes it immediately:

```
0x10 = 0x26   0010 0110b
        bit7    = 0    LEFT_RIGHT_PIXELS off
        bits6:5 = 01   DSI_CHANNEL_MODE = single channel A
        bits4:3 = 00   4 lanes on channel A
        bits2:1 = 11   channel-B selector (unused in single mode)
```

This is the single most important finding: the panel is not broken, the
register is just half-programmed.

#### Device tree

The stock DT does not describe this panel at all, and two details are easy to
get wrong:

- The bridge node originally sat on `i2c@a88000`, which is the wrong controller.
  That bus hosts a SAR sensor that also answers at `0x2c`, so the driver talked
  to the wrong device and logged `Unexpected max rate (0x0)` followed by
  `DP_PLL_LOCK polling failed (-110)`. In ACPI the panel's `SAMO` device
  (`SAM0101`) is on **I2C4**, i.e. `i2c@88c000`, so the node was moved there and
  the bus enabled at 400 kHz.
- `msm` looks for the downstream bridge on **port@1** of the DSI controller
  node. Wiring the output to `port@1` of `dsi@ae94000` was required to get past
  `failed to bind ae94000.dsi (ops dsi_ops [msm]): -19`.

The resulting DT also drops the dual-DSI properties, disables the second DSI
controller, adds `interrupts-extended = <&tlmm 29 1>`, and adds a simple
`panel-dpi` node (`data-mapping = "jeida-24"`) matching the panel timings.

#### Keeping `0x10` correct: `w737-fix10`

The bridge driver rewrites `0x10` on **every** enable — the chip is reset and
re-initialised, and the firmware value comes back. A one-shot fix at boot would
therefore be undone by the first modeset or resume, so the value has to be
watched, not written once.

`w737-fix10` is a ~150-line userspace daemon that opens `i2c-3`, reads `0x10`
every 400 ms and writes `0x26` whenever it differs. It deliberately does **not**
patch the mainline driver: the same two bytes are reachable through `i2c-dev`,
and a separate kernel module would have needed its own handling of the same
race.

Verified behaviour: exactly one correction per boot, within ~1.5 s of the
bridge probing:

```
w737-fix10[859]: 0x10: 0x80 -> 0x26 (fix #1)
```

#### The DP AUX command interface

Registers `0x490..0x493` are a **command window into the panel's TCON**, not
storage. Reading them back after a write returns the panel's idle pattern
(`01 07 32 00`) regardless of what was written, so "read back what I wrote" is
meaningless here.

Access is gated by a handshake: the ASCII string `PARAAUX-REG` written byte by
byte to `0x490`. The gateway is armed when a read of `0x490` returns non-zero.

**Trap:** the handshake must be sent only when the gateway is idle. Re-sending it
while armed silently resets the gateway, after which every `0x493` read returns
zero — which looks exactly like a panel that has no calibration data. This cost
a full debugging cycle and is now guarded in code.

Reading a TCON register:

```
0x491 <- 02 7F 01     gateway preamble
0x491 <- 07 FF 04     gateway preamble
0x492 <- 00 <bank>    channel: 0x02 / 0x04 / 0x08
0x492 <- 01 <half>    0x00 / 0x80
0x492 <- 02 <reg>     register 0..5
0x491 <- 07 0C        latch
read 0x493            value
```

Registers `0x26..0x29` use a shorter path: `0x492 <- 03 <type>`,
`0x492 <- 06 <reg>`, `0x492 <- 0A`, then read `0x493`.

#### Panel calibration

The 33 calibration registers (11 per channel) read back as real, channel-
independent data:

```
bank 0x02: c2 e1 df cc c5 d9 de 2b 8e 80 00
bank 0x04: ce …
bank 0x08: c9 …
0x26..0x29, type 2: 0b ac 0c 49
0x26..0x29, type 4: 06 24 00 00     -> reg2_26 = 0x06
```

`reg2_26` is written back into the brightness payload, so a zero value there is
a reliable symptom of a gateway reset rather than a real reading.

#### Brightness

Brightness on this panel is a TCON register table sent over the same AUX
channel; there is no backlight controller to drive. The protocol — and a working
open-source implementation — comes from the closely related
[Galaxy Book 12 project](https://github.com/Teetoow/SamsungGalaxyBook12/tree/main/brightness).
The `SAMOPanelARM.sys` Windows driver for this tablet (extracted from the
firmware dump) implements the same engine, which made it possible to verify the
protocol byte for byte.

A brightness update is a fixed sequence:

| step | payload |
|---|---|
| 1 | 9 records: `0F FF 00`, `9D`, `9B`, `0F FF 02`, `30`, `33`, `0E 80 90`/`80`, `02 7F 01`, `07 FF 04` |
| 2 | 33 writes `0x492 <- <0x10+i> <value>` (the per-channel gamma table) |
| 3 | 1 ms pause |
| 4 | `0x492 <- 32 01` |
| 5 | 4 records: `02 EB 91`, `09 F0 EF`, `F9 <ramp>`, `F8 19` |
| 6 | 10 records: `02 EB 96`, `0E 10 <v>`, `11..18 <v>` |

**The sequence is identical to the Book 12 reference; the table contents are
not.** Comparing the vendor driver against the reference showed that
`valuesRegCPerBrightness` — the `9D`/`9B` pair — differs on 97 of 102 brightness
levels:

```
level   W737 (9D,9B)   Book 12 (9D,9B)
    0     (0B,68)        (0B,60)
   25     (08,8B)        (08,70)
   50     (04,94)        (04,30)      <-- 2.5x apart
   75     (01,FB)        (01,A8)
  100     (00,70)        (00,70)      <-- identical at maximum
```

Using the reference tables therefore produced a purple, then brown screen at
unchanged brightness: correct protocol, wrong colour-space coefficients. The
fix was to extract the tables from the vendor driver and build the payload from
those instead. With the native tables, brightness tracks the requested level
and colour stays correct.

#### System integration

So that the panel behaves like a normal laptop display, the chain is:

```
GNOME slider
   │  org.freedesktop.login1.Session.SetBrightness
   ▼
/sys/class/backlight/w737/brightness     ← w737-backlight.ko
   │  call_usermodehelper, 15 ms coalescing
   ▼
/usr/local/sbin/w737-brightness-set      ← wrapper, persists the level
   │  writes /run/w737-brightness/request
   ▼
/usr/local/sbin/w737-brightnessd         ← daemon, calibration cached in RAM
   │  setBrightness() → ~57 AUX writes
   ▼
/dev/drm_dp_aux0 → SN65DSI86 → panel TCON
```

The small kernel module only creates the backlight class device and forwards
requests; all protocol logic stays in userspace. The daemon exists because
initialising the panel (handshake + 33 calibration reads) costs ~1.3 s, while an
update costs ~100 ms. Caching the calibration, as the Windows driver does, makes
dragging the slider feel immediate.

#### Gotchas worth remembering

1. **Never re-send the `PARAAUX-REG` handshake while the gateway is armed** — it
   resets the gateway and all reads return zero.
2. **`open()` under `call_usermodehelper` can return fd 0** because stdin is
   closed. The reference code tested `if (!fd)` and reported `open: Success`
   with exit code 1 on a perfectly healthy device. Test `< 0`, and pin fd 0 in
   the wrapper with `</dev/null`.
3. **Treat `reg2_26 == 0` as a failed init**, not as data.
4. **The Book 12 tables are not this panel's tables.** Protocol yes, coefficients
   no.
5. **`0x490..0x493` is a command interface**, not memory.
6. **DPMS and `xset` do not work here** — the compositor holds DRM master and
   the kernel returns `EACCES`, so recovering a confused TCON means a reboot.


#### Known limitations

- **Single DSI channel.** The bridge runs one channel with 4 lanes at 615 MHz
  (`0x12 = 0x7B`). 2160×1440 at 60 Hz fits, but there is no headroom. True
  dual-channel operation needs roughly 100–150 lines of driver work plus DT
  changes.


### Audio — WCD9340 + 2x WSA8815

#### Topology

```
MultiMedia1 (the only PCM, hw:0,0)
   │  Q6 ASM frontend on the ADSP
   ▼
SLIMBUS_0_RX ── SLIMbus ──> WCD9340 ── SLIM RX0/RX1 ── RX INT7_1 / RX INT8_1
                                                   └─ SPK1/SPK2 OUT
                                                        │ analog
                                                        ▼
                                             WSA8815 left / right ──> speakers
                                             (SoundWire: sdw:0:0:0217:2110:00:3, …:00:4)
```

The card is `Book2` (`snd_soc_sdm845`), and it exposes exactly **one** PCM,
`MultiMedia1`. The SLIMbus link is a DPCM backend, so it has to be routed with
the mixer before the PCM can even be opened.

#### Why the card never appears on a clean install

`qcom_q6v5_pas` is a module and Debian keeps modules in the initramfs
(`MODULES=most`). udev loads it there and the driver probes ~130 ms **before the
root filesystem is mounted**:

```
00:15:26.437678  Run /init as init process
00:15:26.454409  remoteproc-adsp is available
00:15:26.458145  Direct firmware load for qcom/sdm850/samsung/w737/qcadsp850.mbn failed with error -2
00:15:26.585408  EXT4-fs (sdb3): mounted filesystem 265893f1-…
```

The firmware is on disk and readable — just not yet, so the ADSP stays `offline`
and the machine driver registers no card. initramfs-tools cannot know about it
either: the driver takes the file name from the device tree (`firmware-name`) and
declares no `MODULE_FIRMWARE()` (the GPU firmware *is* in the initramfs for the
opposite reason — `msm` declares its files).

**Trap:** copying `qcadsp850.mbn` into the initramfs "fixes" the `-2` and bricks
the boot. The ADSP then starts inside the initramfs, in the middle of udev
coldplug, and the root is never mounted: black screen, nothing in the journal, no
pstore. We hit this and recovered through a live USB (the installer carries no
`ext4.ko`; it has to be loaded from the stick together with `mbcache`, `jbd2` and
`crc16`), restoring the previous `/boot/initrd.img-…`.

The working fix is the opposite: start the ADSP **after** the root is mounted.

```sh
# units/gb2-adsp.service + bin/gb2-adsp-start.sh
After=local-fs.target
Before=sound.target display-manager.service
ExecStart=/usr/local/sbin/gb2-adsp-start.sh
```

The script writes `start` into `/sys/class/remoteproc/remoteproc*/state`, waits
for `running`, then waits for the card; if the machine driver probed while the
ADSP was still down, it re-probes `snd-soc-sdm845` once. Log on a healthy boot:

```
gb2-adsp-start.sh: adsp: running
gb2-adsp-start.sh: sound: card0=Book2
```

#### Routing and volume

```
MultiMedia1 -> "SLIMBUS_0_RX Audio Mixer MultiMedia1" 1
            -> SLIM RX0/RX1 MUX = AIF1_PB
            -> RX INT7_1 / RX INT8_1 MIX1 INP0 = RX0 / RX1
            -> SpkrLeft/Right DAC, COMP, BOOST switches, PA Volume 9/12
```

Kernel log during playback confirms the AFE map:

```
qcom-q6afe …: slim cfg v2: dev_id 0 rate 48000 bw 16 ch 2 fmt 0 map 144 145 0 0
```

**Trap:** the codec has separate mono gains per speaker (`RX7` for left, `RX8`
for right). Declaring `PlaybackVolume "RX7 Digital Volume"` in the UCM profile
makes PipeWire's slider drive only the left channel — the result is +27 dB on the
left and a badly unbalanced pair. The profile therefore declares no
`PlaybackVolume` at all: PipeWire does the volume in software, the codec stays at
0 dB (`RX7 = RX8 = 84`), the analog PA sits at 9/12 (13.5 dB).

#### Treble: why an EQ

Windows shaped these speakers with the Qualcomm ACDB calibration
(`Speaker_cal.acdb`, opaque CLID tables) running on the **WDSP**
(`qcauddev850.sys` loads `qcwdsp850.mbn`), and with a Dolby Atmos profile whose
graphic EQ is disabled anyway (`graphic-equalizer-enable 0`, all bands zero).
None of that is reachable from mainline, so the Linux path is flat and audibly
brighter.

`units/pipewire/80-book2-eq.conf` inserts a PipeWire `filter-chain` in front of
the hardware sink (a virtual sink `effect_input.book2_eq`, set as default):

| Band | Freq | Q | Gain |
|---|---|---|---|
| low-shelf | 150 Hz | 0.7 | 0 dB |
| high-shelf | 3.5 kHz | 0.7 | −5 dB |
| peak | 6.5 kHz | 1.2 | −3 dB |
| peak | 10 kHz | 1.0 | −2 dB |

That is −3.5 dB at 3.5 kHz, −8.5 dB at 6.5 kHz, −8 dB at 10 kHz. Tune by editing
the `"Gain"` values and restarting `pipewire`; no GUI is shipped on purpose.

#### Speaker protection

There is no ADRC on this side: only the amps' own OCP and over-temperature
protection. Keep the sink at 100 % or below and `Spkr* PA Volume` at 9/12; do not
raise `RX7/RX8 Digital Volume` above 84 (+0 dB) for loudness.


#### Known limitations

- **No microphone path**: only the speaker route is configured in UCM.
- **No headphone path**: the headset jack (`Book2 Headset Jack` input device is
  registered by the codec) has no UCM device yet.
- **No hardware volume**: the sink volume is software, so it cannot go below the
  codec's noise floor the way an analog control would.
- **Windows tuning is unrecoverable**: ACDB + WDSP are unusable on mainline.
