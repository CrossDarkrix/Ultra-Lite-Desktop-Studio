#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LiteDesktopStudio Media Control Helper

One-shot media TRANSPORT control for the C++ build, separate from the
metadata helper on purpose (user decision 2026-11-15): synthesized media
keys (keybd_event / SendInput) did not move the player on the target
machine, while GSMTC session control is the exact channel the metadata
already flows through, so it cannot miss the player.

Usage:
  LiteDesktopStudio-MediaControlHelper.exe <command>

  command: play | pause | play_pause | next | prev | stop

The helper asks GSMTC for the CURRENT session, calls the matching
try_*_async() method, prints one JSON line with the result, and exits:
  {"ok": true, "cmd": "play_pause", "status_before": 5}
  {"ok": false, "cmd": "next", "error": "no current session"}

Exit codes: 0 = command accepted, 2 = no session, 3 = player refused,
1 = exception, 64 = bad usage.

C++ starts this helper detached per button press (CREATE_NO_WINDOW);
nothing stays resident. Dependencies: Python 3.10+ and `winsdk` only.
Packaging: Nuitka standalone (onedir), see README_media_helper.md.
"""

import asyncio
import json
import os
import sys

COMMANDS = ("play", "pause", "play_pause", "next", "prev", "stop")

# GlobalSystemMediaTransportControlsSessionPlaybackStatus
STATUS_PLAYING = 4


def _ensure_stdout():
    """Same UTF-8 handling as media_helper.py (Japanese Windows would
    otherwise emit cp932 and corrupt the JSON line)."""
    if sys.stdout is None:
        try:
            sys.stdout = os.fdopen(1, "w", encoding="utf-8", buffering=1)
        except Exception:
            pass
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def _emit(obj):
    print(json.dumps(obj, ensure_ascii=False), flush=True)


async def _run(cmd):
    from winsdk.windows.media.control import (
        GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    )

    manager = await MediaManager.request_async()
    session = manager.get_current_session()
    if session is None:
        _emit({"ok": False, "cmd": cmd, "error": "no current session"})
        return 2

    status = None
    try:
        status = int(session.get_playback_info().playback_status)
    except Exception:
        pass

    if cmd == "play":
        ok = await session.try_play_async()
    elif cmd == "pause":
        ok = await session.try_pause_async()
    elif cmd == "play_pause":
        if status == STATUS_PLAYING:
            ok = await session.try_pause_async()
        else:
            ok = await session.try_play_async()
    elif cmd == "next":
        ok = await session.try_skip_next_async()
    elif cmd == "prev":
        ok = await session.try_skip_previous_async()
    elif cmd == "stop":
        ok = await session.try_stop_async()
    else:  # unreachable, main() validates
        ok = False

    _emit({"ok": bool(ok), "cmd": cmd, "status_before": status})
    return 0 if ok else 3


def main():
    _ensure_stdout()
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in COMMANDS:
        _emit({"ok": False, "error": "usage: media_control_helper <" + "|".join(COMMANDS) + ">"})
        return 64
    try:
        return asyncio.run(_run(cmd))
    except Exception as exc:  # winsdk missing, no GSMTC, ...
        _emit({"ok": False, "cmd": cmd, "error": repr(exc)})
        return 1


if __name__ == "__main__":
    sys.exit(main())
