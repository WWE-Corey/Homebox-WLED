# TODO

- [ ] J-K-L: install the new controller, set real IPs in the `wled_jkl_*`
      `rest_command`s, apply `jkl-bus-config.json` (confirm the GPIO for the
      board) and `jkl-create-segments.sh`, then step through LED indices on
      each segment to confirm the assumed wiring (bottom-left start, Y up
      then X left-to-right, chained J→K→L).
- [ ] J-K-L: sync `rest_commands.yaml`, `automation.yaml` and
      `display_ack_automation.yaml` into live Home Assistant and reload.
- [ ] J-K-L: reprint/relabel physical drawer labels if they show the old
      J names (old J is now L, drawers renamed to `A-01 Bushings` style).
