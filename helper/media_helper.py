#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LiteDesktopStudio Media Helper

Standalone media-metadata bridge for the C++ build.

The Python original fetches media info through WinRT (winsdk) GSMTC, which
MinGW C++ cannot call directly. This helper isolates exactly that part:

  * C++ starts this helper only when something needs media metadata
    (media widget, or a visualizer style that shows the artwork disc).
  * The helper polls GSMTC once per second (same proven fetch path as
    litedesktopstudio/runtime.py) and writes one JSON object per line to
    stdout, but ONLY when something actually changed.
  * C++ reads the lines with QProcess and updates its widgets.
  * When C++ closes the helper's stdin (or exits), the helper exits too,
    so nothing stays resident when metadata is not needed.

Dependencies: Python 3.10+ and the `winsdk` package only (no PySide).
Packaging: PyInstaller one-file build, see README_media_helper.md.

Output records:
  {"type": "ready", "available": true}
  {"type": "media", "available": true, "session": true,
   "title": "...", "artist": "...", "album": "...", "app": "...",
   "status": "Playing", "position_ms": 12345, "duration_ms": 234567,
   "thumbnail_mime": "image/jpeg", "thumbnail_b64": "..."}
  {"type": "media", "available": true, "session": false, ... empty fields}
  {"type": "timeline", "position_ms": ..., "duration_ms": ..., "status": "..."}
  {"type": "error", "error": "..."}   (emitted when availability changes)
