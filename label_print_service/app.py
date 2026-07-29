"""Small internal web service that prints Homebox location bin labels on
the Brother P-touch D610BT via the b-PAC SDK's COM automation, using the
LocationBinLabel.lbx template checked into this repo's root.

Runs ON the Windows PC the printer is USB-attached to (192.168.30.39 at
time of writing) — b-PAC is a Windows-only COM library, this can't run
anywhere else. See ../README.md's "Label Printing" section for the full
setup/deployment walkthrough.

Deliberately single-threaded (Flask's dev server default): the b-PAC
Document object is COM state, not safe to share across concurrent
requests, and print jobs from this tool are low-volume/infrequent, so
serializing them is simpler than adding locking for no real benefit.
"""

import config
import requests
from flask import Flask, jsonify, render_template, request

app = Flask(__name__)


def _auth_headers():
    return {"Authorization": config.HOMEBOX_AUTH_HEADER}


def fetch_locations():
    """Flat list of every Homebox location, grouped by parent unit.

    There's no separate /locations route — confirmed against Homebox's
    actual backend source (sysadminsmedia/homebox,
    backend/app/api/routes.go + repo_entities.go): locations and items
    share GET /api/v1/entities, which defaults to items-only for backward
    compatibility, so listing locations needs the explicit
    isLocation=true query param. The response is a
    PaginationResult[EntitySummary] ({"items": [...], ...}), and
    EntitySummary already carries "description" and a nested "parent"
    (itself an EntitySummary with "id"/"name") directly — no per-item
    detail fetch needed just to build this list. Omitting page/pageSize
    entirely returns every location unpaginated (repo_entities.go only
    applies Offset/Limit when at least one of them is set).
    """
    resp = requests.get(
        f"{config.HOMEBOX_URL}/api/v1/entities",
        params={"isLocation": "true"},
        headers=_auth_headers(),
        timeout=10,
    )
    resp.raise_for_status()
    data = resp.json()
    raw = data.get("items", data) if isinstance(data, dict) else data

    locations = []
    for loc in raw:
        parent = loc.get("parent") or {}
        locations.append(
            {
                "id": loc["id"],
                "name": loc.get("name", ""),
                "parent_id": parent.get("id") or loc.get("parentId") or "",
                "parent_name": parent.get("name") or "Ungrouped",
            }
        )
    locations.sort(key=lambda loc: (loc["parent_name"], loc["name"]))
    return locations


def fetch_location_detail(location_id):
    """Full detail for one location at print time — deliberately a fresh
    call per id rather than trusting fetch_locations()'s list response,
    since that's the endpoint this project has already confirmed
    eager-loads what's needed (see display_automation.yaml's
    location_detail_resp / homebox_get_entity)."""
    resp = requests.get(
        f"{config.HOMEBOX_URL}/api/v1/entities/{location_id}",
        headers=_auth_headers(),
        timeout=10,
    )
    resp.raise_for_status()
    data = resp.json()
    return {
        "id": location_id,
        "name": data.get("name", ""),
        "description": data.get("description", ""),
    }


def print_labels(locations):
    """Opens LocationBinLabel.lbx once via b-PAC and prints one label per
    location, setting the three template objects confirmed by inspecting
    the .lbx (label.xml): Text5=name, Text7=description, Barcode1=QR data.

    Confirmed against real hardware — the Open/GetObject/StartPrint/
    PrintOut/EndPrint sequence below prints correctly on the D610BT.

    Deliberately never calls doc.Close(): under plain (late-bound)
    win32com.client.Dispatch, Close() fails with "TypeError: 'bool'
    object is not callable" — win32com can't tell a zero-arg method from
    a property without the real COM type library, guesses property, and
    hands back Close's boolean return value instead of letting it be
    called. The usual fix (win32com.client.gencache.EnsureDispatch, which
    builds a typed wrapper from the type library so this resolves
    correctly) doesn't work here either — b-PAC's COM object fails
    gencache's own GetTypeInfo() call ("This COM object can not automate
    the makepy process"). Simplest working option: don't call Close() at
    all — dropping the last Python reference to doc (the `doc = None` in
    the finally block below) releases the underlying COM object via
    normal reference counting, which is what Close() would have
    triggered anyway.

    Returns (success_count, errors) where errors is a list of
    {id, name, error} for locations that failed to print (doesn't abort
    the whole batch on one bad label).
    """
    import pythoncom
    import win32com.client

    pythoncom.CoInitialize()
    doc = None
    try:
        doc = win32com.client.Dispatch("bpac.Document")
        if not doc.Open(config.LBX_TEMPLATE_PATH):
            raise RuntimeError(
                f"b-PAC could not open template: {config.LBX_TEMPLATE_PATH}"
            )
        success = 0
        errors = []
        for loc in locations:
            try:
                doc.GetObject("Text5").Text = loc["name"] or ""
                doc.GetObject("Text7").Text = loc["description"] or ""
                doc.GetObject("Barcode1").Text = (
                    f"{config.HOMEBOX_URL}/location/{loc['id']}"
                )
                doc.StartPrint("", 0)
                doc.PrintOut(1, 0)
                doc.EndPrint()
                success += 1
            except Exception as e:  # noqa: BLE001 - surface per-label, keep going
                errors.append({"id": loc["id"], "name": loc.get("name"), "error": str(e)})
        return success, errors
    finally:
        doc = None
        pythoncom.CoUninitialize()


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/locations")
def locations_route():
    try:
        return jsonify(fetch_locations())
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502


@app.route("/print", methods=["POST"])
def print_route():
    body = request.get_json(force=True, silent=True) or {}
    ids = body.get("location_ids") or []
    if not ids:
        return jsonify({"error": "location_ids is required and must be non-empty"}), 400

    resolved = []
    fetch_errors = []
    for location_id in ids:
        try:
            resolved.append(fetch_location_detail(location_id))
        except requests.RequestException as e:
            fetch_errors.append({"id": location_id, "error": str(e)})

    printed, print_errors = (0, [])
    if resolved:
        printed, print_errors = print_labels(resolved)

    return jsonify(
        {
            "requested": len(ids),
            "printed": printed,
            "fetch_errors": fetch_errors,
            "print_errors": print_errors,
        }
    )


if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT)
