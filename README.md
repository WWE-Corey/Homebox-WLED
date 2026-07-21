# Homebox → WLED Bin Locator

Physically locates parts across 18 stacked [Akro-Mills drawer bin
organizers](https://www.akromils.com/) by lighting up an LED whenever you
look up an item or location in [Homebox](https://github.com/sysadminsmedia/homebox).
Browsing to a drawer's page highlights that drawer's row and column on a
physical LED strip mounted around the units.

A second, complementary piece — an ESP32 + SPI TFT "Location Display" —
runs on top of the same event pipeline: see [Location Display (ESP32 +
TFT)](#location-display-esp32--tft) below.

## Why this exists

Homebox has no concept of "viewing" a page — its API only exposes create/
update/delete events. So there's no server-side event to hook. The only
reliable signal for "what is the person looking at right now" is
Homebox's own frontend fetching a record to render it — a plain
`GET /api/v1/entities/{id}` (Homebox v0.26+ merged items and locations
into this one endpoint; there's no separate `/items/` or `/locations/`
route), not a purpose-built "view" event, but tappable either way.
Originally this project watched for that signal client-side, via a
Tampermonkey userscript polling the browser's URL. It's now watched
server-side instead, via a small nginx instance
(`homebox-tap-nginx.conf`) sitting in front of Homebox that mirrors that
GET pattern to Home Assistant — see [Setup order](#setup-order). The
userscript (`homebox-wled.user.js`) still works and is left in the repo,
but is no longer the recommended path: the nginx tap doesn't depend on a
specific browser/extension on every device that needs to trigger a
highlight.

## Architecture

```
Homebox web UI (browser)
        │  ordinary Homebox traffic, unmodified
        ▼
nginx tap (homebox-tap-nginx.conf, separate LXC/VM)
        │  proxies everything through to Homebox unchanged; mirrors only
        │  GET /api/v1/entities/{id} to Home Assistant as ?id=<uuid> —
        │  no Homebox API calls, no token, request/response bodies never
        │  touched. Can't tell item vs. location from the URL (Homebox
        │  doesn't distinguish them there either) — that's resolved
        │  downstream instead, see below.
        ▼
Home Assistant webhook
        │  automation fetches the entity itself (token lives only in
        │  HA's secrets.yaml), reads its entityType.isLocation to find
        │  out whether it's an item or a location, resolves id → unit +
        │  drawer code, looks up LED indices, clears whatever was
        │  previously lit
        ▼
rest_command → WLED JSON API
        │
        ▼
LED strip lights up the drawer
```

Homebox, Home Assistant, and WLED are three independent systems glued
together at the edges. The nginx tap only ever talks to Home
Assistant — all real Homebox API access (and its credential) lives
server-side in HA, since Homebox has no scoped/read-only token option and
neither the tap nor a browser-visible script is a place that credential
should live.

## The LED addressing model

This is **not** a full 64-pixel matrix per unit. Each unit is 8 columns ×
a variable number of rows, and two units are addressed with a single **X
strip** (8 LEDs, shared columns) and one continuous **Y strip** that runs
down through both stacked units. A drawer like `D-03` lights the 3rd X
LED and the 4th Y LED — two individually-addressed pixels, not 64.

Two units stack per physical strip (e.g. unit A1 on top of A2). The X
axis (8 LEDs, columns 01–08) is fixed across all 9 columns, but **the Y
split between top and bottom unit is not** — each physical column was
only wired with as many Y-LEDs as it has actual drawer rows, so the strip
length and the top/bottom boundary both vary per column:

| Column | Top rows | Bottom rows | Y LEDs | Total LEDs (8 X + Y) |
|---|---|---|---|---|
| A | 8 | 5 | 13 | 21 |
| B | 7 | 5 | 12 | 20 |
| C | 8 | 5 | 13 | 21 |
| D | 7 | 5 | 12 | 20 |
| E | 7 | 5 | 12 | 20 |
| F | 6 | 5 | 11 | 19 |
| G | 6 | 5 | 11 | 19 |
| H | 6 | 5 | 11 | 19 |
| I | 6 | 5 | 11 | 19 |

Bottom-unit row count happens to be 5 everywhere; only the top unit's row
count varies (6, 7, or 8). Each column is still wired Y first then X so
the strip wraps the corner without doubling back — it's just a
column-specific total instead of a fixed 21.

### Why each column starts at bottom-left

Each column's strip starts at LED index 0 = the **bottom-left** corner
(bottom row of the bottom unit) and ends at **top-right** (column 8, the
last LED in the block). Y is wired first, running bottom→top through the
bottom unit's rows then continuing up through the top unit's rows; X is
wired last, running left→right (column 01→08) across the top. The two
axes meet at the top-left corner — the top unit's row A (Y's last LED) is
physically adjacent to column 01 (X's first LED) — so the strip wraps
that corner without doubling back. This is why the row-to-index and
column-to-index formulas count *down* from `rows1 + rows2 - 1` rather
than counting up from 0.

### Unit → WLED segment assignment

9 columns (A through I, 18 units total) run across 3 physical WLED buses
(GPIO16/12/4 — see Hardware below), but **each column gets its own two
dedicated WLED segments**, not one shared segment per bus: a Y-axis
segment and an X-axis segment, both running the same idle Breathe
effect. That's 18 segments total from 9 columns.

Why per-column rather than per-bus: each column is wired Y-then-X back
to back (bottom-Y, top-Y, then 8 X-LEDs), and that repeats per column —
so column A's X-strip is never physically contiguous with column B's
X-strip; they're separated by column B's own Y-strip. A single WLED
segment can only run one effect across one contiguous LED range, so a
single highlight write to column A's X-axis can't be a single segment
spanning multiple columns — it's 9 independently-addressed 8-LED X
segments (one per column) and 9 independently-addressed Y segments (one
per column), all 18 of which run identical Breathe parameters when
idle, making them appear to move in sync even though each is really its
own small independent segment.

| Column | Y segment | X segment | Y range (global LEDs) | X range (global LEDs) |
|---|---|---|---|---|
| A | 0 | 9  | 0-12   | 13-20  |
| B | 1 | 10 | 21-32  | 33-40  |
| C | 2 | 11 | 41-53  | 54-61  |
| D | 3 | 12 | 62-73  | 74-81  |
| E | 4 | 13 | 82-93  | 94-101 |
| F | 5 | 14 | 102-112| 113-120|
| G | 6 | 15 | 121-131| 132-139|
| H | 7 | 16 | 140-150| 151-158|
| I | 8 | 17 | 159-169| 170-177|

All 18 segments (Y 0-8 and X 9-17) run identical Breathe parameters when
idle (see `rest_commands.yaml`'s `wled_start_breathe`), with a single
speed control shared across all of them
(`input_number.homebox_breathe_effect_speed`) — the per-column split
exists purely because the segments are physically non-contiguous, not
because any of them are meant to look different.

Because each column owns its dedicated segments, addressing within a
column no longer needs an offset into a shared range — `x_indices`/
`y_index` are simply local positions within that column's own X/Y
segment (0-7 for X, 0 to `rows1+rows2-1` for Y). `rows1`/`rows2` (each
column's own top/bottom row counts) are still not uniform across
columns — see the table earlier in this section.

### Column layout: not every bin is 1 column wide

The X axis isn't a uniform 8-narrow-columns-per-row layout either. Some
rows use physically wider bins that each span 2 of the underlying 8 X
slots, so a single Homebox code lights **two** X LEDs, not one:

- **Bottom units** (`rows2 = 5`, identical across all 9 columns): rows
  A–E all use the same layout — codes `01`–`06`, where `01` spans slots
  1+2, `02`–`05` are single slots 3–6, and `06` spans slots 7+8. Only 6
  codes exist, not 8.
- **Top units**: which rows are wide depends on that column's `rows1`
  (which uniquely identifies its layout — see the row-count table
  above):
  - `rows1 = 8` (columns A, C): every row (A–H) is normal — 8 single-slot
    codes.
  - `rows1 = 7` (columns B, D, E): rows A–D are normal (8 codes), but
    rows E–G switch to wide bins — codes `01`–`04`, each spanning 2 slots
    (1+2, 3+4, 5+6, 7+8).
  - `rows1 = 6` (columns F, G, H, I): every row (A–F) uses the same wide,
    4-code layout as above.

This is fixed physical data, not something derivable from a formula, so
it's a lookup table (`normal_col_map` / `merged_col_map` /
`bottom_col_map`, selected via `top_row_mode[rows1][row_letter]` for top
units or directly for bottom units) rather than arithmetic — see
`automation.yaml`. A code's validity is "is this code a key in whichever
map applies" rather than a numeric range check, which naturally handles
the different code counts (8, 6, or 4) per layout.

### Full coordinate formula

Given a Homebox location named e.g. `D-03` whose parent is named `A2`:

```
unit_letter = A          (A2's first character)
unit_number = 2          (A2's second character — which unit in the stack)
row_letter  = D          (D-03's first character)
col_number  = 03         (D-03's last two characters)

y_seg_id       = unit_map[unit_letter].y_seg   # this column's dedicated Y-axis segment
x_seg_id       = unit_map[unit_letter].x_seg   # this column's dedicated X-axis segment
rows1          = unit_map[unit_letter].rows1   # this column's top-unit row count (6, 7, or 8)
rows2          = unit_map[unit_letter].rows2   # this column's bottom-unit row count (always 5)
active_col_map = the normal/merged/bottom map that applies to this row_letter + unit_number
physical_slots = active_col_map[col_number]    # 1 or 2 physical X slots (1-8) this code lights

x_indices = [slot - 1 for slot in physical_slots]   # 1 or 2 LEDs, local to x_seg_id (0-7)

y_index = (rows1 + rows2 - 1) - row_index   if unit_number == 1  (top unit)
        = rows2 - 1           - row_index   if unit_number == 2  (bottom unit)
```

Both `x_indices` and `y_index` are local positions within that column's
own dedicated segment now — no offset arithmetic, since there's no
longer a bigger shared segment to be offset within (see Unit → WLED
segment assignment above). Index 0 of a column's Y segment is its
bottom-left LED (bottom unit, last row); the last index is its topmost
row. Index 0 of its X segment is column 01; the last index is column 08.

`row_index` must additionally be validated against that column's own
`rows1`/`rows2` (e.g. column B's top unit only has rows A–G valid, not
A–H) — a row letter that's in-range for column A can be out-of-range for
column B.

### Gotchas found the hard way

- **Homebox names aren't bare codes.** Real locations are named things
  like `"C-04 M8 Hex Head Cap Screws"` and units like `"A2 Metric"` — the
  code/unit is only the *leading* prefix, followed by a space and a
  description. The automation takes `name[:4]` / `parent_name[:2]` rather
  than requiring the whole name to match, so descriptive suffixes are
  ignored rather than causing a silent failure. See `code`/`unit` in
  `automation.yaml`.
- **Home Assistant silently turns some template results into numbers.**
  If a template's entire rendered output round-trips cleanly through
  `int()` (e.g. `"2"`), HA stores it as a native int instead of a string —
  but a value like `"03"` doesn't round-trip (it'd lose the leading zero)
  so it stays a string. This byte-for-byte inconsistency broke
  `unit_number in ['1', '2']` (comparing an int against string literals
  always fails) even though everything else was correct. Rather than
  fight the coercion, `unit_number` is deliberately cast with `| int(0)`
  and compared against integers (`in [1, 2]`) everywhere in the
  automation — see the comment above `unit_number` in `automation.yaml`.
- **The opposite problem hits dict keys.** HA's UI-managed automation
  storage round-trips the whole config through JSON at some point, and
  JSON object keys are always strings — so int keys written in YAML
  (`1: [...]`, `8: {...}`) silently became `"1"`, `"8"` after saving,
  breaking lookups like `(col_number | int) in bottom_col_map` even
  though the key was plainly visible in the trace. `normal_col_map` /
  `merged_col_map` / `bottom_col_map` / `top_row_mode` are now defined
  with explicit string keys, and every lookup normalizes its key with
  `| string` to match — see `automation.yaml`.
- **`wait_for_trigger` listening for the same event as the automation's
  own trigger is a trap.** The idle timeout was first built with
  `mode: restart` plus `wait_for_trigger` steps waiting on the same
  webhook the automation itself triggers on, on the theory that a new
  webhook call would always cancel the current run and restart from the
  top. In practice, a new call could instead get silently absorbed by the
  pending `wait_for_trigger` (resolving it as a normal completion rather
  than triggering a restart) — the automation just ended without
  redoing the highlight, and the *next* navigation was the one that
  actually worked. Switching the idle triggers to watch a separate
  `input_number` counter (bumped on every webhook call) instead of
  re-listening for the webhook itself avoids the ambiguity entirely —
  see the idle timeout comment block at the top of `automation.yaml`.
- **Setting individual pixels freezes the segment, silently blocking
  effects afterward.** WLED's JSON `"i"` array (used by `wled_clear_all`
  and `wled_set_xy` to light specific LEDs) implicitly sets that
  segment's `"frz"` (freeze) flag, which holds that exact static frame
  and stops the effect engine from rendering — permanently, until
  something explicitly clears it. The first two attempts at the idle
  effect set `fx`/`col`/`sx` correctly (WLED's own UI even showed the
  effect selected) but nothing visibly animated, because the segment was
  still frozen from the highlight that came before it. `wled_start_breathe`
  now explicitly sets `"frz":false` on every segment — see
  `rest_commands.yaml`.
- **The compact `"i"` range-fill shorthand doesn't reliably clear a
  pixel that was just individually set.** `wled_clear_all` originally
  blanked each segment with WLED's 3-element range-fill form —
  `"i":[0, len-1, "000000"]` — the same form the pre-18-segment design
  used successfully for years. After splitting into per-unit segments
  (see Unit → WLED segment assignment above), this stopped reliably
  clearing the *specific* pixel `wled_set_xy` had just set on that
  segment: WLED reported the segment as cleared (`fx:0`, `frz:true`)
  but the one LED stayed lit. Reproduced twice against real hardware
  via actual webhook navigations (not just synthetic API calls), and
  fixed both times by rewriting that segment's `"i"` array as explicit
  per-index pairs (`[0,"000000",1,"000000",...]`) instead of the range
  shorthand. Never reproduced on the old, larger per-bus segments, so
  it may be specific to small segments or to a pixel very recently
  touched by an individual write — not root-caused further than that,
  since it's WLED firmware (16.0.1) internal behavior, not anything on
  the HA/automation side. `wled_clear_all` now uses explicit per-index
  pairs for every one of the 18 segments — see its comment in
  `rest_commands.yaml`.
- **A trigger's `for:` duration can't be templated.** Making the idle
  delays configurable initially meant swapping the fixed `for: minutes: 5`
  / `for: minutes: 30` on the `idle_scan`/`idle_off` `state` triggers for
  `for: minutes: "{{ states('input_number...') }}"`. This looked
  reasonable and matches how `wait_for_trigger`'s `timeout:` accepts
  templates, but trigger `for:` durations don't — it's a genuinely open
  Home Assistant feature request, not a mistake in the template syntax.
  The idle timeout was rebuilt around a `time_pattern` trigger firing
  every minute with the elapsed-time comparison done manually in the
  action instead — see the Idle behavior section below.
- **Helpers created via the HA UI get slugified entity IDs, not the ones
  in `input_helpers.yaml`.** Creating a helper through Settings → Devices
  & Services → Helpers generates its entity ID from the full friendly
  `name:` text (e.g. "Homebox Activity Counter" →
  `input_number.homebox_activity_counter`), not from any short key —
  there's no YAML involved at all for a UI-created helper. Every one of
  this project's helpers ended up under UI-slugified IDs that didn't
  match what `automation.yaml` referenced, and `input_helpers.yaml`'s own
  keys had to be renamed to match reality rather than the other way
  around. If you create these via the UI instead of pasting
  `input_helpers.yaml`, check Developer Tools → States for the actual
  entity IDs before assuming they match what's referenced elsewhere.
- **Powering on needs a moment to settle before pixel data lands.** After
  a real 30-minute power-off, the first highlight following a new
  navigation would power WLED back on but the pixel data wouldn't
  render — only a second, separate command afterward would actually show
  anything, even though the automation's own trace showed correct
  `is_valid`/`y_seg_id`/`x_seg_id`/`x_indices`/`y_index` and a real call
  to `wled_set_xy` on the very first navigation. Splitting the power-on into
  its own standalone `wled_power_on` request (instead of bundling
  `"on":true` into `wled_clear_all`'s payload) wasn't enough by itself —
  a short `delay` after that call, before `wled_clear_all` runs, was also
  needed. That combination points at the LED output hardware itself
  needing a moment after being re-enabled, not just a JSON API
  request-bundling quirk — see `wled_power_on` in `rest_commands.yaml`
  and the `delay` step right after it in `automation.yaml`.

## Idle behavior

If nothing happens for a while after a drawer is highlighted (or
cleared), the strip winds down in two stages instead of just sitting lit
indefinitely:

- **Idle for `homebox_idle_breathe_delay`** (default 5) → every column's
  Y-axis and X-axis segments switch to WLED's built-in `fx: 2`
  ("Breathe"), all in the currently selected color, across all 18
  segments at once. Each column's segments are independently addressed
  (see Unit → WLED segment assignment above), but all 18 run identical
  parameters so they appear synchronized.
- **Idle for `homebox_idle_power_off_delay`** (default 30) → the whole WLED
  device powers off (`"on": false`). The same delay also turns off the
  ESP32 display's backlight, if wired for it — see [Location Display
  (ESP32 + TFT)](#location-display-esp32--tft) below.

Both delays are `input_number` helpers, adjustable live from the HA UI —
no YAML edits needed. "Idle" means no webhook call at all — highlighting
a new drawer *or* clearing (navigating away) both count as activity and
reset the clock.

This is implemented with two triggers sharing one automation
(`mode: restart`), branched with `choose:` + a `condition: trigger, id:
...` check per branch:

- `id: webhook` — the normal highlight/clear path. Also increments
  `input_number.homebox_activity_counter` and resets the `homebox_breathe_started_internal` /
  `homebox_powered_off_internal` `input_boolean` flags on every call.
- `id: idle_check` — a plain `time_pattern` trigger firing every minute.
  The action computes minutes elapsed since `homebox_activity_counter`'s
  `last_changed` timestamp and compares it against the two delay helpers
  itself, rather than relying on a triggered `for:` duration.

A trigger's `for:` duration can't be templated in Home Assistant (still
an open feature request, not just an oversight) — that's why this isn't
built as two `state` triggers with `for: minutes: "{{ ... }}"`, which is
what a first pass at "configurable delays" would look like. The
`homebox_breathe_started_internal`/`homebox_powered_off_internal` flags exist so each stage
only fires once per idle period; without them, the every-minute check
would restart the breathe effect's animation from scratch every single
minute once idle (rewriting `fx` resets it) and spam `wled_power_off`
indefinitely once past the off delay. The counter's actual value is
otherwise meaningless — it's just a reliable, always-distinct thing to
measure elapsed time from. A normal highlight run always explicitly
turns the device back on and resets every segment's effect to `fx: 0`
(Solid) first, in case a previous idle cycle had put it to sleep or left
the breathe effect running.

The highlight color (and the breathe color) is chosen via the
`input_select.homebox_highlight_color` helper (`input_helpers.yaml`) —
add an entry to both `input_helpers.yaml`'s `options` and
`automation.yaml`'s `color_map` to offer another color. The breathe
effect's speed is tunable live from the HA UI via
`input_number.homebox_breathe_effect_speed` (WLED's `sx` segment param,
0-255), shared uniformly across all 18 segments. No YAML edit needed to
adjust it.
All of these are `input_number`/`input_select` helpers, which persist
whatever value you set across Home Assistant restarts automatically
(HA's `RestoreEntity`); `initial:` in `input_helpers.yaml` only applies
the very first time a helper is created.

## Location Display (ESP32 + TFT)

### What it adds

A small SPI TFT (ESP32 + NV3007 controller) that shows whatever
location/item you're currently viewing in Homebox — its code, the item's
name, and a photo — plus three buttons: increase/decrease a staged
quantity, and acknowledge to commit that quantity back to Homebox. It's
a second consumer of the same "what is the user looking at" signal the
LED strip already reacts to, not a replacement for it.

### Hardware

**Board: ESP32-S3 with PSRAM** — a Hosyond ESP32-S3 N16R8 dev board
(genuine ESP32-S3-WROOM-1 module, 16MB flash / 8MB octal PSRAM). PSRAM
matters because a full 142×428 RGB565 framebuffer plus WiFi/HTTP/JSON
overhead is tight on the chip's internal SRAM alone.

**Display: Estardyn 2.79" TFT-SPI**, NV3007 controller, 142×428 native
glass (NOT the also-common 168×428 variant of this controller — this
project's `PANEL_NATIVE_WIDTH`/`DISPLAY_HEIGHT`/`logo_data.h` are all
set for 142×428). 8-pin header silkscreened `GND VDD SCL SDA RES DC CS
BL`: `GND`/`VDD`→`GND`/3.3V, `SCL`→GPIO 18, `SDA`→GPIO 17, `CS`→GPIO 5,
`DC`→GPIO 21, `RES`→the ESP32's `EN` pin (`TFT_RST = -1`, not
separately driven). `BL` (backlight) ships wired straight to 3.3V
(always on) — **rewire it to GPIO 8 instead** if you want the idle
backlight-off feature (see Open Items below); left on 3.3V, the
feature's `/backlight` calls are harmless no-ops.

**GPIO 33-37 are unusable on this board** — permanently wired to the
octal PSRAM itself on N16R8 chips, unavailable for SPI, buttons, or
anything else, regardless of what a generic pin-mapping example
assumes. Check any new pin assignment against Hosyond's actual pinout
diagram, not just against this reserved range, before wiring — several
other GPIOs (22-25, 26-32) are also unusable on this specific
board/chip combination; see the firmware's own pin comments for the
current, bench-confirmed assignments.

**Orientation**: the NV3007 glass is physically portrait (142 wide ×
428 tall) — no version of this chip is natively landscape. This project
wants landscape, so `homebox_display.ino` initializes LVGL at the
panel's native 142×428 and applies
`lv_display_set_rotation(display, LV_DISPLAY_ROTATION_270)`, so
everything downstream — labels, the image canvas, the HTTP API,
`push_display_image.sh`, `logo.h` — can think in landscape (428×142)
without knowing the panel itself is wired portrait.
`DISPLAY_WIDTH`/`DISPLAY_HEIGHT` in the `.ino` and
`push_display_image.sh` must be kept in sync by hand.

### Architecture

```
Homebox web UI (browser)
        │  same nginx tap, same webhook — unchanged
        ▼
Home Assistant: homebox-highlight webhook
        │  automation.yaml (unchanged highlight logic) also fires a
        │  homebox_location_viewed EVENT carrying the raw {type, id}
        ├──▶ WLED highlight
        └──▶ display_automation.yaml (separate automation,
                 triggered by that event)
                 │  does its own lightweight Homebox lookup — just
                 │  item name/quantity/image, none of the LED math
                 ├──▶ rest_command.esp32_display_set_text
                 └──▶ shell_command.push_display_image
                          (fetches the Homebox thumbnail, converts to
                          raw RGB565 via ffmpeg, POSTs bytes to the ESP32)
                 ▼
        ESP32 + NV3007 shows location + item + photo + staged quantity

ESP32 buttons
        │  +/- adjust a LOCAL staged quantity only — no network call,
        │  so a few extra taps cost nothing and are easy to undo
        │  acknowledge → POST to its own webhook (homebox-display-ack)
        ▼
Home Assistant: display_ack_automation.yaml
        ├──▶ rest_command.homebox_patch_quantity — PATCH the real
        │        quantity into Homebox, confirmed against Homebox's own
        │        backend source (see its comment in rest_commands.yaml)
        ├──▶ bumps the same activity counter/flags automation.yaml's
        │        idle_check uses — acknowledge counts as activity too
        ├──▶ rest_command.wled_clear_all + wled_start_breathe — clears the
        │        highlight and starts the breathe effect immediately, as a
        │        "job's done" visual, instead of waiting for the normal
        │        idle timeout
        └──▶ rest_command.esp32_display_clear — display returns to its
                 baked-in idle logo (logo.h), not just a blank screen
```

**Why a separate automation, and why an event instead of a second
webhook**: `display_automation.yaml` deliberately doesn't share
`automation.yaml`'s resolved variables — separate automation runs don't
share state, and the display doesn't need any of the WLED
column/row/segment math anyway, just item name/quantity/image. But two
automations can't each declare a `webhook` trigger on the same
`webhook_id` (a webhook is 1:1 with a URL), so `automation.yaml` fires a
plain HA event (`homebox_location_viewed`) instead, which any number of
automations can listen for. `display_ack_automation.yaml` uses its own
distinct webhook (`homebox-display-ack`) instead, since it's the only
automation registering that one — no collision to avoid there.

**Why quantity changes are staged, not sent per button press**: writing
to Homebox on every tap would spam its API and make "pressed + one too
many times" annoying to correct. The ESP32 tracks a running total
locally and only calls out to Home Assistant once, when acknowledge is
pressed.

**Why acknowledge also drives the WLED side**: pressing acknowledge means
the person is physically done at that drawer, which is exactly the same
"wrap up" moment the idle timeout is trying to detect after a period of
inactivity — acknowledge just means it's known immediately instead of
inferred after N minutes of silence. Reusing `wled_clear_all`/
`wled_start_breathe` for this (rather than only relying on the idle
timeout) also means the LED strip and the display return to their idle
states in the same visual step, instead of the LEDs staying stuck on the
old highlight until whatever idle delay is configured elapses.

### New files

- `homebox_display/` — a **PlatformIO** project (not an Arduino IDE
  sketch — hence `src/`/`include/` folders and `platformio.ini` instead
  of a sketch folder):
  - `platformio.ini` — board/framework/library config for the Hosyond
    ESP32-S3 N16R8 board. Two environments: `[env:esp32-s3]` (the real
    firmware) and `[env:bringup]` (`src/blink_test.cpp` only, for
    board/PSRAM/button verification independent of the full firmware).
  - `include/lv_conf.h` — generated from LVGL 9.5.0's own
    `lv_conf_template.h` with `LV_USE_NV3007` enabled and `LV_MEM_SIZE`
    bumped for headroom; everything else is template default.
  - `include/logo.h` / `include/logo_data.h` — the baked-in idle-screen
    logo shown on boot and after `/clear` (the "White Wolf Creations"
    wolf mark + wordmark, a 428×142 RGB565 array) — see
    `logo_data.h`'s header comment for the regeneration pipeline.
    `LOGO_WIDTH`/`LOGO_HEIGHT` stay tied to `DISPLAY_WIDTH`/`HEIGHT`.
  - `src/homebox_display.ino` — the firmware itself; see the Open Items
    above for the hardware-specific gotchas found getting it working.
  - `src/blink_test.cpp` — minimal board bring-up test (PlatformIO/
    board config/PSRAM/button wiring) independent of the full firmware.
    `pio run -e bringup -t upload`.
- `esp32_display_set_text`, `esp32_display_clear`, and
  `homebox_patch_quantity` live in `rest_commands.yaml` alongside the
  WLED commands (not a separate file); `push_display_image` similarly
  lives in `shell_commands.yaml`. `rest_command`/`shell_command` are
  dict-type HA config domains — most setups only point each one at a
  single `!include` file, so a second file for the same domain would
  silently never load.
- `push_display_image.sh` — fetches a Homebox attachment, converts it to
  raw RGB565 via `ffmpeg`, and POSTs the bytes to the ESP32. See its own
  header comment and the Open Items above for the HA-container gotchas.
- `display_automation.yaml` — the `homebox_location_viewed` event handler
  described above.
- `display_ack_automation.yaml` — the acknowledge-button webhook handler.

### Open items

- [ ] Confirm the acknowledge flow's immediate WLED clear+breathe (see
      `display_ack_automation.yaml`) doesn't fight with `automation.yaml`'s
      own idle-triggered breathe in some edge case — they share the same
      `homebox_breathe_started_internal` flag on purpose, but this hasn't
      been exercised against a real acknowledge button press yet.
- [ ] Confirm where `shell_command` actually executes on your specific
      Home Assistant install, and whether `ffmpeg`/`curl` are available
      there or need a different approach (add-on, external helper
      service, etc.).
- [ ] `HOMEBOX_TOKEN_FILE` in `push_display_image.sh` is a second copy of
      the Homebox token, separate from `secrets.yaml`, because
      `shell_command` can't read HA's `!secret`-loaded Jinja values —
      worth keeping in mind if the token is ever rotated (two places to
      update, not one).
- [ ] **Display backlight idle-off, needs hardware rewiring to actually
      do anything**: firmware/automation side is done — `POST /backlight
      {"on":bool}` (see `TFT_BL`/`handleSetBacklight()` in
      `homebox_display.ino`) is wired into the same `off_delay`/power-on
      moments as WLED's own idle power-off (`automation.yaml`), reusing
      `input_number.homebox_idle_power_off_delay` rather than adding a
      separate timeout. Does nothing on the actual panel until `BL` is
      physically moved from 3.3V (its default wiring) to GPIO 8 — not
      yet done/confirmed against real hardware.

Resolved, kept as reference for anyone touching this hardware again:

- **Board/panel**: Hosyond ESP32-S3 N16R8 (genuine WROOM-1, octal
  PSRAM) driving an Estardyn 2.79" TFT-SPI, NV3007 controller, 142×428
  native glass, 8-pin header `GND VDD SCL SDA RES DC CS BL`. See
  Hardware below for pin assignments and their constraints.
- **`qio_opi` boot loop**: the stock PlatformIO board profile
  (`esp32-s3-devkitc-1`, an 8MB-flash/no-PSRAM N8 board) needs several
  overrides for this actual N16R8 board (16MB flash, octal PSRAM) — see
  `platformio.ini`'s own comments for the full list. `qio_opi` (QIO
  flash + octal PSRAM) is the usually-cited-correct memory type for
  N16R8 boards and looked right on paper, but caused a watchdog-reset
  boot loop before any bootloader output at all on this board — `dio_opi`
  is what's actually running (the ROM boot log always reports "mode:DIO"
  regardless of this setting), and is what's configured.
  Separately, `pinMode()` on GPIO 26-32 (wired to the flash SPI bus on
  every ESP32-S3-WROOM-1 module) alone caused the same kind of
  watchdog-reset boot loop — the actual cause of an extended debugging
  session that initially suspected the PSRAM/flash-size config instead.
- **Rotation**: `LV_DISPLAY_ROTATION_270` is correct for the case design
  in use (connector edge on the left, logo upright). Don't get a
  different orientation by overriding individual MADCTL mirror bits via
  `lv_lcd_generic_mipi_set_address_mode()` instead of switching the
  whole rotation value — causes visible tearing on this panel.
- **`WebServer::arg("plain")` truncates binary bodies** at the first
  `0x00` byte (it null-terminates internally) — any image containing
  pure black would silently truncate. `/image` uses a `RequestHandler`
  subclass with WebServer's raw-body streaming API instead.
- **Homebox eager-loading gaps**: a single-entity `GET` doesn't
  eager-load `attachments[].thumbnail` or a nested `location`'s own
  parent. Use the item's own `imageId` field (server-computed primary
  photo id) instead of digging through `attachments[]`, and fetch a
  location by its own id (whose `parent` Homebox *does* eager-load)
  rather than relying on an item response's nested `location` summary —
  see `location_detail_resp` in `display_automation.yaml`.
