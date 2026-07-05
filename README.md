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
count varies (6, 7, or 8). Each column is still wired X first then Y so
the strip wraps the corner without doubling back — it's just a
column-specific total instead of a fixed 21.

### Why X runs right-to-left

The X strip was originally wired left→right (column 01→08), which meant
the wire had to double back across the top of the unit to reach the start
of the Y strip. Reversing X to run right→left (column 08→01) means LED 7
(column 01) lands physically adjacent to LED 8 (row A) — the strip just
wraps the corner. This is why the column-to-index formula is
`8 - column_number` rather than `column_number - 1`.

See `docs/led-order-diagram.md` for the worked-out layout.

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

Within a column's block: indices `0–7` = X, `8` to `8+rows1-1` = Y (top
unit), the rest = Y (bottom unit) — where `rows1` is that column's
specific top-row count from the table above.

### Full coordinate formula

Given a Homebox location named e.g. `D-03` whose parent is named `A2`:

```
unit_letter = A          (A2's first character)
unit_number = 2          (A2's second character — which unit in the stack)
row_letter  = D          (D-03's first character)
col_number  = 03         (D-03's last two characters)

seg_id      = unit_map[unit_letter].seg     # which of the 4 WLED buses
led_offset  = unit_map[unit_letter].offset  # this column's starting LED, within its segment
rows1       = unit_map[unit_letter].rows1   # this column's top-unit row count (6, 7, or 8)
rows2       = unit_map[unit_letter].rows2   # this column's bottom-unit row count (always 5)

x_index = led_offset + (8 - col_number)

y_index = led_offset + 8         + row_index   if unit_number == 1  (top unit)
        = led_offset + 8 + rows1 + row_index   if unit_number == 2  (bottom unit)
```

`row_index` must additionally be validated against that column's own
`rows1`/`rows2` (e.g. column B's top unit only has rows A–G valid, not
A–H) — a row letter that's in-range for column A can be out-of-range for
column B.

## Hardware

- **Controller**: GLEDOPTO GL-C-618WL (ESP32, Ethernet variant, WLED
  16.0.0)
- **4 fixed LED output channels** on GPIOs 16, 12, 4, 2 — these are
  hard-wired on this board and were *not* user-configurable; WLED
  preserved the correct board-specific pins even when an incorrect config
  attempt was pushed with placeholder GPIOs. Don't override them.
- Ethernet variant reserves GPIOs `21,19,22,25,26,27,5,23,33,0` for RMII —
  worth knowing if you ever add hardware to this board.

See `wled/bus-config.json` for the full applied configuration, and
`wled/apply-bus-config.sh` to push it to the controller and reboot.

## Setup order

1. **WLED**: run `wled/apply-bus-config.sh` to push the 4-bus config
   (`wled/bus-config.json`) and reboot, then run `wled/create-segments.sh`
   to create the 4 segments. Confirm with `GET /json/info` — `seglc`
   should show 4 entries.
2. **Home Assistant**: add the `rest_command`s and the automation
   (`home-assistant/`), and add your Homebox API token to `secrets.yaml`
   as `homebox_auth_header` (see comments in `rest_commands.yaml`).
3. **Tampermonkey**: install the userscript on each device
   (`tampermonkey/homebox-wled.user.js`), filling in your own domains. No
   API token needed here — it never leaves Home Assistant.
4. **Verify layer by layer** before wiring real LEDs — see
   `docs/testing.md`.
5. **Confirm physical LED order** once LEDs are wired, by stepping through
   indices 0–20 on one segment and noting what actually lights up. Any
   mismatch with the assumed order is a one-time offset/direction fix, not
   a redesign.

## Known open items

- [ ] Confirm physical LED order matches the assumed wiring (X
      right-to-left, then Y top-unit-first) once LEDs are physically
      installed — see the coordinate formula assumptions above.
- [ ] Confirm all 64+ Homebox drawer locations follow the strict
      `LETTER-NN` naming convention; anything that doesn't will silently
      fail to highlight.
- [x] ~~Consider scoping the Homebox API token down from full access~~ —
      Homebox has no scoped/read-only token support today, so instead the
      token was moved out of the browser script entirely and into HA's
      `secrets.yaml`; HA now does all Homebox API resolution server-side
      (see `home-assistant/automation.yaml`). It's still a full-access
      token, but it's no longer visible via browser dev tools.
