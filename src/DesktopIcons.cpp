#include "DesktopIcons.h"
#include "Platform.h"
#include <QDateTime>
#include <QHash>

#ifdef Q_OS_WIN
#include <windows.h>
#include <commctrl.h>
#include <QFile>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace {
QVector<DesktopIcons::IconItem> g_cache;
qint64 g_cacheAtMs = 0;
QSize g_cacheSize;
QPoint g_cacheOrigin;
QHash<int, QPixmap> g_imageCache;

HWND findDesktopListView() {
    // Progman -> SHELLDLL_DefView -> SysListView32 ("FolderView"); the
    // DefView can also live inside a WorkerW on some shells.
    if (HWND progman = FindWindowW(L"Progman", nullptr)) {
        if (HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
            if (HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr))
                return lv;
    }
    HWND found = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (HWND def = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
            if (HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr)) {
                *reinterpret_cast<HWND *>(lp) = lv;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// RAII helper for memory allocated inside the Explorer process.
struct RemoteMem {
    HANDLE proc = nullptr;
    void *ptr = nullptr;
    RemoteMem(HANDLE p, SIZE_T size) : proc(p) {
        ptr = VirtualAllocEx(p, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    ~RemoteMem() {
        if (ptr) VirtualFreeEx(proc, ptr, 0, MEM_RELEASE);
    }
    bool ok() const { return ptr != nullptr; }
};

HANDLE openExplorer(HWND lv, DWORD *pidOut) {
    DWORD pid = 0;
    GetWindowThreadProcessId(lv, &pid);
    if (!pid) return nullptr;
    if (pidOut) *pidOut = pid;
    return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                           PROCESS_VM_READ | PROCESS_VM_WRITE,
                       FALSE, pid);
}
} // namespace
#endif

namespace DesktopIcons {

void invalidateCache() {
#ifdef Q_OS_WIN
    g_cacheAtMs = 0;
#endif
}

QVector<IconItem> readItems(const QPoint &localOrigin, const QSize &canvasSize) {
#ifdef Q_OS_WIN
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!g_cache.isEmpty() && now - g_cacheAtMs < 1500 && g_cacheSize == canvasSize &&
        g_cacheOrigin == localOrigin)
        return g_cache;

    QVector<IconItem> items;
    HWND lv = findDesktopListView();
    if (!lv) return items;
    const int count = int(SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0));
    if (count <= 0 || count > 4096) return items;
    HANDLE proc = openExplorer(lv, nullptr);
    if (!proc) return items;

    RemoteMem remotePoint(proc, sizeof(POINT));
    RemoteMem remoteRect(proc, sizeof(RECT));
    const int textChars = 260;
    RemoteMem remoteText(proc, SIZE_T(textChars) * sizeof(wchar_t));
    RemoteMem remoteItem(proc, sizeof(LVITEMW));
    if (remotePoint.ok() && remoteRect.ok() && remoteText.ok() && remoteItem.ok()) {
        const int maxCount = qMin(count, 256);
        for (int i = 0; i < maxCount; ++i) {
            IconItem item;
            item.index = i;
            POINT pt{0, 0};
            if (SendMessageW(lv, LVM_GETITEMPOSITION, WPARAM(i),
                             reinterpret_cast<LPARAM>(remotePoint.ptr))) {
                SIZE_T read = 0;
                ReadProcessMemory(proc, remotePoint.ptr, &pt, sizeof(pt), &read);
            }
            item.pos = QPoint(int(pt.x), int(pt.y));
            RECT rc{LONG(LVIR_BOUNDS), 0, 0, 0};
            bool haveRect = false;
            {
                SIZE_T written = 0;
                WriteProcessMemory(proc, remoteRect.ptr, &rc, sizeof(rc), &written);
                if (SendMessageW(lv, LVM_GETITEMRECT, WPARAM(i),
                                 reinterpret_cast<LPARAM>(remoteRect.ptr))) {
                    SIZE_T read = 0;
                    if (ReadProcessMemory(proc, remoteRect.ptr, &rc, sizeof(rc), &read))
                        haveRect = rc.right > rc.left && rc.bottom > rc.top;
                }
            }
            if (!haveRect) {
                rc = RECT{pt.x, pt.y, pt.x + 96, pt.y + 76};
            }
            // Label + shell image index via a remote LVITEMW.
            {
                LVITEMW li{};
                li.mask = LVIF_TEXT | LVIF_IMAGE;
                li.iItem = i;
                li.iSubItem = 0;
                li.pszText = reinterpret_cast<LPWSTR>(remoteText.ptr);
                li.cchTextMax = textChars;
                li.iImage = -1;
                SIZE_T written = 0;
                WriteProcessMemory(proc, remoteItem.ptr, &li, sizeof(li), &written);
                SendMessageW(lv, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(remoteItem.ptr));
                LVITEMW out{};
                SIZE_T read = 0;
                if (ReadProcessMemory(proc, remoteItem.ptr, &out, sizeof(out), &read))
                    item.imageIndex = out.iImage;
                wchar_t buf[260] = {};
                if (ReadProcessMemory(proc, remoteText.ptr, buf, sizeof(buf), &read))
                    item.text = QString::fromWCharArray(buf);
                if (item.text.isEmpty()) { // some listviews prefer GETITEMTEXT
                    SendMessageW(lv, LVM_GETITEMTEXTW, WPARAM(i),
                                 reinterpret_cast<LPARAM>(remoteItem.ptr));
                    if (ReadProcessMemory(proc, remoteText.ptr, buf, sizeof(buf), &read))
                        item.text = QString::fromWCharArray(buf);
                }
            }
            const LRESULT state = SendMessageW(lv, LVM_GETITEMSTATE, WPARAM(i),
                                               LVIS_SELECTED | LVIS_FOCUSED);
            item.selected = (state & LVIS_SELECTED) != 0;
            item.focused = (state & LVIS_FOCUSED) != 0;
            // ListView client coords -> screen -> overlay-local.
            POINT p1{rc.left, rc.top}, p2{rc.right, rc.bottom};
            ClientToScreen(lv, &p1);
            ClientToScreen(lv, &p2);
            item.bounds = QRect(QPoint(int(p1.x) - localOrigin.x(), int(p1.y) - localOrigin.y()),
                                QPoint(int(p2.x) - localOrigin.x(), int(p2.y) - localOrigin.y()))
                              .normalized();
            const QPoint c = item.bounds.center();
            if (c.x() >= -256 && c.x() <= canvasSize.width() + 256 && c.y() >= -256 &&
                c.y() <= canvasSize.height() + 256)
                items.append(item);
        }
    }
    CloseHandle(proc);
    g_cache = items;
    g_cacheAtMs = now;
    g_cacheSize = canvasSize;
    g_cacheOrigin = localOrigin;
    { // one diagnostic line per cache fill (v0.9.1)
        QStringList idx;
        for (const IconItem &it : items) idx.append(QString::number(it.imageIndex));
        debugLog(QStringLiteral("icons: read %1 items (lv count %2), imageIndex: %3")
                     .arg(items.size()).arg(count).arg(idx.join(QStringLiteral(","))));
    }
    return items;
#else
    Q_UNUSED(localOrigin);
    Q_UNUSED(canvasSize);
    return {};
#endif
}

QPixmap iconImageFor(const IconItem &item) {
#ifdef Q_OS_WIN
    const int key = item.index; // one icon per desktop item
    const auto it = g_imageCache.constFind(key);
    if (it != g_imageCache.constEnd()) return it.value();
    QPixmap result;
    const QString label = item.text.trimmed();
    if (!label.isEmpty()) {
        // Desktop roots: the user's Desktop and the public Desktop.
        QStringList roots;
        wchar_t buf[MAX_PATH] = {};
        if (SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, buf) == S_OK)
            roots.append(QString::fromWCharArray(buf));
        if (SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, buf) == S_OK)
            roots.append(QString::fromWCharArray(buf));
        static const char *exts[] = {"",     ".lnk", ".url", ".exe", ".appref-ms", ".txt",
                                     ".json", ".ini", ".yaml", ".yml", ".pdf",  ".doc",
                                     ".docx", ".xls", ".xlsx", ".ppt", ".pptx", ".png",
                                     ".jpg",  ".jpeg", ".webp", ".gif",  ".zip",  ".7z",
                                     ".rar",  ".py",   ".bat",  ".cmd"};
        QString found;
        for (const QString &root : roots) {
            for (const char *ext : exts) {
                const QString cand = root + QLatin1Char('\\') + label + QLatin1String(ext);
                if (QFile::exists(cand)) { found = cand; break; }
            }
            if (!found.isEmpty()) break;
        }
        if (!found.isEmpty()) {
            SHFILEINFOW sfi{};
            const std::wstring wpath = found.toStdWString();
            if (SHGetFileInfoW(wpath.c_str(), 0, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_LARGEICON) &&
                sfi.hIcon) {
                result = QPixmap::fromImage(QImage::fromHICON(sfi.hIcon));
                DestroyIcon(sfi.hIcon);
            }
        }
        if (result.isNull() &&
            (label == QStringLiteral("ごみ箱") || !label.compare(QStringLiteral("Recycle Bin"), Qt::CaseInsensitive))) {
            // Shell namespace item (no file on disk): resolve the icon
            // through the Recycle Bin's display name, like Python's
            // _lds_extract_recycle_bin_icon_image (v0.9.5).
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SHParseDisplayName(L"::{645FF040-5081-101B-9F08-00AA002F954E}",
                                   nullptr, &pidl, 0, nullptr) == S_OK &&
                pidl) {
                SHFILEINFOW sfi{};
                if (SHGetFileInfoW(reinterpret_cast<const wchar_t *>(pidl), 0, &sfi, sizeof(sfi),
                                   SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON) &&
                    sfi.hIcon) {
                    result = QPixmap::fromImage(QImage::fromHICON(sfi.hIcon));
                    DestroyIcon(sfi.hIcon);
                }
                ILFree(pidl);
            }
        }
        if (result.isNull()) {
            // Generic shell file icon as the last resort (store-app or
            // namespace items with no resolvable file), so the item
            // still shows SOMETHING instead of a hole.
            SHFILEINFOW sfi{};
            if (SHGetFileInfoW(L"file", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) &&
                sfi.hIcon) {
                result = QPixmap::fromImage(QImage::fromHICON(sfi.hIcon));
                DestroyIcon(sfi.hIcon);
            }
        }
        debugLog(QStringLiteral("icons: item %1 '%2' -> %3")
                     .arg(item.index).arg(label)
                     .arg(result.isNull() ? QStringLiteral("NULL")
                                          : QStringLiteral("ok via %1").arg(found)));
    }
    g_imageCache.insert(key, result);
    return result;