- **`push_display_image.sh`** has full step-by-step error checking
  (HTTP status codes, byte counts, `ffmpeg`'s exit code) and logs to
  `/config/push_display_image.log`, because HA's `shell_command` logger
  only ever surfaces a bare return code. On Supervised/HAOS, don't
  assume an SSH session is the same container `shell_command` executes
  in (it often isn't) — `/config` is a shared volume, making a log file
  there more trustworthy for debugging. The real HA Core container
  ships **BusyBox** `mktemp`, not GNU coreutils — don't use GNU-only
  flags like `--suffix=`; use the portable `mktemp path/prefix.XXXXXX`
  template form instead.
- **`lv_nv3007_create()`/`lv_lcd_generic_mipi_create()` don't call
  `lv_display_set_buffers()`** — register `flush_cb` but allocate no
  render buffer on their own. `setupDisplay()` calls it explicitly
  (single PSRAM buffer, `LV_DISPLAY_RENDER_MODE_FULL`).
- **Panel gap/border**: `PANEL_GAP_X=0`, `PANEL_GAP_Y=14` compensate for
  the visible glass not starting at GRAM address 0 on either axis — two
  independent hardware defect zones near opposite ends of the panel's
  addressable row range, not a single offset (splitting the difference
  between two swept extremes is what cleared both). A `BORDER_THICKNESS`
  2px white border along `IMAGE_AREA`/`TEXT_AREA`'s true panel edges
  additionally masks residual artifact against the black item/location
  background (never visible against the white idle logo). These values
  are per-unit — if working from a different physical panel, re-sweep
  both `PANEL_GAP_Y` and the border against real content, checking all
  four edges, not just whether a stripe reproduces at all.
- **Item photos, direct-SPI rendering**: photos are written straight to
  the panel's GRAM (`sendImageAreaDirect()` in `homebox_display.ino`),
  bypassing LVGL's canvas/image compositing entirely — LVGL's own
  compositing had a confirmed bug under this rotation setup that
  corrupted photo content (text/logo, which use a different LVGL draw
  path, were unaffected). Two things this bypass had to replicate by
  hand, found empirically rather than derived from LVGL's own rotation
  math (which turned out not to match this panel's actual addressing
  convention): the logical→native pixel mapping is direct, no axis
  transpose; and pixel bytes need to be swapped (high/low byte) before
  sending, which LVGL's normal render path apparently already does
  internally. Also: `IMAGE_AREA_WIDTH` is 142, not a wider value — the
  panel's column-address (`CASET`) axis has a real ~142-position limit;
  addressing wider produces a repeating/wrapped raster artifact instead
  of a clean image.
- **`/image` POST took ~5 seconds, unrelated to SPI clock speed or the
  Homebox/ffmpeg pipeline** (both together take <250ms — see
  `push_display_image.sh`'s own `TIMING:` log lines). Root-caused with
  firmware-side per-chunk timing: WebServer's raw-body loop
  (`Parsing.cpp::_parseRequest`) always asks `Stream::readBytes()` to
  fill a full `HTTP_RAW_BUFLEN` (1436 bytes) chunk, even on the LAST
  chunk of a 40328-byte image where only 120 bytes actually remain —
  `readBytes()` then blocks retrying, byte by byte, for the remaining
  1316 bytes that will never arrive, until its internal timeout finally
  gives up. Bench-confirmed exactly one ~5000ms stall on the final
  chunk of every single upload, all other chunks completing in
  single-digit ms. A `#define` in `homebox_display.ino` does NOT fix
  this — `Parsing.cpp` is a separately-compiled library source that
  never sees a sketch-local macro, regardless of `#include` order.
  Fixed via `platformio.ini`'s `build_flags` (`-D HTTP_RAW_BUFLEN=40328
  -D HTTP_UPLOAD_BUFLEN=40328`, both required together — the second
  sizes the actual fixed-size receive buffer, and must be at least as
  large as the first or `readBytes()` writes past its end), setting
  both to the display's exact `/image` payload size so the whole image
  always arrives in exactly one read that can never ask for more bytes
  than the client will send. A `static_assert` in `homebox_display.ino`
  (right after `IMAGE_AREA_WIDTH`/`DISPLAY_HEIGHT` are defined) fails
  the build loudly if this value and the real payload size ever drift
  apart again. Net effect: ~5.2s → ~0.1-0.6s per photo.
- **Display backlight, idle-off**: `POST /backlight {"on":bool}` (see
  `TFT_BL`/`handleSetBacklight()`) turns the panel backlight on/off via
  GPIO 8, wired into the same idle-timeout moments as WLED's own power
  off/on (`automation.yaml`, reusing `input_number.homebox_idle_power_
  off_delay` rather than a separate timeout) — saves backlight
  lifespan while nobody's looking anyway. Requires `BL` physically
  rewired from 3.3V (its default wiring) to GPIO 8; a harmless no-op
  otherwise.
- **Display auto-clear timeout**: separately from the backlight,
  `display_automation.yaml`'s `display_idle_check` trigger clears the
  display back to the idle logo if an item/location has sat unacknowledged
  for longer than `input_number.homebox_display_timeout_minutes` (0 =
  disabled, the default) — reuses `input_number.homebox_activity_counter`
  as the idle signal (bumped by both real navigations and acknowledges)
  rather than adding a redundant counter.

## Hardware

- **Controller**: GLEDOPTO GL-C-618WL (ESP32, Ethernet variant, WLED
  16.0.0)
- **4 fixed LED output channels available** on GPIOs 16, 12, 4, 2 — these
  are hard-wired on this board and were *not* user-configurable; WLED
  preserved the correct board-specific pins even when an incorrect config
  attempt was pushed with placeholder GPIOs. Don't override them.
- **Only 3 of the 4 are actually used** — GPIO2 was dropped and its
  columns (F, G, H) folded into the other three channels: seg0 (GPIO16)
  = A,B,C; seg1 (GPIO12) = D,E; seg2 (GPIO4) = F,G,H,I. Total LED count
  is unchanged (178) — this was a pure regroup onto fewer physical
  outputs, not a change to the LEDs themselves. See `bus-config.json`/
  `create-segments.sh` and the LED addressing model above.
- Ethernet variant reserves GPIOs `21,19,22,25,26,27,5,23,33,0` for RMII —
  worth knowing if you ever add hardware to this board.

See `bus-config.json` for the full applied configuration, and
`apply-bus-config.sh` to push it to the controller and reboot.

## Setup order

1. **WLED**: run `apply-bus-config.sh` to push the 3-bus config
   (`bus-config.json`) and reboot, then run `create-segments.sh`
   to create the 3 segments. Confirm with `GET /json/info` — `seglc`
   should show 3 entries.
2. **Home Assistant**: add the `rest_command`s (`rest_commands.yaml`) and
   `shell_command` (`shell_commands.yaml` — only needed once the
   Location Display is in play), the `input_select`/`input_number`
   helpers (`input_helpers.yaml`), and the automation (`automation.yaml`),
   and add your Homebox API token to `secrets.yaml` as
   `homebox_auth_header` (see comments in `rest_commands.yaml`). If your
   `configuration.yaml` uses `rest_command: !include rest_commands.yaml`
   (rather than pasting the block directly), read `rest_commands.yaml`'s
   header comment first — there's a real, easy-to-hit double-nesting
   mistake to avoid there.
3. **nginx tap**: stand up a small separate LXC/VM with nginx, apply
   `homebox-tap-nginx.conf` (filling in your real Homebox backend and
   Home Assistant addresses), and point whatever you use to reach
   Homebox at this box instead of the Homebox LXC directly. No API
   token needed here — it never leaves Home Assistant, and the tap never
   touches request/response bodies, only the URL. (The older
   `homebox-wled.user.js` Tampermonkey userscript still works as an
   alternative if you'd rather not stand up a second LXC, but requires
   installing it on every device that should trigger a highlight.)
4. **Verify layer by layer** before wiring real LEDs — see
   `testing.md`.
5. **Confirm physical LED order** once LEDs are wired, by stepping through
   indices 0–20 on one segment and noting what actually lights up. Any
   mismatch with the assumed order is a one-time offset/direction fix, not
   a redesign.

## Known open items

These are all about the LED/WLED side specifically — see the [Location
Display](#location-display-esp32--tft) section's own Open Items for
that side's list.

- [ ] Confirm physical LED order matches the assumed wiring (Y
      bottom-to-top through the bottom unit then the top unit, then X
      left-to-right across the top) once LEDs are physically installed —
      see the coordinate formula assumptions above.
- [ ] Confirm the column-layout lookup tables (`normal_col_map` /
      `merged_col_map` / `bottom_col_map` / `top_row_mode`) match reality
      for every column — they're built from two reference images (one per
      bottom-unit layout, one per each of the 3 top-unit layouts) rather
      than exhaustively checked against all 9 columns' actual Homebox
      codes. A location using a code the map doesn't expect (e.g. a `07`
      on a wide-bin row that should only go up to `04`) will silently
      fail validation rather than highlight the wrong LED.
- [x] ~~Confirm all 64+ Homebox drawer locations follow the strict
      `LETTER-NN` naming convention~~ — they don't; real names are
      descriptive (`"C-04 M8 Hex Head Cap Screws"`, `"A2 Metric"`). Fixed
      by taking just the leading prefix instead of requiring an exact
      match (see the Gotchas section above). Still assumes the code is
      followed by a space or nothing — a name like `"C-041 ..."` with no
      separator would misparse.
- [x] ~~Consider scoping the Homebox API token down from full access~~ —
      Homebox has no scoped/read-only token support today, so instead the
      token was moved out of the browser script entirely and into HA's
      `secrets.yaml`; HA now does all Homebox API resolution server-side
      (see `automation.yaml`). It's still a full-access token, but it's
      no longer visible via browser dev tools.
