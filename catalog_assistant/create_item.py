"""Creates a brand NEW Homebox item (not yet cataloged) in a chosen
location, then flashes that bin to confirm. Case B of the two restocking
cases described in ../README.md's Cataloging Assistant section --
restock_item.py is Case A (something already in Homebox).

Unlike Case A, there's nothing to look up here: the human has to supply
the location up front, since Homebox has no record of this part at all.
Confirmed live against Homebox's real API before building this (see
git history / conversation record, not repeated here): POST
/api/v1/entities requires only `name`; `entityTypeId` defaults to the
Item type if omitted (confirmed via a validation-only probe), but this
resolves and passes it explicitly rather than relying on that default,
since GET /api/v1/entity-types is cheap and avoids a silent surprise if
that default ever changes; `parentId` set to a location's id correctly
nests the new item directly under it (confirmed: created a test item,
verified its `parent` in the response, deleted it, verified 404).
"""

import sys

import config
import requests

from flash_bin import flash_bin


def _auth_headers():
    return {"Authorization": config.HOMEBOX_AUTH_HEADER}


def get_item_entity_type_id(timeout=10):
    """The id of Homebox's non-location entity type ('global.item' by
    default) -- fetched live rather than hardcoded, so this keeps
    working if that id ever differs (e.g. a fresh Homebox install)."""
    resp = requests.get(
        f"{config.HOMEBOX_URL}/api/v1/entity-types",
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()
    for entity_type in resp.json():
        if not entity_type.get("isLocation"):
            return entity_type["id"]
    raise RuntimeError("No non-location entity type found in Homebox")


def search_locations(query, timeout=10):
    """Locations (not items) whose name/description contain `query` --
    the mirror image of restock_item.py's search_items(), which filters
    the other way. Same q-param/no-pagination behavior as that function;
    see its docstring."""
    resp = requests.get(
        f"{config.HOMEBOX_URL}/api/v1/entities",
        params={"q": query},
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()
    data = resp.json()
    raw = data.get("items", data) if isinstance(data, dict) else data

    locations = []
    for entity in raw:
        if not (entity.get("entityType") or {}).get("isLocation", False):
            continue
        locations.append({"id": entity["id"], "name": entity.get("name", "")})
    return locations


def create_item(name, location_id, description="", quantity=1, timeout=10):
    entity_type_id = get_item_entity_type_id(timeout=timeout)
    body = {
        "name": name,
        "description": description,
        "entityTypeId": entity_type_id,
        "parentId": location_id,
        "quantity": quantity,
    }
    resp = requests.post(
        f"{config.HOMEBOX_URL}/api/v1/entities",
        json=body,
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()
    return resp.json()


MAX_SHOWN = 20


def _prompt_pick_location(locations):
    shown = locations[:MAX_SHOWN]
    for i, loc in enumerate(shown, 1):
        print(f"  {i}. {loc['name']!r} ({loc['id']})")
    if len(locations) > MAX_SHOWN:
        print(f"  ...{len(locations) - MAX_SHOWN} more matches not shown, refine your search term")
    print("  0. None of these — abort")

    choice = input(f"Pick a location [0-{len(shown)}]: ").strip()
    if not choice.isdigit() or not (0 < int(choice) <= len(shown)):
        return None
    return shown[int(choice) - 1]


def main(query):
    locations = search_locations(query)
    if not locations:
        print(f"No location matches for {query!r}.")
        return

    location = _prompt_pick_location(locations)
    if location is None:
        print("Aborted, nothing changed.")
        return

    name = input("New item name: ").strip()
    if not name:
        print("Name is required, aborted.")
        return
    description = input("Description (optional): ").strip()
    quantity_raw = input("Quantity [1]: ").strip()
    try:
        quantity = int(quantity_raw) if quantity_raw else 1
    except ValueError:
        print("Not a number, aborted.")
        return

    confirm = input(
        f"Create {name!r} (qty {quantity}) in {location['name']!r} and flash its bin? [y/N] "
    ).strip().lower()
    if confirm != "y":
        print("Aborted, nothing changed.")
        return

    created = create_item(name, location["id"], description=description, quantity=quantity)
    flash_bin(created["id"])
    print(f"Done. Created {name!r} in {location['name']!r}. Bin flashed — go put it there.")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <location search term>", file=sys.stderr)
        sys.exit(1)
    main(sys.argv[1])
