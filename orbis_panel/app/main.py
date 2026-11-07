"""Orbis Panel Editor — Home Assistant add-on (zero dependency).

Standard library only: no pip, no Flask, no requests. Serves a small web editor
for the panel's JSON layout and writes it into Home Assistant's www folder, so
the panel can fetch it from `<ha>/local/orbis/layout.json`.

Environment:
    LAYOUT_PATH       where to write the layout (default /config/www/orbis/layout.json)
    SUPERVISOR_TOKEN  injected by the Supervisor, used to list entities
"""

import json
import os
import hashlib
import tempfile
import time
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

APP_DIR = os.path.dirname(os.path.abspath(__file__))
LAYOUT_PATH = os.environ.get("LAYOUT_PATH", "/config/www/orbis/layout.json")
CORE_API = "http://supervisor/core/api"
TOKEN = os.environ.get("SUPERVISOR_TOKEN", "")
PORT = int(os.environ.get("PORT", "8099"))

WWW_DIR = os.path.dirname(LAYOUT_PATH)
FW_PATH = os.path.join(WWW_DIR, "firmware.bin")
FW_META = os.path.join(WWW_DIR, "firmware.json")
MAX_FW = 4 * 1024 * 1024  # app 分区 4MB

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


def read_layout():
    if os.path.exists(LAYOUT_PATH):
        with open(LAYOUT_PATH, "r", encoding="utf-8") as fh:
            return json.load(fh)
    return json.loads(json.dumps(DEFAULT_LAYOUT))


