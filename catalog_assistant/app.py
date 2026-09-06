"""Small internal web page wrapping restock_item.py's search -> confirm
-> patch -> flash flow, meant to be pulled up on a phone while standing
at the shelf with a part in hand, not typed at a terminal. Same
"independent piece talking to Homebox/HA" pattern as label_print_service
-- see its app.py for the sibling this one is modeled on.

Reuses restock_item.py's Homebox calls directly rather than duplicating
them; this file only adds the web layer around them.
"""

import config
import requests
from flask import Flask, jsonify, render_template, request

from restock_item import MAX_SHOWN, flash_bin, patch_quantity, search_items

app = Flask(__name__)


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/search")
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


if __name__ == "__main__":
    app.run(host=config.HOST, port=config.PORT)
