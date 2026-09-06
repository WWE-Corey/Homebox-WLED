"""Flashes a Homebox item's (or location's) assigned bin by calling the
existing homebox-highlight webhook (see ../automation.yaml) directly —
the same request ../testing.md documents as a manual curl bypass of the
browser/nginx tap, wrapped here for reuse by other tools.

Intended caller: a future cataloging tool that just wrote a newly
identified part into Homebox and wants to visually confirm which bin it
landed in. HA resolves item -> location itself (entityType.isLocation,
see automation.yaml), so either kind of real Homebox UUID works here —
this module does no Homebox lookups of its own.
"""

import sys

import config
import requests


def flash_bin(entity_id, timeout=10):
    """POST a real Homebox item or location UUID to the homebox-highlight
    webhook, triggering the same resolve -> coordinate -> LED-write chain
    a browser page view would."""
    resp = requests.post(
        config.HOMEBOX_HIGHLIGHT_WEBHOOK_URL,
        json={"id": entity_id},
        timeout=timeout,
    )
    resp.raise_for_status()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <homebox-item-or-location-uuid>", file=sys.stderr)
        sys.exit(1)
    flash_bin(sys.argv[1])
