"""Small internal web page wrapping restock_item.py's search -> confirm
-> patch -> flash flow, meant to be pulled up on a phone while standing
at the shelf with a part in hand, not typed at a terminal. Same
"independent piece talking to Homebox/HA" pattern as label_print_service
-- see its app.py for the sibling this one is modeled on.

Reuses restock_item.py's Homebox calls directly rather than duplicating
them; this file only adds the web layer around them.

Gates every route behind a shared-secret login (config.APP_SECRET) --
this app writes to Homebox (quantity, bin-flash) and was found to be
reachable completely unauthenticated by anything else on the same LAN
segment as deployed (a fronting reverse-proxy/SSO layer only guards a
separate public path in, not this app's own LAN address -- see
../README.md's Cataloging Assistant -> Security note / Open Items for
the full story). This check exists independent of network topology so
it holds regardless of how or where this gets deployed next.
"""

from datetime import timedelta
from functools import wraps

import config
import requests
from flask import Flask, jsonify, redirect, render_template, request, session, url_for

from create_item import create_item, search_locations
from restock_item import MAX_SHOWN, flash_bin, patch_quantity, search_items

app = Flask(__name__)
app.secret_key = config.FLASK_SESSION_KEY
app.config["PERMANENT_SESSION_LIFETIME"] = timedelta(days=30)


def login_required(view):
    @wraps(view)
    def wrapped(*args, **kwargs):
        if not session.get("authenticated"):
            if request.path == "/" or request.path == "/login":
                return redirect(url_for("login", next=request.path))
            # /search, /restock -- called by the page's own JS, not a
            # browser navigation, so redirecting would just confuse a
            # fetch() call. A plain 401 is what the frontend can act on.
            return jsonify({"error": "not authenticated"}), 401
        return view(*args, **kwargs)

    return wrapped


@app.route("/login", methods=["GET", "POST"])
def login():
    error = None
    if request.method == "POST":
        if request.form.get("secret") == config.APP_SECRET:
            session.permanent = True
            session["authenticated"] = True
            return redirect(request.args.get("next") or url_for("index"))
        error = "Incorrect password."
    return render_template("login.html", error=error)


@app.route("/logout", methods=["POST"])
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.route("/")
@login_required
def index():
    return render_template("index.html")


@app.route("/search")
@login_required
def search_route():
    query = request.args.get("q", "").strip()
    if not query:
        return jsonify({"items": [], "total": 0})
    try:
        items = search_items(query)
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502
    return jsonify({"items": items[:MAX_SHOWN], "total": len(items)})


@app.route("/restock", methods=["POST"])
@login_required
def restock_route():
    body = request.get_json(force=True, silent=True) or {}
    item_id = body.get("item_id")
    new_quantity = body.get("new_quantity")
    if not item_id or new_quantity is None:
        return jsonify({"error": "item_id and new_quantity are required"}), 400

    try:
        patch_quantity(item_id, new_quantity)
        flash_bin(item_id)
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502

    return jsonify({"ok": True})


@app.route("/search_locations")
@login_required
def search_locations_route():
    query = request.args.get("q", "").strip()
    if not query:
        return jsonify({"locations": [], "total": 0})
    try:
        locations = search_locations(query)
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502
    return jsonify({"locations": locations[:MAX_SHOWN], "total": len(locations)})


@app.route("/create_item", methods=["POST"])
@login_required
def create_item_route():
    body = request.get_json(force=True, silent=True) or {}
    name = (body.get("name") or "").strip()
    location_id = body.get("location_id")
    description = (body.get("description") or "").strip()
    quantity = body.get("quantity", 1)
    if not name or not location_id:
        return jsonify({"error": "name and location_id are required"}), 400

    try:
        created = create_item(name, location_id, description=description, quantity=quantity)
        flash_bin(created["id"])
    except requests.RequestException as e:
        return jsonify({"error": str(e)}), 502

    return jsonify({"ok": True})


if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT)