#else
    Q_UNUSED(item);
    return QPixmap();
#endif
}

bool postMouse(quint32 message, const QPoint &localPt, const QPoint &globalOrigin, quintptr wparam) {
#ifdef Q_OS_WIN
    HWND lv = findDesktopListView();
    if (!lv) return false;
    POINT pt{globalOrigin.x() + localPt.x(), globalOrigin.y() + localPt.y()};
    ScreenToClient(lv, &pt);
    const LPARAM lp = MAKELPARAM(WORD(pt.x), WORD(pt.y));
    const bool ok = PostMessageW(lv, UINT(message), WPARAM(wparam), lp) != FALSE;
    if (ok && (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
               message == WM_LBUTTONDBLCLK || (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON))))
        invalidateCache();
    return ok;
#else
    Q_UNUSED(message);
    Q_UNUSED(localPt);
    Q_UNUSED(globalOrigin);
    Q_UNUSED(wparam);
    return false;
#endif
}

bool clearSelection() {
#ifdef Q_OS_WIN
    HWND lv = findDesktopListView();
    if (!lv) return false;
    HANDLE proc = openExplorer(lv, nullptr);
    if (!proc) return false;
    RemoteMem remoteItem(proc, sizeof(LVITEMW));
    bool ok = false;
    if (remoteItem.ok()) {
        LVITEMW li{};
        li.state = 0;
        li.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SIZE_T written = 0;
        WriteProcessMemory(proc, remoteItem.ptr, &li, sizeof(li), &written);
        SendMessageW(lv, LVM_SETITEMSTATE, WPARAM(-1),
                     reinterpret_cast<LPARAM>(remoteItem.ptr));
        InvalidateRect(lv, nullptr, TRUE);
        ok = true;
    }
    CloseHandle(proc);
    invalidateCache();
    return ok;
#else
    return false;
#endif
}

} // namespace DesktopIcons
