"""Orbis Panel Editor — Home Assistant add-on.

Serves a small web editor for the JSON layout of the JC3636W518 control panel
and writes it to Home Assistant's www folder, so the panel can fetch it from
`<ha>/local/orbis/layout.json`.

Environment:
    LAYOUT_PATH       where to write the layout (default /config/www/orbis/layout.json)
    SUPERVISOR_TOKEN  injected by the Supervisor, used to list entities
"""

import json
import os
import tempfile

import requests
from flask import Flask, jsonify, request, send_from_directory

APP_DIR = os.path.dirname(os.path.abspath(__file__))
LAYOUT_PATH = os.environ.get("LAYOUT_PATH", "/config/www/orbis/layout.json")
CORE_API = "http://supervisor/core/api"
TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")

DEFAULT_LAYOUT = {
    "version": 1,
    "title": "Home",
    "pages": [
        {
            "name": "Devices",
            "tiles": [
                {"entity": "light.example", "label": "示例灯", "type": "toggle"},
            ],
        }
    ],
}

app = Flask(__name__, static_folder=None)


def read_layout():
    if os.path.exists(LAYOUT_PATH):
        with open(LAYOUT_PATH, "r", encoding="utf-8") as fh:
            return json.load(fh)
    return dict(DEFAULT_LAYOUT)


def write_layout(data):
    os.makedirs(os.path.dirname(LAYOUT_PATH), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(LAYOUT_PATH))
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
    os.replace(tmp, LAYOUT_PATH)


def ha_headers():
    return {"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"}


@app.get("/")
def index():
    return send_from_directory(APP_DIR, "index.html")


@app.get("/api/health")
def health():
    return jsonify(
        {
            "ok": True,
            "layout_path": LAYOUT_PATH,
            "layout_exists": os.path.exists(LAYOUT_PATH),
            "token": bool(TOKEN),
        }
    )


@app.get("/api/layout")
def get_layout():
    try:
        return jsonify({"ok": True, "layout": read_layout(), "layout_path": LAYOUT_PATH})
    except Exception as exc:  # noqa: BLE001
        return jsonify({"ok": False, "error": str(exc)}), 500


@app.post("/api/layout")
def set_layout():
    data = request.get_json(silent=True)
    if not isinstance(data, dict):
        return jsonify({"ok": False, "error": "body must be a JSON object"}), 400
    pages = data.get("pages")
    if not isinstance(pages, list) or not pages:
        return jsonify({"ok": False, "error": "pages[] is required"}), 400
    for page in pages:
        if not isinstance(page, dict) or not page.get("name"):
            return jsonify({"ok": False, "error": "every page needs a name"}), 400
        tiles = page.get("tiles", [])
        if not isinstance(tiles, list):
            return jsonify({"ok": False, "error": "tiles must be an array"}), 400
        for tile in tiles:
            if not isinstance(tile, dict) or not tile.get("entity"):
                return jsonify({"ok": False, "error": "every tile needs an entity"}), 400

    data["version"] = int(data.get("version", 0)) + 1
    try:
        write_layout(data)
    except Exception as exc:  # noqa: BLE001
        return jsonify({"ok": False, "error": str(exc)}), 500
    return jsonify({"ok": True, "version": data["version"], "layout_path": LAYOUT_PATH})


@app.get("/api/entities")
def entities():
    query = (request.args.get("q") or "").lower()
    domain = request.args.get("domain") or ""
    try:
        resp = requests.get(CORE_API + "/states", headers=ha_headers(), timeout=10)
        resp.raise_for_status()
        states = resp.json()
    except Exception as exc:  # noqa: BLE001
        return jsonify({"ok": False, "error": str(exc), "entities": []}), 502

    out = []
    for state in states:
        entity_id = state.get("entity_id", "")
        dom = entity_id.split(".", 1)[0]
        if domain and dom != domain:
            continue
        name = (state.get("attributes") or {}).get("friendly_name") or entity_id
        if query and query not in entity_id.lower() and query not in str(name).lower():
            continue
        out.append(
            {
                "entity_id": entity_id,
                "domain": dom,
                "name": name,
                "state": state.get("state"),
                "unit": (state.get("attributes") or {}).get("unit_of_measurement"),
            }
        )
    out.sort(key=lambda e: (e["domain"], e["name"]))
    return jsonify({"ok": True, "entities": out[:800], "count": len(out)})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8099)
