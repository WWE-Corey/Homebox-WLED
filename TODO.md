# TODO

J-K-L (second WLED controller, `wled-garage2`) — done 2026-11-17: controller
on the network, `jkl-bus-config.json` applied (40 LEDs, GPIO 16), 6 segments
created, live HA `rest_commands.yaml` + `automations.yaml` synced (backups:
`*.bak-2026-11-17-pre-jkl` in HA's /config), end-to-end webhook test verified
the right LEDs for J A-01, K F-01, L H-03, L A-06.

- [ ] J-K-L: confirm the real strip wiring once the LEDs are mounted (assumed
      bottom-left start, Y up then X left-to-right, chained J→K→L on GPIO 16);
      step through indices per segment (testing.md section 5/7). Any
      difference is a segment-boundary / `jkl_map` fix.
- [ ] J-K-L: test the handheld display with an item stored in a J/K/L drawer
      (location code, quantity change, acknowledge → both walls breathe).
- [ ] J-K-L: reprint/relabel physical drawer labels if they show the old
      J names (old J is now L, drawers renamed to `A-01 Bushings` style).