"""

import asyncio
import base64
import hashlib
import json
import os
import sys
import threading
import time

POLL_INTERVAL_SEC = 1.0
TIMELINE_INTERVAL_SEC = 2.0


def _ensure_stdout():
    """PyInstaller --noconsole builds have sys.stdout = None; when the
    parent (C++) launches us with a pipe, fd 1 is valid, so reopen it.
    Always force UTF-8: on a Japanese Windows console the default stdout
    encoding is cp932, which would corrupt non-ASCII titles in the JSON
    stream (found in the first real-device capture, 2026-11-14)."""
    if sys.stdout is None:
        try:
            sys.stdout = os.fdopen(1, "w", encoding="utf-8", buffering=1)
        except Exception:
            pass
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


# GSMTC PlaybackStatus values. winsdk stringifies this enum differently
# across versions ("PlaybackStatus.PLAYING" in one, plain int 4 in
# another), so normalize through int whenever possible.
_PLAYBACK_STATUS_NAMES = {
    0: "Closed",
    1: "Opened",
    2: "Changing",
    3: "Stopped",
    4: "Playing",
    5: "Paused",
}


def _status_name(value):
    try:
        return _PLAYBACK_STATUS_NAMES[int(value)]
    except Exception:
        pass
    text = str(value).split(".")[-1]
    return text[:1].upper() + text[1:].lower() if text else ""


def emit(record):
    try:
        sys.stdout.write(json.dumps(record, ensure_ascii=False) + "\n")
        sys.stdout.flush()
    except Exception:
        os._exit(0)  # pipe is gone -> parent is gone


def _watch_stdin():
    """Exit when the parent closes our stdin (parent exit / stop request)."""
    try:
        if sys.stdin is None:
            return
        while True:
            line = sys.stdin.readline()
            if not line:
                break
            if line.strip().lower() in ("quit", "exit", "stop"):
                break
    except Exception:
        pass
    os._exit(0)


def _detect_image_mime_type(data):
    # Same detection as litedesktopstudio/runtime.py
    if not data:
        return ""
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if data.startswith(b"GIF87a") or data.startswith(b"GIF89a"):
        return "image/gif"
    if data.startswith(b"RIFF") and len(data) >= 12 and data[8:12] == b"WEBP":
        return "image/webp"
    if data.startswith(b"BM"):
        return "image/bmp"
    return ""


def _timespan_ms(value):
    try:
        return int(round(float(value.total_seconds()) * 1000.0))
    except Exception:
        return 0


async def _read_thumbnail_bytes(thumbnail_ref):
    # Same reader as litedesktopstudio/runtime.py
    try:
        stream = await thumbnail_ref.open_read_async()
        size = int(getattr(stream, "size", 0) or 0)
        if size <= 0:
            return b""
        from winsdk.windows.storage.streams import DataReader

        reader = DataReader(stream)
        await reader.load_async(size)
        data = bytearray(size)
        reader.read_bytes(data)
        try:
            reader.close()
        except Exception:
            pass
        return bytes(data)
    except Exception:
        return b""


async def _fetch_once():
    """One GSMTC fetch. Returns a media record dict."""
    from winsdk.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    )

    manager = await MediaManager.request_async()
    session = manager.get_current_session()

    record = {
        "type": "media",
        "available": True,
        "session": False,
        "title": "",
        "artist": "",
        "album": "",
        "app": "",
        "status": "NoSession",
        "position_ms": 0,
        "duration_ms": 0,
        "thumbnail_mime": "",
        "thumbnail_b64": "",
    }
    if session is None:
        return record

    record["session"] = True
    props = await session.try_get_media_properties_async()
    record["title"] = getattr(props, "title", "") or ""
    record["artist"] = getattr(props, "artist", "") or ""
    record["album"] = getattr(props, "album_title", "") or ""
    record["app"] = getattr(session, "source_app_user_model_id", "") or ""

    try:
        info = session.get_playback_info()
        record["status"] = _status_name(getattr(info, "playback_status", ""))
    except Exception:
        record["status"] = ""

    try:
        timeline = session.get_timeline_properties()
        start_ms = _timespan_ms(getattr(timeline, "start_time", None))
        end_ms = _timespan_ms(getattr(timeline, "end_time", None))
        pos_ms = _timespan_ms(getattr(timeline, "position", None))
        record["duration_ms"] = max(0, end_ms - start_ms)
        record["position_ms"] = max(0, pos_ms - start_ms)
    except Exception:
        pass

    thumbnail = getattr(props, "thumbnail", None)
    if thumbnail is not None:
        thumb_bytes = await _read_thumbnail_bytes(thumbnail)
        if thumb_bytes:
            record["thumbnail_mime"] = _detect_image_mime_type(thumb_bytes)
            record["thumbnail_b64"] = base64.b64encode(thumb_bytes).decode("ascii")

    return record


async def _run():
    emit({"type": "ready", "available": True})
    last_key = None
    last_error = None
    last_timeline_emit = 0.0
    last_position_ms = -1

    while True:
        try:
            record = await _fetch_once()
        except Exception as e:  # winsdk missing, no media stack, ...
            error = repr(e)
            if error != last_error:
                last_error = error
                last_key = None
                emit({"type": "error", "error": error})
            await asyncio.sleep(5.0)
            continue

        if last_error is not None:
            last_error = None
            emit({"type": "ready", "available": True})

        thumb_hash = hashlib.sha1(record["thumbnail_b64"].encode("ascii")).hexdigest()
        key = (
            record["session"],
            record["title"],
            record["artist"],
            record["album"],
            record["app"],
            record["status"],
            thumb_hash,
        )
        now = time.monotonic()
        if key != last_key:
            last_key = key
            last_position_ms = record["position_ms"]
            last_timeline_emit = now
            emit(record)
        elif (
            record["session"]
            and record["status"] in ("Playing", "Paused")
            and record["position_ms"] != last_position_ms
            and now - last_timeline_emit >= TIMELINE_INTERVAL_SEC
        ):
            last_position_ms = record["position_ms"]
            last_timeline_emit = now
            emit(
                {
                    "type": "timeline",
                    "position_ms": record["position_ms"],
                    "duration_ms": record["duration_ms"],
                    "status": record["status"],
                }
            )

        await asyncio.sleep(POLL_INTERVAL_SEC)


def main():
    _ensure_stdout()
    threading.Thread(target=_watch_stdin, daemon=True).start()
    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
