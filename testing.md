# Testing without LEDs wired

Every layer of this can be verified independently, most without any
hardware powered on at all.

## 1. Browser → userscript

Open DevTools (F12) → Network tab → filter for the webhook path →
navigate between drawers in Homebox. Confirm outgoing POSTs appear with
each navigation, carrying `{"type":"location"|"item","id":"..."}`, and
check their response status. The userscript no longer calls the Homebox
API itself, so there's nothing to check on that side beyond the URL match.

To decouple from Home Assistant entirely, temporarily point
`HA_WEBHOOK_URL` at a throwaway [webhook.site](https://webhook.site) URL
and watch payloads arrive live as you browse.

## 2. Webhook → Home Assistant automation logic

Bypass the browser and fire a payload directly. This exercises the full
chain including the server-side Homebox lookups, so it needs a real
location or item id from your Homebox instance:

```bash
curl -X POST https://homeassistant.url/api/webhook/homebox-highlight \
  -H "Content-Type: application/json" \
  -d '{"type":"location","id":"<a real location uuid, e.g. drawer C-03>"}'
```

Then check **Settings → Automations & Scenes → this automation → ⋮ →
Traces**. This shows every computed variable (`item_resp`, `location_resp`,
`parent_resp`, `code`, `unit`, `unit_valid`, `code_valid`, `rows1`,
`rows2`, `col_mode`, `active_col_map`, `col_valid`, `is_valid`,
`led_offset`, `seg_id`, `physical_slots`, `x_indices`, `y_index`) for
that run — the most useful single test in the whole chain, since it
validates the Homebox lookups and the coordinate math without touching
WLED.

To test just the coordinate math without hitting Homebox at all, you can
temporarily fire a payload that skips resolution — e.g. edit the
automation's `location_id` variable to a hardcoded value in a scratch
copy, or check the trace of a real run and read the `code`/`unit`
variables it computed.

Don't use the automation editor's "Run" button for this — it fakes a
trigger context with no `json` key, which throws
`UndefinedError: 'dict object' has no attribute 'json'`. That error is
expected from a manual run and isn't a bug.

If a Homebox lookup fails (bad token, wrong URL, id doesn't exist), the
`rest_command.homebox_get_entity` step is set `continue_on_error: true`,
so the automation still runs to completion and clears the strip
(`is_valid` ends up `false`) rather than aborting partway through — check
the trace's step statuses to see which lookup actually failed.

## 3. Home Assistant → WLED reachability

**Developer Tools → Actions → rest_command.wled_clear_all** with no data —
confirms basic reachability.

**rest_command.wled_set_xy** in YAML mode with known-good test values (the
A2/C-03 example — bottom unit, so code `03` resolves through
`bottom_col_map` to physical slot 4, a single LED): `seg_id: 0,
x_indices: [16], y_index: 2, color: "FF3B00"`. For a merged/wide-bin code
(e.g. a `rows1=6` top unit), `x_indices` will have two entries instead of
one — `wled_set_xy`'s payload loops over however many are given.

## 4. WLED itself — works with zero LEDs physically connected

WLED's JSON API responds normally with the firmware flashed and powered,
even with nothing wired to the data pins.

```bash
curl http://<wled-ip>/json/info      # confirm reachable, check seglc has 4 entries
curl http://<wled-ip>/json/cfg       # confirm hw.led.ins matches bus-config.json
```

WLED's own web UI also has a live software preview of segment/pixel
colors, which updates from state changes independent of real hardware —
useful for visually confirming which index lit up before any LEDs exist.

## 5. Physical LED order (requires real hardware)

The only thing that can't be verified without real LEDs: whether the
strip is actually wired in the assumed order (bottom-to-top through the
bottom unit, then the top unit, then left-to-right across the X row).
Step through one full pair and note what lights up at each index:

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

## 6. Idle timeout (scanner + power-off)

Waiting a real 5 or 30 minutes every test run isn't practical, so test
the two pieces independently instead of end-to-end:

**The effects themselves**, without waiting at all — call these directly
via **Developer Tools → Actions**:

- `rest_command.wled_start_scan` with `r: 255, g: 0, b: 0` — every
  segment should immediately switch to a bouncing-dot scan in red.
- `rest_command.wled_power_off` with no data — the whole device should
  go dark and unresponsive to `wled_set_xy` until powered back on.
- `rest_command.wled_clear_all` with no data — should both power the
  device back on and stop any running scan (`fx` reset to Solid/0).

**The timeout logic itself**: temporarily lower the two `for:` durations
in `automation.yaml`'s `idle_scan`/`idle_off` triggers (e.g. `seconds: 20`
/ `seconds: 40` instead of `minutes: 5` / `minutes: 30`), fire a real
webhook payload (per test 2), and watch **Developer Tools → States** for
`input_number.homebox_activity` — it should bump by 1 immediately, then
(after your shortened duration) a *new* automation trace should appear
triggered by `idle_scan`, and later one triggered by `idle_off`. Revert
the durations afterward.

**Confirming a real navigation actually interrupts a running scan**: while
the scanner is active (or during a shortened test wait), send another
real webhook call and check that the LEDs immediately show the new
highlight — not a scan that keeps running until some later action. If
this doesn't work on the first call and needs a second one to take
effect, that's the exact `wait_for_trigger`-on-the-same-event bug
documented in the README's Gotchas section — the fix was moving off
`wait_for_trigger` entirely in favor of the `input_number` counter +
separate `state` triggers, so this shouldn't reappear, but it's the thing
to watch for if the idle logic is ever changed again.
