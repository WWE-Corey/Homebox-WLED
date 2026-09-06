"""Restocks an EXISTING Homebox item: search by name, confirm the match
with a human before touching anything, bump its quantity, then flash its
already-assigned bin so you know where to physically put the part back.

This is deliberately the "part I already cataloged, found more of it"
case, not the "brand new part" case — there's no location decision to
make here, Homebox already has one on file for the matched item. Never
writes anything without an explicit y/n confirmation first: a wrong
fuzzy-name match silently bumping the wrong item's quantity is a real
database change, not something to risk on a guess.
"""

import sys

import config
import requests


def _auth_headers():
    return {"Authorization": config.HOMEBOX_AUTH_HEADER}


def search_items(query, timeout=10):
    """Items (not locations) whose name/description contain `query`,
    substring match via Homebox's own `q` param — confirmed live that
    `search`/`name` are silently ignored and only `q` works (see
    rest_commands.yaml's homebox_search_entities comment). Homebox
    doesn't honor limit/page params on this endpoint either, so this
    always gets every match back and filters/truncates client-side.
    """
    resp = requests.get(
        f"{config.HOMEBOX_URL}/api/v1/entities",
        params={"q": query},
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()
    data = resp.json()
    raw = data.get("items", data) if isinstance(data, dict) else data

    items = []
    for entity in raw:
        # entityType.isLocation, dropped from the JSON entirely (Go's
        # omitempty) rather than sent as null when absent — same
        # .get()-based defensiveness as automation.yaml's is_location.
        is_location = (entity.get("entityType") or {}).get("isLocation", False)
        if is_location:
            continue
        location = entity.get("location") or {}
        items.append(
            {
                "id": entity["id"],
                "name": entity.get("name", ""),
                "description": entity.get("description", ""),
                "quantity": entity.get("quantity", 0),
                "location_name": location.get("name", "(no location)"),
            }
        )
    return items


def patch_quantity(entity_id, quantity, timeout=10):
    resp = requests.patch(
        f"{config.HOMEBOX_URL}/api/v1/entities/{entity_id}",
        json={"quantity": quantity},
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()


def flash_bin(entity_id, timeout=10):
    resp = requests.post(
        config.HOMEBOX_HIGHLIGHT_WEBHOOK_URL,
        json={"id": entity_id},
        timeout=timeout,
    )
    resp.raise_for_status()


MAX_SHOWN = 20


def _prompt_pick(items):
    shown = items[:MAX_SHOWN]
    for i, item in enumerate(shown, 1):
        print(
            f"  {i}. {item['name']!r} — qty {item['quantity']} "
            f"in {item['location_name']} ({item['id']})"
        )
    if len(items) > MAX_SHOWN:
        print(f"  ...{len(items) - MAX_SHOWN} more matches not shown, refine your search term")
    print("  0. None of these — abort")

    choice = input(f"Pick a match [0-{len(shown)}]: ").strip()
    if not choice.isdigit() or not (0 < int(choice) <= len(shown)):
        return None
    return shown[int(choice) - 1]


def main(query):
    items = search_items(query)
    if not items:
        print(f"No item matches for {query!r}.")
        return

    match = _prompt_pick(items)
    if match is None:
        print("Aborted, nothing changed.")
        return

    try:
        added = int(input(f"How many are you adding to {match['name']!r}? "))
    except ValueError:
        print("Not a number, aborted.")
        return

    new_quantity = match["quantity"] + added
    confirm = input(
        f"Set {match['name']!r} quantity {match['quantity']} -> {new_quantity} "
        f"and flash {match['location_name']}? [y/N] "
    ).strip().lower()
    if confirm != "y":
        print("Aborted, nothing changed.")
        return

    patch_quantity(match["id"], new_quantity)
    flash_bin(match["id"])
    print(f"Done. Quantity is now {new_quantity}. Bin flashed — go put it in {match['location_name']}.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <search term>", file=sys.stderr)
        sys.exit(1)
    main(sys.argv[1])