def write_layout(data):
    folder = os.path.dirname(LAYOUT_PATH) or "."
    os.makedirs(folder, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder)
    with os.fdopen(fd, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
    os.replace(tmp, LAYOUT_PATH)


def read_fw_meta():
    if os.path.exists(FW_META):
        try:
            with open(FW_META, "r", encoding="utf-8") as fh:
                return json.load(fh)
        except Exception:  # noqa: BLE001
            pass
    return None


def write_firmware(data, version):
    """保存固件并生成 firmware.json（设备端按 version 判断是否需要升级）。"""
    os.makedirs(WWW_DIR, exist_ok=True)
    with open(FW_PATH, "wb") as fh:
        fh.write(data)
    meta = {
        "version": version,
        "file": "firmware.bin",
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "uploaded_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }
    with open(FW_META, "w", encoding="utf-8") as fh:
        json.dump(meta, fh, ensure_ascii=False, indent=2)
    return meta


def ha_states():
    if not TOKEN:
        raise RuntimeError("SUPERVISOR_TOKEN is not set (run inside the Supervisor)")
    req = urllib.request.Request(CORE_API + "/states", headers={"Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.load(resp)


def validate_layout(data):
    if not isinstance(data, dict):
        return "body must be a JSON object"
    pages = data.get("pages")
    if not isinstance(pages, list) or not pages:
        return "pages[] is required"
    for page in pages:
        if not isinstance(page, dict) or not page.get("name"):
            return "every page needs a name"
        tiles = page.get("tiles", [])
        if not isinstance(tiles, list):
            return "tiles must be an array"
        for tile in tiles:
            if not isinstance(tile, dict) or not tile.get("entity"):
                return "every tile needs an entity"
    return None


class Handler(BaseHTTPRequestHandler):
    server_version = "OrbisPanel/0.1"

    # --- helpers ---------------------------------------------------------
    def send_json(self, obj, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_file(self, name, ctype):
        path = os.path.join(APP_DIR, name)
        if not os.path.exists(path):
            self.send_json({"ok": False, "error": "not found"}, 404)
            return
        with open(path, "rb") as fh:
            body = fh.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def route(self):
        # HA ingress may keep a path prefix; compare the tail of the path
        path = urllib.parse.urlparse(self.path).path.rstrip("/") or "/"
        return path

    # --- GET -------------------------------------------------------------
    def do_GET(self):
        path = self.route()
        if path in ("/", "/index.html") or path.endswith("/index.html"):
            self.send_file("index.html", "text/html; charset=utf-8")
        elif path.endswith("/api/layout"):
            try:
                self.send_json({"ok": True, "layout": read_layout(), "layout_path": LAYOUT_PATH})
            except Exception as exc:  # noqa: BLE001
                self.send_json({"ok": False, "error": str(exc)}, 500)
        elif path.endswith("/api/health"):
            self.send_json(
                {
                    "ok": True,
                    "layout_path": LAYOUT_PATH,
                    "layout_exists": os.path.exists(LAYOUT_PATH),
                    "token": bool(TOKEN),
                }
            )
        elif path.endswith("/api/firmware"):
            meta = read_fw_meta()
            self.send_json(
                {
                    "ok": True,
                    "firmware": meta,
                    "present": os.path.exists(FW_PATH),
                    "max_size": MAX_FW,
                }
            )
        elif path.endswith("/api/entities"):
            query = urllib.parse.urlparse(self.path).query
            params = urllib.parse.parse_qs(query)
            q = (params.get("q") or [""])[0].lower()
            domain = (params.get("domain") or [""])[0]
            try:
                states = ha_states()
            except Exception as exc:  # noqa: BLE001
                self.send_json({"ok": False, "error": str(exc), "entities": []}, 502)
                return
            out = []
            for state in states:
                entity_id = state.get("entity_id", "")
                dom = entity_id.split(".", 1)[0]
                if domain and dom != domain:
                    continue
                name = (state.get("attributes") or {}).get("friendly_name") or entity_id
                if q and q not in entity_id.lower() and q not in str(name).lower():
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
            self.send_json({"ok": True, "entities": out[:800], "count": len(out)})
        else:
            self.send_json({"ok": False, "error": "not found: " + path}, 404)

    # --- POST ------------------------------------------------------------
    def do_POST(self):
        path = self.route()
        if path.endswith("/api/firmware"):
            self.handle_firmware_upload()
            return
        if not path.endswith("/api/layout"):
            self.send_json({"ok": False, "error": "not found"}, 404)
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            data = json.loads(raw.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            self.send_json({"ok": False, "error": "invalid JSON: %s" % exc}, 400)
            return

        err = validate_layout(data)
        if err:
            self.send_json({"ok": False, "error": err}, 400)
            return

        data["version"] = int(data.get("version", 0)) + 1
        try:
            write_layout(data)
        except Exception as exc:  # noqa: BLE001
            self.send_json({"ok": False, "error": str(exc)}, 500)
            return
        self.send_json({"ok": True, "version": data["version"], "layout_path": LAYOUT_PATH})

    def handle_firmware_upload(self):
        params = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        version = (params.get("version") or [""])[0].strip()
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            self.send_json({"ok": False, "error": "empty body"}, 400)
            return
        if length > MAX_FW:
            self.send_json({"ok": False, "error": "firmware larger than %d bytes" % MAX_FW}, 413)
            return
        data = self.rfile.read(length)
        # ESP32 app image starts with 0xE9; also accept merged images offset by bootloader
        if not data or data[0] != 0xE9:
            self.send_json(
                {"ok": False, "error": "not an ESP32 app image (first byte 0x%02x)" % (data[0] if data else 0)},
                400,
            )
            return
        if not version:
            self.send_json({"ok": False, "error": "version is required (e.g. 0.1.1)"}, 400)
            return
        try:
            meta = write_firmware(data, version)
        except Exception as exc:  # noqa: BLE001
            self.send_json({"ok": False, "error": str(exc)}, 500)
            return
        print("firmware %s saved (%d bytes, sha256 %s...)" % (version, len(data), meta["sha256"][:12]), flush=True)
        self.send_json({"ok": True, "firmware": meta})

    # keep logs short; the Supervisor captures stdout/stderr
    def log_message(self, fmt, *args):
        print("%s - %s" % (self.address_string(), fmt % args), flush=True)


def main():
    os.makedirs(os.path.dirname(LAYOUT_PATH) or ".", exist_ok=True)
    print("Orbis Panel Editor on 0.0.0.0:%d, layout=%s" % (PORT, LAYOUT_PATH), flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
