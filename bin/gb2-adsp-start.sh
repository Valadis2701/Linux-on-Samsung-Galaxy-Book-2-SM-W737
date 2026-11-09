#!/bin/sh
# GB2 (SM-W737): bring the ADSP remoteproc up once the real root is mounted.
#
# Why this exists: qcom_q6v5_pas lives in the initramfs and probes ~130 ms
# BEFORE / is mounted. Its firmware (qcom/sdm850/samsung/w737/qcadsp850.mbn,
# 10.3 MB) lives on the root filesystem and is deliberately NOT copied into
# the initramfs -- putting it there makes the driver boot the ADSP inside the
# initramfs, which stalls the boot before the root is mounted (black screen).
# So the early probe fails with -2, the rproc stays offline, and we simply
# start it again here, when /lib/firmware is readable. No initramfs changes.
set -u

find_adsp() {
    for d in /sys/class/remoteproc/remoteproc*; do
        [ -e "$d/name" ] || continue
        [ "$(cat "$d/name" 2>/dev/null)" = "remoteproc-adsp" ] && { echo "$d"; return 0; }
    done
    return 1
}

card_present() {
    [ -r /sys/class/sound/card0/id ] && [ "$(cat /sys/class/sound/card0/id)" = "Book2" ]
}

R=$(find_adsp) || { echo "adsp: remoteproc node not found"; exit 1; }

if [ "$(cat "$R/state")" != "running" ]; then
    echo start > "$R/state" || { echo "adsp: start failed"; exit 1; }
fi

i=0
while [ $i -lt 20 ]; do
    [ "$(cat "$R/state")" = "running" ] && break
    sleep 1; i=$((i+1))
done
[ "$(cat "$R/state")" = "running" ] || { echo "adsp: never reached running"; exit 1; }
echo "adsp: running"

# The machine driver may have probed while the ADSP was still offline; in that
# case the card never registers, so re-probe it once.
i=0
while [ $i -lt 10 ]; do
    card_present && { echo "sound: card0=Book2"; exit 0; }
    sleep 1; i=$((i+1))
done

echo "sound: card missing after adsp start, re-probing snd-soc-sdm845"
modprobe -r snd-soc-sdm845 2>/dev/null || true
modprobe snd-soc-sdm845 2>/dev/null || true

i=0
while [ $i -lt 15 ]; do
    card_present && { echo "sound: card0=Book2 after re-probe"; exit 0; }
    sleep 1; i=$((i+1))
done
echo "sound: card0 still missing"
exit 1
