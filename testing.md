# Testing without LEDs wired

Every layer of this can be verified independently, most without any
hardware powered on at all.

## 1. Browser → nginx tap

Confirm nginx is actually mirroring the traffic before involving Home
Assistant at all. On the nginx tap box:

```bash
sudo tail -f /var/log/nginx/access.log
```

Then browse Homebox **through the tap** (not directly to the Homebox
LXC) and navigate between drawers/items. Each item or location page load
should produce two log lines for the same request: the real proxied one
and a second one nginx generates internally for `/mirror_entity_view` —
that second line is the mirrored copy actually being sent on to Home
Assistant. Both items and locations hit the identical
`GET /api/v1/entities/{id}` shape (Homebox v0.26+ merged them into one
endpoint — no `/items/` or `/locations/` route exists anymore), so one
mirror block covers both. If you only ever see one line per navigation,
the `location ~ ^/api/v1/entities/...` regex isn't matching what
Homebox's frontend is actually requesting — check the real request path
in the browser's DevTools → Network tab against the regex in
`homebox-tap-nginx.conf`.

To decouple from Home Assistant entirely while testing just the nginx
layer, temporarily point the `proxy_pass` line in the `/mirror_entity_view`
block at a throwaway [webhook.site](https://webhook.site) URL instead
of your real Home Assistant webhook, `nginx -t && systemctl reload
nginx`, and watch payloads arrive live as you browse — then point it
back at Home Assistant once confirmed.

You can also bypass the browser and hit the tap directly, if you have a
real item/location uuid and know how Homebox's frontend authenticates
(check DevTools → Network → Headers on a real request for the exact
auth header/cookie it sends):

```bash
curl "http://<nginx-tap-ip>/api/v1/entities/<a real item or location uuid>" \
  -H "<the same auth header/cookie the frontend sends>"
```

## 2. Webhook → Home Assistant automation logic

Bypass the browser (and nginx) and fire a payload directly at Home
Assistant. This exercises the full chain including the server-side
Homebox lookups, so it needs a real location or item id from your
Homebox instance. Either shape works — `automation.yaml`'s `req_id`
extraction checks POST+json first, then falls back to GET+query (see its
comment for why both are supported). Note there's no `type` anymore —
`automation.yaml` determines item-vs-location itself from the fetched
entity's `entityType.isLocation`, so any `type` you pass is ignored:

```bash
# POST+json — same shape the old Tampermonkey userscript sent
curl -X POST https://homeassistant.url/api/webhook/homebox-highlight \
  -H "Content-Type: application/json" \
  -d '{"id":"<a real location uuid, e.g. drawer C-03>"}'

# GET+query — same shape the nginx tap sends
curl "https://homeassistant.url/api/webhook/homebox-highlight?id=<a real location uuid>"
```

Then check **Settings → Automations & Scenes → this automation → ⋮ →
Traces**. This shows every computed variable (`entity_resp`, `is_location`, `location_resp`,
`parent_resp`, `code`, `unit`, `unit_valid`, `code_valid`, `rows1`,
`rows2`, `col_mode`, `active_col_map`, `col_valid`, `is_valid`,
`y_seg_id`, `x_seg_id`, `physical_slots`, `x_indices`, `y_index`) for
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
`bottom_col_map` to physical slot 4, a single LED): `y_seg_id: 0,
x_seg_id: 9, x_indices: [3], y_index: 2, color: "FF3B00"`. For a
merged/wide-bin code (e.g. a `rows1=6` top unit), `x_indices` will have
two entries instead of one — `wled_set_xy`'s payload loops over however
many are given.

## 4. WLED itself — works with zero LEDs physically connected

WLED's JSON API responds normally with the firmware flashed and powered,
even with nothing wired to the data pins.

```bash
curl http://<wled-ip>/json/info      # confirm reachable, check seglc has 18 entries
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

## 6. Idle timeout (breathe + power-off)

Waiting a real 5 or 30 minutes every test run isn't practical, so test
the two pieces independently instead of end-to-end:

**The effects themselves**, without waiting at all — call these directly
via **Developer Tools → Actions**:

- `rest_command.wled_start_breathe` with `r: 255, g: 0, b: 0, breathe_sx: 128`
  — every segment (all 18) should immediately switch to a breathing
  pulse in red. Try low/high `breathe_sx` values to confirm it actually
  changes the speed, not just accepted-but-ignored.
- `rest_command.wled_power_off` with no data — the whole device should
  go dark and unresponsive to `wled_set_xy` until powered back on.
- `rest_command.wled_power_on` with no data, **by itself** — device comes
  back on. Then, in a *separate* call, `rest_command.wled_clear_all` —
  should stop any running breathe (`fx` reset to Solid/0). These two are
  deliberately kept as separate requests, and the real automation also
  waits ~300ms between them (see the comment on `wled_power_on` in
  `rest_commands.yaml` and the `delay` step in `automation.yaml`) — the
  LED output hardware needs a moment to settle after being re-enabled
  before it reliably renders new pixel data.

**The timeout logic itself**: set `input_number.homebox_idle_breathe_delay`
and `homebox_idle_power_off_delay` (Developer Tools → States, or the
Settings → Devices & Services → Helpers UI) to something small, like `1`
and `2`. Fire a real webhook payload (per test 2) and watch **Developer
Tools → States**:

- `input_number.homebox_activity_counter` should bump by 1 immediately, and
  `input_boolean.homebox_breathe_started_internal` / `homebox_powered_off_internal` should
  both go `off`.
- Once a minute, a new automation trace appears triggered by
  `idle_check` — it's a no-op until enough idle minutes have passed.
- After your shortened breathe delay, `homebox_breathe_started_internal` flips `on` and
  the breathe effect starts; it should **not** restart/stutter on
  subsequent `idle_check` ticks.
- After your shortened off delay, `homebox_powered_off_internal` flips `on` and
  the device powers off.

Revert both delay values back to their normal minutes afterward.

**Confirming a real navigation actually interrupts a running breathe
effect**: while breathe is active, send another real webhook call and
check that the LEDs immediately show the new highlight — not a breathe
animation that keeps running until some later action. If this doesn't
work on the first call and needs a second one to take effect, that's the
exact `wait_for_trigger`-on-the-same-event bug documented in the README's
Gotchas section — the current design (activity counter + `time_pattern`
trigger) doesn't listen for the webhook a second time anywhere, so this
shouldn't reappear, but it's the thing to watch for if the idle logic is
ever changed again.
