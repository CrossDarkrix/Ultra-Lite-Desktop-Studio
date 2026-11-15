#pragma once
#include <QtGlobal>
#include <QString>

// Small Windows platform helpers. Non-Windows builds get stubs returning
// failure / -1 so widgets can show a placeholder.
namespace Platform {
// Master volume 0..100, -1 when unavailable.
int getVolumePercent();
bool setVolumePercent(int percent);
bool getMute();
bool setMute(bool mute);
// Media keys (VK_MEDIA_* via keybd_event on Windows).
// Return true when the key events were actually delivered to the OS.
bool mediaPlayPause();
bool mediaNext();
bool mediaPrevious();
bool mediaStop();
} // namespace Platform

// Desktop layer hosting (Windows, v0.8.2): moves a top-level window into
// the desktop's WorkerW layer so it renders as part of the wallpaper —
// beneath the desktop icons and every normal window. Mirrors the Python
// build's attach_background_effects_to_desktop_host /
// lds_detach_window_from_desktop_host. Non-Windows builds return false.
// Appends one timestamped line to ~/LiteDesktopStudio_debug.log and
// flushes immediately, so the last line on disk localizes a hang.
// Gate (v0.13.3, the user's request): logging is OFF by default and
// is enabled only from the tray ("デバッグログをオン", persisted as
// the config root key "debug_log"). Every writer goes through
// debugLog(), which is a no-op while disabled — some writers are
// chatty (the desktop-icon layer logs a line per 1.5 s cache fill),
// so an always-on log grew without bound in long sessions. Even
// while enabled, debugLog() rotates the file once at ~5 MB (one
// .old generation), capping the pair at ~10 MB.
void debugLog(const QString &msg);
void setDebugLogEnabled(bool on);
bool debugLogEnabled();

namespace DesktopHost {
// Reparents `hwnd` into a desktop WorkerW layer, restyles it as a child
// window and sizes it to the virtual screen. Tries every WorkerW
// candidate and verifies the result (parent set + host visible); when no
// candidate works it leaves the window untouched and returns false.
bool attach(quintptr hwnd);
// Restores `hwnd` as a normal bottom-most popup window.
bool detach(quintptr hwnd);
// Current native parent of `hwnd` (0 when it is a top-level window).
quintptr parentOf(quintptr hwnd);
} // namespace DesktopHost
