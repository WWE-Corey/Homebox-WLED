#!/bin/bash
# Applies bus-config.json to the WLED controller and reboots it.
# Run this once during initial setup, before create-segments.sh.
# Confirmed working on GLEDOPTO GL-C-618WL (Ethernet variant) — pins are
# board-specific fixed channels; do not change bus-config.json's pins
# without confirming against your own board's documentation.

WLED_IP="192.168.xx.xx"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

curl -X POST "http://${WLED_IP}/json/cfg" \
  -H "Content-Type: application/json" \
  --data @"${SCRIPT_DIR}/bus-config.json"

echo
echo "Rebooting WLED to apply..."
curl -X POST "http://${WLED_IP}/json/state" \
  -H "Content-Type: application/json" \
  -d '{"rb":true}'

echo
echo "After reboot, verify with:"
echo "curl -s http://${WLED_IP}/json/cfg | jq '.hw.led.ins'"
echo "(WLED reuses the key \"ins\" for wifi/button/IR config too, so a plain"
echo "grep for \"ins\" will match those as well — jq's path query goes"
echo "straight to the LED bus array.)"
echo "Then run create-segments.sh."
