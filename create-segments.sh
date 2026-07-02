#!/bin/bash
# Run once after bus-config.json has been applied and the device rebooted.
# Confirms/creates the 4 segments the Home Assistant automation addresses
# by seg_id. Check `seglc` in /json/info before and after — it should show
# 4 entries afterward.

WLED_IP="192.168.xx.xx"

curl -X POST "http://${WLED_IP}/json/state" \
  -H "Content-Type: application/json" \
  -d '{"seg":[
    {"id":0,"start":0,"stop":42},
    {"id":1,"start":42,"stop":84},
    {"id":2,"start":84,"stop":126},
    {"id":3,"start":126,"stop":189}
  ]}'

echo
echo "Verify with:"
echo "curl http://${WLED_IP}/json/info | grep -o '\"seglc\":\[[^]]*\]'"
