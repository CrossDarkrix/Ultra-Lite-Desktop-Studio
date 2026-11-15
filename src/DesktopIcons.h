#pragma once
#include <QPixmap>
#include <QRect>
#include <QString>
#include <QVector>

// Desktop icon scene integration (v0.9.0).
//
// The Explorer desktop ListView (SysListView32) stays the source of truth
// for icons. This module reads its items cross-process (positions, labels,
// shell image indices, selection state) and offers the small input-proxy
// operations the scene overlay needs. On non-Windows builds every call
// returns empty / false.
//
// Ported from the Python build's _lds_* icon-scene functions, with one
// simplification: Python draws the *effects* integration (shadow /
// reflection / glow / fog) under the REAL icons from a window layered
// under the desktop; the C++ effects overlay sits above the desktop, so
// it also paints the icon images and labels themselves (cached pixmaps),
// and forwards mouse input to the real ListView.
namespace DesktopIcons {

struct IconItem {
    int index = -1;
    QString text;
    int imageIndex = -1;
    bool selected = false;
    bool focused = false;
    // Item bounds (icon + label) in the overlay's local coordinates: the
    // caller passes the overlay's global origin, items come back local.
    QRect bounds;
    QPoint pos; // LVM_GETITEMPOSITION (icon top-left), same space
};

// Reads the desktop icons (cached internally for ~1.5s like Python's
// LITEDESKTOPSTUDIO_ICON_SCENE_SYNC_INTERVAL). localOrigin is the global
// position of the overlay's (0,0). Empty when unavailable.
QVector<IconItem> readItems(const QPoint &localOrigin, const QSize &canvasSize);
// Drops the read cache so the next readItems() re-queries (after input).
void invalidateCache();

// The icon image for a desktop item (48px, cached). The ListView's
// image-list handle is process-local to Explorer and cannot be drawn
// from outside (v0.9.0/0.9.1 read NULL for every item), so — like
// Python's _lds_extract_desktop_icon_image_for_item — the icon is
// resolved from the item's label: find the file on the desktop folders
// and ask the shell for its icon (SHGetFileInfo). Null when unresolved.
QPixmap iconImageFor(const IconItem &item);

// Forwards a mouse message (WM_LBUTTONDOWN etc.) to the desktop
// ListView. clientPt is in the overlay's local coordinates; conversion
// to ListView client space happens inside (via the screen).
bool postMouse(quint32 message, const QPoint &localPt, const QPoint &globalOrigin, quintptr wparam = 0);
// Clears the desktop icon selection (blank-area click).
bool clearSelection();

} // namespace DesktopIcons
