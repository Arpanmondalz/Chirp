#!/usr/bin/env python3
"""Chirp file manager.

Drag music in, it gets transcoded and renamed to what the player can handle,
then you pick a folder already on the card and hit Sync.

    python chirp_web.py     ->  http://127.0.0.1:5000

The device must be on Screen 3 with sync mode armed (hold the centre button)
before the page can talk to it.
"""

from __future__ import annotations

import shutil
import sys
import threading
import uuid
from pathlib import Path

try:
    from flask import Flask, jsonify, render_template, request
except ImportError:
    sys.exit("Missing dependency. Run: pip install -r requirements.txt")

from chirp_link import ChirpError, find_device
from clean_music import AUDIO_EXTS, clean_filename, convert, find_ffmpeg

HOST = "127.0.0.1"
PORT = 5000
STAGING = Path(__file__).parent / "_staging"
MAX_UPLOAD_MB = 512

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = MAX_UPLOAD_MB * 1024 * 1024

# One serial link at a time, so two browser tabs can't interleave transfers.
_serial_lock = threading.Lock()

_items: dict[str, dict] = {}
_items_lock = threading.Lock()
_sync_state = {"running": False, "done": 0, "total": 0, "current": "", "log": []}


def _set(item_id: str, **kw) -> None:
    with _items_lock:
        if item_id in _items:
            _items[item_id].update(kw)


def _convert_worker(item_id: str, raw: Path) -> None:
    ffmpeg = find_ffmpeg()
    out = STAGING / f"{item_id}.mp3"
    _set(item_id, status="converting")
    ok, err = convert(ffmpeg, raw, out)
    raw.unlink(missing_ok=True)
    if ok:
        _set(item_id, status="ready", size=out.stat().st_size)
    else:
        out.unlink(missing_ok=True)
        _set(item_id, status="failed", error=err)


@app.get("/")
def index():
    return render_template("index.html")


@app.post("/api/upload")
def upload():
    added = []
    for storage in request.files.getlist("files"):
        src_name = Path(storage.filename or "").name
        if not src_name or Path(src_name).suffix.lower() not in AUDIO_EXTS:
            continue
        item_id = uuid.uuid4().hex[:12]
        raw = STAGING / f"{item_id}{Path(src_name).suffix.lower()}"
        STAGING.mkdir(parents=True, exist_ok=True)
        storage.save(raw)

        name = f"{clean_filename(Path(src_name).stem)}.mp3"
        with _items_lock:
            _items[item_id] = {"id": item_id, "source": src_name, "name": name,
                               "status": "queued", "size": 0, "error": ""}
        added.append(item_id)
        threading.Thread(target=_convert_worker, args=(item_id, raw), daemon=True).start()
    return jsonify(added=added)


@app.get("/api/staging")
def staging():
    with _items_lock:
        return jsonify(items=list(_items.values()), sync=_sync_state)


@app.delete("/api/staging/<item_id>")
def staging_remove(item_id: str):
    with _items_lock:
        _items.pop(item_id, None)
    (STAGING / f"{item_id}.mp3").unlink(missing_ok=True)
    return jsonify(ok=True)


@app.get("/api/device")
def device():
    try:
        with _serial_lock, find_device() as link:
            info = link.hello()
            return jsonify(ok=True, info=info, dirs=link.dirs())
    except ChirpError as exc:
        return jsonify(ok=False, error=str(exc))


@app.get("/api/device/files")
def device_files():
    directory = request.args.get("dir", "/")
    try:
        with _serial_lock, find_device() as link:
            if directory not in link.dirs():
                return jsonify(ok=False, error="unknown folder")
            return jsonify(ok=True, files=link.list(directory))
    except ChirpError as exc:
        return jsonify(ok=False, error=str(exc))


@app.post("/api/device/delete")
def device_delete():
    path = (request.json or {}).get("path", "")
    if not path.startswith("/") or ".." in path:
        return jsonify(ok=False, error="bad path")
    try:
        with _serial_lock, find_device() as link:
            link.delete(path)
            return jsonify(ok=True)
    except ChirpError as exc:
        return jsonify(ok=False, error=str(exc))


def _sync_worker(target: str, ids: list[str]) -> None:
    _sync_state.update(running=True, done=0, total=len(ids), current="", log=[])
    try:
        with _serial_lock, find_device() as link:
            # The folder must be one the device reported; never trust the browser.
            if target not in link.dirs():
                _sync_state["log"].append(f"unknown folder {target}")
                return
            existing = {f["n"].lower() for f in link.list(target)}
            prefix = "" if target == "/" else target

            for item_id in ids:
                with _items_lock:
                    item = dict(_items.get(item_id, {}))
                if item.get("status") != "ready":
                    continue
                _sync_state["current"] = item["name"]

                if item["name"].lower() in existing:
                    _set(item_id, status="skipped")
                    _sync_state["log"].append(f"skipped {item['name']} (already there)")
                else:
                    local = STAGING / f"{item_id}.mp3"
                    try:
                        link.put(local, f"{prefix}/{item['name']}")
                        _set(item_id, status="uploaded")
                        _sync_state["log"].append(f"uploaded {item['name']}")
                    except ChirpError as exc:
                        _set(item_id, status="failed", error=str(exc))
                        _sync_state["log"].append(f"FAILED {item['name']}: {exc}")
                _sync_state["done"] += 1
    except ChirpError as exc:
        _sync_state["log"].append(str(exc))
    finally:
        _sync_state.update(running=False, current="")


@app.post("/api/sync")
def sync():
    if _sync_state["running"]:
        return jsonify(ok=False, error="already running")
    body = request.json or {}
    target = body.get("folder", "")
    if not target.startswith("/") or ".." in target:
        return jsonify(ok=False, error="bad folder")
    with _items_lock:
        ids = [i for i, v in _items.items() if v["status"] == "ready"]
    if not ids:
        return jsonify(ok=False, error="nothing ready to send")
    threading.Thread(target=_sync_worker, args=(target, ids), daemon=True).start()
    return jsonify(ok=True, count=len(ids))


@app.post("/api/clear")
def clear():
    with _items_lock:
        _items.clear()
    shutil.rmtree(STAGING, ignore_errors=True)
    return jsonify(ok=True)


if __name__ == "__main__":
    STAGING.mkdir(parents=True, exist_ok=True)
    print(f"Chirp file manager -> http://{HOST}:{PORT}")
    # Localhost only: this exposes unauthenticated file upload and delete.
    app.run(host=HOST, port=PORT, debug=False)
