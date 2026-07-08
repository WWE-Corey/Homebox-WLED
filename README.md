# Homebox → WLED Bin Locator

Physically locates parts across 18 stacked [Akro-Mills drawer bin
organizers](https://www.akromils.com/) by lighting up an LED whenever you
look up an item or location in [Homebox](https://github.com/sysadminsmedia/homebox).
Browsing to a drawer's page highlights that drawer's row and column on a
physical LED strip mounted around the units.

## Why this exists

Homebox has no concept of "viewing" a page — its API only exposes create/
update/delete events. So there's no server-side event to hook. The only
reliable signal for "what is the person looking at right now" is the
browser itself, which is why this is built around a userscript rather than
a Homebox plugin or webhook.

## Architecture

```
Homebox web UI (browser)
        │  Tampermonkey userscript watches the URL,
        │  extracts a raw {type, id} — no Homebox API calls, no token
        ▼
Home Assistant webhook
        │  automation resolves id → unit + drawer code via the Homebox
        │  API (token lives only in HA's secrets.yaml), looks up LED
        │  indices, clears whatever was previously lit
        ▼
rest_command → WLED JSON API
        │
        ▼
LED strip lights up the drawer
```

Homebox, Home Assistant, and WLED are three independent systems glued
together at the edges. The browser script only ever talks to Home
Assistant — all Homebox API access (and its credential) lives
server-side in HA, since Homebox has no scoped/read-only token option and
a browser-visible script is the last place that credential should live.

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

### Unit-pair → WLED channel assignment

9 columns (A through I, 18 units total) run across a 4-channel WLED
controller. Because each column's LED count now varies (see table above),
each column's starting offset within its segment is stored explicitly
rather than computed from a fixed block size:

| WLED segment (bus) | Columns (physical order) | Segment LED count |
|---|---|---|
| 0 | A (offset 0), B (offset 21) | 41 |
| 1 | C (offset 0), D (offset 21) | 41 |
| 2 | E (offset 0), I (offset 20) | 39 |
| 3 | F (offset 0), G (offset 19), H (offset 38) | 57 |

A column's offset is just the sum of the LED counts of every column
before it in the same segment — e.g. I's offset is 20 because E (its
segment-mate) is 8 X + 7 top + 5 bottom = 20 LEDs.

Within a column's block: indices `0` to `rows2-1` = Y (bottom unit,
bottom row first), `rows2` to `rows1+rows2-1` = Y (top unit, ending at
its topmost row), the rest = X (column 01 first, column 08 last) — where
`rows1`/`rows2` are that column's specific row counts from the table
above.

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

seg_id         = unit_map[unit_letter].seg     # which of the 4 WLED buses
led_offset     = unit_map[unit_letter].offset  # this column's starting LED, within its segment
rows1          = unit_map[unit_letter].rows1   # this column's top-unit row count (6, 7, or 8)
rows2          = unit_map[unit_letter].rows2   # this column's bottom-unit row count (always 5)
active_col_map = the normal/merged/bottom map that applies to this row_letter + unit_number
physical_slots = active_col_map[col_number]    # 1 or 2 physical X slots (1-8) this code lights

x_base    = led_offset + (rows1 + rows2 - 1)
x_indices = [x_base + slot for slot in physical_slots]   # 1 or 2 LEDs

y_index = led_offset + (rows1 + rows2 - 1) - row_index   if unit_number == 1  (top unit)
        = led_offset + rows2 - 1           - row_index   if unit_number == 2  (bottom unit)
```

Index 0 of a column's block is its bottom-left LED (bottom unit, last
row); the last index is its top-right LED (column 08). This is the
reverse of the block-relative order used before this strip direction was
flipped.

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
  something explicitly clears it. The first two attempts at the scanner
  set `fx`/`col`/`sx`/`ix` correctly (WLED's own UI even showed "Scan"
  selected) but nothing visibly animated, because the segment was still
  frozen from the highlight that came before it. `wled_start_scan` now
  explicitly sets `"frz":false` on every segment — see
  `rest_commands.yaml`.

## Idle behavior

If nothing happens for a while after a drawer is highlighted (or
cleared), the strip winds down in two stages instead of just sitting lit
indefinitely:

- **5 minutes idle** → every segment switches to WLED's built-in `fx: 10`
  ("Scan" — a single dot bouncing back and forth, Larson-scanner/Cylon/
  KITT style) in the currently selected color, across all 9 columns at
  once.
- **30 minutes idle** → the whole WLED device powers off (`"on": false`).

"Idle" means no webhook call at all — highlighting a new drawer *or*
clearing (navigating away) both count as activity and reset both clocks.

This is implemented with three separate triggers sharing one automation
(`mode: restart`), branched with `choose:` + a `condition: trigger, id:
...` check per branch:

- `id: webhook` — the normal highlight/clear path. Also increments
  `input_number.homebox_activity` (`input_helpers.yaml`) on every call.
- `id: idle_scan` — a `state` trigger on that same counter with `for:
  minutes: 5`. Starts the scan effect.
- `id: idle_off` — the same counter, `for: minutes: 30`. Powers off.

The counter's actual value is meaningless — it exists purely so the two
`state` triggers have something to watch that reliably changes on every
real webhook call, resetting their `for:` timers. A normal highlight run
always explicitly turns the device back on and resets every segment's
effect to `fx: 0` (Solid) first, in case a previous idle cycle had put it
to sleep or left the scanner running.

The highlight color (and the scanner's color) is chosen via the
`input_select.homebox_highlight_color` helper (`input_helpers.yaml`) —
add an entry to both `input_helpers.yaml`'s `options` and
`automation.yaml`'s `color_map` to offer another color.

## Hardware

- **Controller**: GLEDOPTO GL-C-618WL (ESP32, Ethernet variant, WLED
  16.0.0)
- **4 fixed LED output channels** on GPIOs 16, 12, 4, 2 — these are
  hard-wired on this board and were *not* user-configurable; WLED
  preserved the correct board-specific pins even when an incorrect config
  attempt was pushed with placeholder GPIOs. Don't override them.
- Ethernet variant reserves GPIOs `21,19,22,25,26,27,5,23,33,0` for RMII —
  worth knowing if you ever add hardware to this board.

See `bus-config.json` for the full applied configuration, and
`apply-bus-config.sh` to push it to the controller and reboot.

## Setup order

1. **WLED**: run `apply-bus-config.sh` to push the 4-bus config
   (`bus-config.json`) and reboot, then run `create-segments.sh`
   to create the 4 segments. Confirm with `GET /json/info` — `seglc`
   should show 4 entries.
2. **Home Assistant**: add the `rest_command`s (`rest_commands.yaml`), the
   `input_select`/`input_number` helpers (`input_helpers.yaml`), and the
   automation (`automation.yaml`), and add your Homebox API token to
   `secrets.yaml` as `homebox_auth_header` (see comments in
   `rest_commands.yaml`).
3. **Tampermonkey**: install the userscript on each device
   (`homebox-wled.user.js`), filling in your own domains. No
   API token needed here — it never leaves Home Assistant.
4. **Verify layer by layer** before wiring real LEDs — see
   `testing.md`.
5. **Confirm physical LED order** once LEDs are wired, by stepping through
   indices 0–20 on one segment and noting what actually lights up. Any
   mismatch with the assumed order is a one-time offset/direction fix, not
   a redesign.

## Known open items

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
