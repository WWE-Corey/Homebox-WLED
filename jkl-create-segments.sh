#!/bin/bash
# Run once on the SECOND WLED controller (bin sets J, K, L) after its LED
# config is applied. Mirror of create-segments.sh for the A-I controller.
#
# Assumes (confirm against the real wiring before relying on it):
#   - one LED bus, 40 LEDs, chained J -> K -> L
#   - each set wired like A-I: starts bottom-left, Y (rows) runs up, then
#     X (columns) runs left-to-right across the top
#   - LEDs per set: J = 6 Y + 6 X = 12, K = 8 Y + 6 X = 14, L = 8 Y + 6 X = 14
#
# Global LED ranges:
#   J: Y 0-5    X 6-11
#   K: Y 12-19  X 20-25
#   L: Y 26-33  X 34-39
#
# Segment IDs (match rest_commands.yaml's wled_jkl_* and automation.yaml's
# jkl_map): Y = 0 (J), 1 (K), 2 (L); X = 3 (J), 4 (K), 5 (L). WLED's
# "stop" is exclusive.

WLED_IP="192.168.xx.xx"

curl -X POST "http://${WLED_IP}/json/state" \
  -H "Content-Type: application/json" \
  -d '{"seg":[
    {"id":0,"start":0,"stop":6},
    {"id":1,"start":12,"stop":20},
    {"id":2,"start":26,"stop":34},
    {"id":3,"start":6,"stop":12},
    {"id":4,"start":20,"stop":26},
    {"id":5,"start":34,"stop":40}
  ]}'

# WLED does NOT keep segments created through /json/state across a
# reboot/power cycle (confirmed live 2026-11-23: after a power cycle the
# controller came back with one full-length segment, and HA showed the
# Segment 2-5 entities as unavailable). The A-I controller survives this
# by storing its layout as preset 1 and booting into it; do the same here:
# save the current segments as preset 1, then make preset 1 the boot preset.
curl -s -X POST "http://${WLED_IP}/json/state" \
  -H "Content-Type: application/json" \
  -d '{"psave":1,"n":"Solid","ib":true,"sb":true}' > /dev/null
curl -s -X POST "http://${WLED_IP}/json/cfg" \
  -H "Content-Type: application/json" \
  -d '{"def":{"ps":1,"on":true,"bri":128}}'

echo
echo "Verify with (seglc should list 6 entries, bootps 1, and survive a reboot):"
echo "curl http://${WLED_IP}/json/info | grep -o '\"seglc\":\[[^]]*\]'"
