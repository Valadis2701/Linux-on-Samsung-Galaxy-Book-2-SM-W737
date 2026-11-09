#!/usr/bin/env python3
"""Build the native-path DTS. Usage: make_dt_native.py [2160|1080]

Set W737_WORK to the directory that holds w737.dts and the dt/ output folder.
"""
import os
import re
import sys

BASE = os.environ.get("W737_WORK", r"C:\Users\Valadis\Desktop\Decompile\WORK")
SRC = os.path.join(BASE, "w737.dts")
OUTDIR = os.path.join(BASE, "dt")

MODES = {
    "2160": dict(clock=206570000, h=2160, v=1440, hfp=48, hbp=80, hsw=32,
                 vfp=3, vbp=35, vsw=6, name="native"),
    "1080": dict(clock=148500000, h=1920, v=1080, hfp=88, hbp=148, hsw=44,
                 vfp=4, vbp=36, vsw=5, name="native-1080"),
}
mode_key = sys.argv[1] if len(sys.argv) > 1 else "2160"
m = MODES[mode_key]
OUT = os.path.join(OUTDIR, m["name"] + ".dts")


def node_span(s, name, start=0):
    mm = re.search(r"\n(\t*)" + re.escape(name) + r"\s*\{", s[start:])
    if not mm:
        return None
    i = start + mm.start() + 1
    j = s.index("{", i)
    depth = 0
    for k in range(j, len(s)):
        if s[k] == "{":
            depth += 1
        elif s[k] == "}":
            depth -= 1
            if depth == 0:
                return i, k
    return None


s = open(SRC, encoding="utf-8").read()

# 1. dsi0: drop the dual-DSI properties
a, b = node_span(s, "dsi@ae94000")
blk = s[a:b]
for prop in ("qcom,dual-dsi-mode", "qcom,master-dsi", "qcom,sync-dual-dsi"):
    blk = re.sub(r"\n\t+" + re.escape(prop) + ";", "", blk)
s = s[:a] + blk + s[b:]

# 1b. dsi1: disable the second controller
a, b = node_span(s, "dsi@ae96000")
blk = s[a:b]
blk = re.sub(r"\n\t+qcom,dual-dsi-mode;", "", blk)
blk = re.sub(r"\n\t+qcom,sync-dual-dsi;", "", blk)
blk = blk.replace('status = "okay";', 'status = "disabled";', 1)
s = s[:a] + blk + s[b:]

# 2. dsi0 port@1 -> bridge
a, b = node_span(s, "dsi@ae94000")
blk = s[a:b]
p1 = re.search(r"port@1\s*\{\s*reg = <0x01>;\s*endpoint\s*\{", blk)
if not p1:
    raise SystemExit("dsi0 port@1 endpoint not found")
rel = a + p1.start() + p1.group(0).index("endpoint")
seg_end = s.index("};", rel)
seg = s[rel:seg_end].replace("endpoint {", "dsi0_out: endpoint {", 1)
seg = re.sub(r"remote-endpoint = <0x[0-9a-f]+>;", "remote-endpoint = <&sn65_in>;", seg, count=1)
s = s[:rel] + seg + s[seg_end:]

# 3. disable the old panel node
a, b = node_span(s, "dsi@ae94000")
blk = s[a:b]
pl = re.search(r"\n(\t*)panel@0\s*\{", blk)
if pl:
    indent = pl.group(1)
    pos = a + blk.index('compatible = "samsung,w737-panel";')
    end_line = s.index("\n", pos)
    s = s[:end_line + 1] + f'{indent}\tstatus = "disabled";\n' + s[end_line + 1:]

# 3.5 move the bridge from i2c@a88000 to i2c@88c000 (per ACPI SAM0101)
a, b = node_span(s, "i2c@a88000")
blk = s[a:b]
mb = re.search(r"\n(\t*)sn65dsi86@2c\s*\{", blk)
if not mb:
    raise SystemExit("sn65dsi86@2c not found on i2c@a88000")
start = a + mb.start()
ob = s.index("{", start)
depth = 0
for k in range(ob, len(s)):
    if s[k] == "{":
        depth += 1
    elif s[k] == "}":
        depth -= 1
        if depth == 0:
            end = k
            break
node_txt = s[start:end + 2]          # including "};" and the newline
s = s[:start] + s[end + 2:]

# enable i2c@88c000 and move the bridge there
a2, b2 = node_span(s, "i2c@88c000")
blk2 = s[a2:b2]
if 'status = "disabled"' in blk2:
    blk2 = blk2.replace('status = "disabled"', 'status = "okay";\n\t\t\t\tclock-frequency = <0x61a80>', 1)
s = s[:a2] + blk2 + s[b2:]
a2, b2 = node_span(s, "i2c@88c000")
s = s[:b2] + node_txt + s[b2:]

# 3.6 bridge interrupt: TLMM pin 29 (0x1d), rising edge
a, b = node_span(s, "sn65dsi86@2c")
blk = s[a:b]
if "interrupts-extended" not in blk:
    blk = blk.replace('status = "okay";',
                      'status = "okay";\n\t\t\t\t\tinterrupts-extended = <0x67 0x1d 0x01>;', 1)
s = s[:a] + blk + s[b:]

# 4. bridge ports
a, b = node_span(s, "sn65dsi86@2c")
ports = """
\t\t\t\tports {
\t\t\t\t\t#address-cells = <0x01>;
\t\t\t\t\t#size-cells = <0x00>;

\t\t\t\t\tport@0 {
\t\t\t\t\t\treg = <0x00>;

\t\t\t\t\t\tsn65_in: endpoint {
\t\t\t\t\t\t\tremote-endpoint = <&dsi0_out>;
\t\t\t\t\t\t};
\t\t\t\t\t};

\t\t\t\t\tport@1 {
\t\t\t\t\t\treg = <0x01>;

\t\t\t\t\t\tsn65_out: endpoint {
\t\t\t\t\t\t\tremote-endpoint = <&panel_in>;
\t\t\t\t\t\t};
\t\t\t\t\t};
\t\t\t\t};
"""
s = s[:b] + ports + s[b:]

# 5. panel node with the selected timings
panel = f"""
\tpanel {{
\t\tcompatible = "panel-dpi";
\t\twidth-mm = <250>;
\t\theight-mm = <170>;
\t\tdata-mapping = "jeida-24";

\t\tpanel-timing {{
\t\t\tclock-frequency = <{m['clock']}>;
\t\t\thactive = <{m['h']}>;
\t\t\tvactive = <{m['v']}>;
\t\t\thfront-porch = <{m['hfp']}>;
\t\t\thback-porch = <{m['hbp']}>;
\t\t\thsync-len = <{m['hsw']}>;
\t\t\tvfront-porch = <{m['vfp']}>;
\t\t\tvback-porch = <{m['vbp']}>;
\t\t\tvsync-len = <{m['vsw']}>;
\t\t}};

\t\tport {{
\t\t\tpanel_in: endpoint {{
\t\t\t\tremote-endpoint = <&sn65_out>;
\t\t\t}};
\t\t}};
\t}};
"""
root_end = s.rstrip().rfind("};")
s = s[:root_end] + panel + s[root_end:]

open(OUT, "w", encoding="utf-8", newline="\n").write(s)
print(f"mode {mode_key}: {m['h']}x{m['v']} @ {m['clock']/1e6:.2f} MHz -> {OUT} ({os.path.getsize(OUT)} bytes)")
print(f"  per-lane data rate at 4 lanes: {m['clock']*24/4/1e6:.0f} Mbit/s")
