#!/bin/bash
# Run once after bus-config.json has been applied and the device rebooted.
# Confirms/creates the 18 segments the Home Assistant automation addresses
# by seg_id. Check `seglc` in /json/info before and after — it should show
# 18 entries afterward.
#
# Grown from 3 physical-bus-sized segments to 18 — one Y-axis segment
# (breathe, IDs 0-8) and one X-axis segment (scan, IDs 9-17) PER UNIT,
# instead of one segment per physical bus. This is a purely logical
# split: bus-config.json (the 3 physical GPIO buses) is UNCHANGED, these
# 18 segments just carve up the same 178 LEDs more finely so the idle
# scan effect can run independently on X vs Y — each unit's LEDs are
# wired Y-then-X back to back, so a unit's X-strip is never physically
# contiguous with another unit's X-strip; there's no way to make "one
# scanner across A,B,C,D,E's X-axes" a single segment, only 5
# identically-configured segments that appear to move in sync. Device
# confirmed to support up to 32 segments (this WLED build reports
# maxseg=32), so 18 fits with headroom.
#
# ID scheme: Y (breathe) = 0-8 for units A-I in order. X (scan) = 9-17,
# also A-I in order — 9-13 is the A,B,C,D,E group, 14-17 is the F,G,H,I
# group (both currently run identical scan params; the split is about
# them being non-contiguous, not about wanting different colors/speeds
# — see rest_commands.yaml's wled_start_scan).
#
# Start/stop values below are computed directly from each unit's
# rows1/rows2 (automation.yaml's unit_map) and each bus's start offset
# (bus-config.json) — not guessed. See README's LED addressing model
# for the full per-unit breakdown.

WLED_IP="192.168.xx.xx"

curl -X POST "http://${WLED_IP}/json/state" \
  -H "Content-Type: application/json" \
  -d '{"seg":[
    {"id":0,"start":0,"stop":13},
    {"id":1,"start":21,"stop":33},
    {"id":2,"start":41,"stop":54},
    {"id":3,"start":62,"stop":74},
    {"id":4,"start":82,"stop":94},
    {"id":5,"start":102,"stop":113},
    {"id":6,"start":121,"stop":132},
    {"id":7,"start":140,"stop":151},
    {"id":8,"start":159,"stop":170},
    {"id":9,"start":13,"stop":21},
    {"id":10,"start":33,"stop":41},
    {"id":11,"start":54,"stop":62},
    {"id":12,"start":74,"stop":82},
    {"id":13,"start":94,"stop":102},
    {"id":14,"start":113,"stop":121},
    {"id":15,"start":132,"stop":140},
    {"id":16,"start":151,"stop":159},
    {"id":17,"start":170,"stop":178}
  ]}'

echo
echo "Verify with:"
echo "curl http://${WLED_IP}/json/info | grep -o '\"seglc\":\[[^]]*\]'"
