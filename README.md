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
        │  resolves it to a unit + drawer code
        ▼
Home Assistant webhook
        │  automation looks up LED indices,
        │  clears whatever was previously lit
        ▼
rest_command → WLED JSON API
        │
        ▼
LED strip lights up the drawer
```

Homebox, Home Assistant, and WLED are three independent systems glued
together at the edges — the browser script is the only thing that knows
about all three.

## The LED addressing model

This is **not** a full 64-pixel matrix per unit. Each unit is 8 columns ×
(8 or 5) rows, and two units are addressed with a single **X strip** (8
LEDs, shared columns) and one continuous **Y strip** that runs down through
both stacked units. A drawer like `D-03` lights the 3rd X LED and the 4th Y
LED — two individually-addressed pixels, not 64.

Two units stack per physical strip (e.g. unit A1 on top of A2), giving:

- **X axis**: 8 LEDs, columns 01–08
- **Y axis**: 13 LEDs — rows A–H (top unit, 8 rows) then rows A–E (bottom
  unit, 5 rows), one continuous run
- **21 LEDs per unit-pair**, wired X first then Y so the strip wraps the
  corner without doubling back

### Why X runs right-to-left

The X strip was originally wired left→right (column 01→08), which meant
the wire had to double back across the top of the unit to reach the start
of the Y strip. Reversing X to run right→left (column 08→01) means LED 7
(column 01) lands physically adjacent to LED 8 (row A) — the strip just
wraps the corner. This is why the column-to-index formula is
`8 - column_number` rather than `column_number - 1`.

See `docs/led-order-diagram.md` for the worked-out layout.

### Unit-pair → WLED channel assignment

9 letter-pairs (A/B, C/D, ..., across 18 units) run across a 4-channel
WLED controller:

| WLED segment (bus) | Pairs (physical order) | LED count |
|---|---|---|
| 0 | A, B | 42 |
| 1 | C, D | 42 |
| 2 | E, I | 42 |
| 3 | F, G, H | 63 |

Within a pair's 21-LED block: indices 0–7 = X, 8–15 = Y (top unit A–H),
16–20 = Y (bottom unit A–E). A pair's position within its segment (first,
second, or third) adds a `pair_offset` of `0`, `21`, or `42`.

### Full coordinate formula

Given a Homebox location named e.g. `D-03` whose parent is named `A2`:

```
unit_letter = A          (A2's first character)
unit_number = 2          (A2's second character — which unit in the stack)
row_letter  = D          (D-03's first character)
col_number  = 03         (D-03's last two characters)

seg_id       = unit_map[unit_letter].seg      # which of the 4 WLED buses
pair_offset  = unit_map[unit_letter].pos * 21 # 0, 21, or 42 within that bus

x_index = pair_offset + (8 - col_number)

y_index = pair_offset + 8  + row_index   if unit_number == 1  (top unit)
        = pair_offset + 16 + row_index   if unit_number == 2  (bottom unit)
```

## Hardware

- **Controller**: GLEDOPTO GL-C-618WL (ESP32, Ethernet variant, WLED
  16.0.0)
- **4 fixed LED output channels** on GPIOs 16, 12, 4, 2 — these are
  hard-wired on this board and were *not* user-configurable; WLED
  preserved the correct board-specific pins even when an incorrect config
  attempt was pushed with placeholder GPIOs. Don't override them.
- Ethernet variant reserves GPIOs `21,19,22,25,26,27,5,23,33,0` for RMII —
  worth knowing if you ever add hardware to this board.

See `wled/bus-config.json` for the full applied configuration.

## Setup order

1. **WLED**: configure the 4 buses (`wled/bus-config.json`), reboot,
   confirm 4 segments exist (`GET /json/info`, check `seglc` has 4
   entries).
2. **Home Assistant**: add the `rest_command`s and the automation
   (`home-assistant/`).
3. **Tampermonkey**: install the userscript on each device
   (`tampermonkey/homebox-wled.user.js`), filling in your own domains and
   API token.
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
- [ ] Consider scoping the Homebox API token down from full access, since
      it currently lives in a script visible via browser dev tools.
