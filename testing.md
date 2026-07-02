# Testing without LEDs wired

Every layer of this can be verified independently, most without any
hardware powered on at all.

## 1. Browser → userscript

Open DevTools (F12) → Network tab → filter for the webhook path →
navigate between drawers in Homebox. Confirm outgoing POSTs appear with
each navigation, and check their response status.

To decouple from Home Assistant entirely, temporarily point
`HA_WEBHOOK_URL` at a throwaway [webhook.site](https://webhook.site) URL
and watch payloads arrive live as you browse.

## 2. Webhook → Home Assistant automation logic

Bypass the browser and fire a payload directly:

```bash
curl -X POST https://ha.wwolf.us/api/webhook/homebox-highlight \
  -H "Content-Type: application/json" \
  -d '{"unit":"A2","code":"C-03"}'
```

Then check **Settings → Automations & Scenes → this automation → ⋮ →
Traces**. This shows every computed variable (`unit_valid`, `code_valid`,
`is_valid`, `pair_offset`, `seg_id`, `x_index`, `y_index`) for that run —
the most useful single test in the whole chain, since it validates all
the coordinate math without touching WLED.

Don't use the automation editor's "Run" button for this — it fakes a
trigger context with no `json` key, which throws
`UndefinedError: 'dict object' has no attribute 'json'`. That error is
expected from a manual run and isn't a bug.

## 3. Home Assistant → WLED reachability

**Developer Tools → Actions → rest_command.wled_clear_all** with no data —
confirms basic reachability.

**rest_command.wled_set_xy** in YAML mode with known-good test values (the
A2/C-03 example from the README): `seg_id: 0, x_index: 5, y_index: 18,
color: "FF3B00"`.

## 4. WLED itself — works with zero LEDs physically connected

WLED's JSON API responds normally with the firmware flashed and powered,
even with nothing wired to the data pins.

```bash
curl http://<wled-ip>/json/info      # confirm reachable, check seglc has 4 entries
curl http://<wled-ip>/json/cfg       # confirm hw.led.ins matches wled/bus-config.json
```

WLED's own web UI also has a live software preview of segment/pixel
colors, which updates from state changes independent of real hardware —
useful for visually confirming which index lit up before any LEDs exist.

## 5. Physical LED order (requires real hardware)

The only thing that can't be verified without real LEDs: whether the
strip is actually wired in the assumed order (X right-to-left, then Y
top-unit-first). Step through one full pair and note what lights up at
each index:

```bash
for i in $(seq 0 20); do
  curl -s -X POST http://<wled-ip>/json/state \
    -H "Content-Type: application/json" \
    -d "{\"seg\":[{\"id\":0,\"i\":[$i,\"FF3B00\"]}]}" > /dev/null
  read -p "LED $i is lit at: "
done
```

Any mismatch with the assumed order (see README's coordinate formula) is
a one-time offset/direction correction, not a redesign.
