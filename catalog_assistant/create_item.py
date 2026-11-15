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

Model number, manufacturer, and purchase price all need a follow-up
PUT after creation, not fields on the create call itself -- see
set_full_details()'s own docstring for why (POST silently drops
modelNumber; PATCH on this same path is a narrow, different-fields-only
endpoint that silently no-ops on purchasePrice; PUT is the one that
actually works, and it's a genuine full replace, confirmed via source
and a live test, not a partial merge).
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
    """POST /api/v1/entities -- confirmed live that this endpoint's
    request struct (EntityCreate) has no purchasePrice field at all, and
    that a modelNumber sent here is silently dropped (came back empty on
    a real created item, not just missing from this response) -- for
    both, see set_full_details() below, which is required as a
    follow-up for either."""
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


def set_full_details(
    entity_id,
    name,
    location_id,
    description="",
    quantity=1,
    model_number="",
    manufacturer="",
    purchase_price=None,
    timeout=10,
):
    """PUT /api/v1/entities/{id} -- Homebox's PATCH endpoint on this path
    is a NARROW, separate handler (EntityPatch) that only ever touches
    quantity/parentId/entityTypeId/tagIds/importRef; modelNumber,
    manufacturer, and purchasePrice all live on the PUT endpoint instead
    (EntityUpdate), confirmed via Homebox's actual source, not guessed --
    an initial attempt using PATCH for purchasePrice returned 200 but
    silently changed nothing, which is what sent this to the source.

    PUT is a genuine full replace, confirmed via source AND live test:
    omitting parentId doesn't leave it alone, it actively clears the
    item's location (`else { ClearParent() }` in Homebox's own update
    code). So every field this item should keep has to be passed on
    every call -- this function's signature mirrors create_item()'s
    inputs for exactly that reason, not because PUT needs more
    information than POST, but because it needs the SAME information
    resent alongside whatever's actually changing.
    """
    entity_type_id = get_item_entity_type_id(timeout=timeout)
    body = {
        "name": name,
        "description": description,
        "entityTypeId": entity_type_id,
        "parentId": location_id,
        "quantity": quantity,
        "modelNumber": model_number,
        "manufacturer": manufacturer,
    }
    if purchase_price is not None:
        body["purchasePrice"] = purchase_price
    resp = requests.put(
        f"{config.HOMEBOX_URL}/api/v1/entities/{entity_id}",
        json=body,
        headers=_auth_headers(),
        timeout=timeout,
    )
    resp.raise_for_status()
    return resp.json()


def attach_photo(entity_id, image_path, title=None, primary=True, timeout=15):
    """Uploads a local image file as a photo on the entity -- the primary
    one by default; pass primary=False for extras (e.g. a dimensions
    diagram) that shouldn't replace the main picture. Confirmed
    live against the real API before relying on it: multipart fields are
    `file` (the bytes), `name` (title shown in Homebox), `type` ("photo"),
    and `primary` ("true"/"false") -- field names taken from Homebox's
    actual handler source, not guessed, then verified with a real upload
    that showed up correctly in the entity's `attachments` list."""
    title = title or image_path.rsplit("/", 1)[-1]
    with open(image_path, "rb") as f:
        files = {"file": (title, f, "image/jpeg")}
        data = {"name": title, "type": "photo", "primary": "true" if primary else "false"}
        resp = requests.post(
            f"{config.HOMEBOX_URL}/api/v1/entities/{entity_id}/attachments",
            headers=_auth_headers(),
            files=files,
            data=data,
            timeout=timeout,
        )
    resp.raise_for_status()
    return resp.json()


def import_parts(location_id, parts, timeout=10):
    """Creates several items at once in the same location -- the shape
    that comes out of reading a vendor order confirmation/receipt (a
    PDF, an order-history page, etc.): each part already has a name,
    description, quantity, usually a price and part number, and often a
    product photo already sitting right there in the document, with
    nothing left to ask a human except which bin they all go in.

    `parts` is a list of dicts: {name, description="", quantity=1,
    model_number="", manufacturer="", purchase_price=None,
    image_path=None, extra_image_paths=[]}. Returns a list of {name, id, ok, quantity,
    purchase_price} -- `ok` is a same-call verification (re-fetches each
    created item and checks name/quantity/modelNumber/purchasePrice/
    parent all match what was sent) rather than trusting the write
    response alone, since this project already found one endpoint (PATCH
    for purchasePrice) that returned success while silently changing
    nothing. `ok` does not depend on the photo upload succeeding --
    photo failures don't invalidate an otherwise-correct item, since a
    missing picture is a much smaller problem than wrong data.
    """
    results = []
    for part in parts:
        name = part["name"]
        description = part.get("description", "")
        quantity = part.get("quantity", 1)
        model_number = part.get("model_number", "")
        manufacturer = part.get("manufacturer", "")
        purchase_price = part.get("purchase_price")
        image_path = part.get("image_path")
        extra_images = part.get("extra_image_paths", [])

        created = create_item(name, location_id, description=description, quantity=quantity, timeout=timeout)
        set_full_details(
            created["id"],
            name,
            location_id,
            description=description,
            quantity=quantity,
            model_number=model_number,
            manufacturer=manufacturer,
            purchase_price=purchase_price,
            timeout=timeout,
        )
        if image_path:
            attach_photo(created["id"], image_path, title=f"{name}.jpg", timeout=timeout)
        for n, extra in enumerate(extra_images, 2):
            attach_photo(created["id"], extra, title=f"{name} ({n}).jpg", primary=False, timeout=timeout)

        resp = requests.get(
            f"{config.HOMEBOX_URL}/api/v1/entities/{created['id']}",
            headers=_auth_headers(),
            timeout=timeout,
        )
        final = resp.json()
        ok = (
            final.get("name") == name
            and final.get("quantity") == quantity
            and final.get("modelNumber") == model_number
            and (purchase_price is None or final.get("purchasePrice") == purchase_price)
            and (final.get("parent") or {}).get("id") == location_id
        )
        results.append(
            {
                "name": name,
                "id": created["id"],
                "ok": ok,
                "quantity": final.get("quantity"),
                "purchase_price": final.get("purchasePrice"),
            }
        )
    return results


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
