#include "Platform.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <algorithm>
#include <vector>

static bool g_debugLogEnabled = false; // off by default (v0.13.3)
void setDebugLogEnabled(bool on) { g_debugLogEnabled = on; }
bool debugLogEnabled() { return g_debugLogEnabled; }

void debugLog(const QString &msg) {
    if (!g_debugLogEnabled) return;
    const QString path = QDir::homePath() + QStringLiteral("/LiteDesktopStudio_debug.log");
    // One-generation rotation: at ~5 MB the current file becomes
    // LiteDesktopStudio_debug.log.old and a fresh file starts, so
    // even an opted-in log can never reach GB territory.
    const QFileInfo fi(path);
    if (fi.exists() && fi.size() > 5 * 1024 * 1024) {
        const QString old = path + QStringLiteral(".old");
        QFile::remove(old);
        QFile::rename(path, old);
    }
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream ts(&f);
        ts << QDateTime::currentDateTime().toString(Qt::ISODate) << ' ' << msg << '\n';
        ts.flush();
    }
}

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

static IAudioEndpointVolume *endpointVolume() {
    static IAudioEndpointVolume *vol = nullptr;
    static bool tried = false;
    if (tried) return vol;
    tried = true;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void **)&enumerator)))
        return nullptr;
    IMMDevice *device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&vol);
        device->Release();
    }
    enumerator->Release();
    return vol;
}

int Platform::getVolumePercent() {
    auto *v = endpointVolume();
    if (!v) return -1;
    float level = 0.0f;
    if (FAILED(v->GetMasterVolumeLevelScalar(&level))) return -1;
    return int(level * 100.0f + 0.5f);
}
bool Platform::setVolumePercent(int percent) {
    auto *v = endpointVolume();
    if (!v) return false;
    const float s = std::max(0.0f, std::min(1.0f, percent / 100.0f));
    return SUCCEEDED(v->SetMasterVolumeLevelScalar(s, nullptr));
}
bool Platform::getMute() {
    auto *v = endpointVolume();
    if (!v) return false;
    BOOL m = FALSE;
    return SUCCEEDED(v->GetMute(&m)) && m;
}
bool Platform::setMute(bool mute) {
    auto *v = endpointVolume();
    if (!v) return false;
    return SUCCEEDED(v->SetMute(mute ? TRUE : FALSE, nullptr));
}
static bool pressMediaKey(WORD vk) {
    // SendInput with the extended-key flag: media keys are extended keys,
    // and some players/shell hooks ignore keybd_event without it.
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    return SendInput(2, inputs, sizeof(INPUT)) == 2;
}
bool Platform::mediaPlayPause() { return pressMediaKey(VK_MEDIA_PLAY_PAUSE); }
bool Platform::mediaNext() { return pressMediaKey(VK_MEDIA_NEXT_TRACK); }
bool Platform::mediaPrevious() { return pressMediaKey(VK_MEDIA_PREV_TRACK); }
bool Platform::mediaStop() { return pressMediaKey(VK_MEDIA_STOP); }

// ---- DesktopHost: WorkerW embedding (v0.8.2, hardened v0.8.3) ----
// The effects window becomes a child of a desktop WorkerW, so Explorer's
// icons (in the SHELLDLL_DefView, above the WorkerW layer) and all normal
// windows stay on top. Which WorkerW is the right host varies (Win10 vs
// Win11, multi-monitor), and picking a hidden one makes the effects
// vanish — so attach() tries every candidate and VERIFIES: the parent
// must stick and the host must be visible. The Python build's candidate
// enumeration exists for the same reason.
namespace {
struct WorkerWCandidate { HWND hwnd; bool visible; bool hasDefView; long area; };
BOOL CALLBACK collectWorkerW(HWND hwnd, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"WorkerW") != 0) return TRUE;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    auto *list = reinterpret_cast<std::vector<WorkerWCandidate> *>(lp);
    list->push_back({hwnd, IsWindowVisible(hwnd) != FALSE,
                     FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr,
                     long(rc.right - rc.left) * long(rc.bottom - rc.top)});
    return TRUE;
}
} // namespace

bool DesktopHost::attach(quintptr hwnd_) {
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    if (!hwnd) return false;
    debugLog(QStringLiteral("host: attach begin"));
    if (HWND progman = FindWindowW(L"Progman", nullptr)) {
        debugLog(QStringLiteral("host: sending 0x052C to Progman"));
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
        debugLog(QStringLiteral("host: 0x052C done"));
    }
    std::vector<WorkerWCandidate> cands;
    EnumWindows(collectWorkerW, reinterpret_cast<LPARAM>(&cands));
    for (const WorkerWCandidate &c : cands)
        debugLog(QStringLiteral("host: candidate hwnd=%1 visible=%2 defview=%3 area=%4")
                     .arg(reinterpret_cast<quintptr>(c.hwnd), 0, 16)
                     .arg(c.visible).arg(c.hasDefView).arg(c.area));
    // Prefer: no DefView inside (that one hosts the icons themselves),
    // visible, large. Progman is the last resort (Win11 variant).
    std::stable_sort(cands.begin(), cands.end(), [](const WorkerWCandidate &a, const WorkerWCandidate &b) {
        if (a.hasDefView != b.hasDefView) return !a.hasDefView;
        if (a.visible != b.visible) return a.visible;
        return a.area > b.area;
    });
    if (HWND progman = FindWindowW(L"Progman", nullptr))
        if (FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr))
            cands.push_back({progman, IsWindowVisible(progman) != FALSE, true, 0});
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (const WorkerWCandidate &cand : cands) {
        HWND host = cand.hwnd;
        debugLog(QStringLiteral("host: trying hwnd=%1").arg(reinterpret_cast<quintptr>(host), 0, 16));
        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, host);
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style = (style | WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN) & ~WS_POPUP;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
        // Child coordinates are relative to the host's client area; the
        // host covers the screen at the virtual-screen origin.
        SetWindowPos(hwnd, HWND_TOP, vx, vy, vw, vh,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowWindow(hwnd, SW_SHOW);
        if (GetParent(hwnd) == host && IsWindowVisible(host)) {
            debugLog(QStringLiteral("host: attached to hwnd=%1").arg(reinterpret_cast<quintptr>(host), 0, 16));
            return true;
        }
        // Wrong layer (hidden or rejected): undo and try the next one.
        debugLog(QStringLiteral("host: candidate rejected (parent=%1 hostVisible=%2)")
                     .arg(reinterpret_cast<quintptr>(GetParent(hwnd)), 0, 16).arg(IsWindowVisible(host) != FALSE));
        ShowWindow(hwnd, SW_HIDE);
        SetParent(hwnd, nullptr);
    }
    debugLog(QStringLiteral("host: attach failed, staying a normal window"));
    ShowWindow(hwnd, SW_SHOW);
    return false;
}

quintptr DesktopHost::parentOf(quintptr hwnd_) {
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    return hwnd ? reinterpret_cast<quintptr>(GetParent(hwnd)) : 0;
}

bool DesktopHost::detach(quintptr hwnd_) {
    HWND hwnd = reinterpret_cast<HWND>(hwnd_);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_HIDE);
    SetParent(hwnd, nullptr);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style = (style & ~WS_CHILD & ~WS_VISIBLE) | WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    return true;
}

#else
#include <algorithm>
int Platform::getVolumePercent() { return -1; }
bool Platform::setVolumePercent(int) { return false; }
bool Platform::getMute() { return false; }
bool Platform::setMute(bool) { return false; }
bool Platform::mediaPlayPause() { return false; }
bool Platform::mediaNext() { return false; }
bool Platform::mediaPrevious() { return false; }
bool Platform::mediaStop() { return false; }
bool DesktopHost::attach(quintptr) { return false; }
bool DesktopHost::detach(quintptr) { return false; }
quintptr DesktopHost::parentOf(quintptr) { return 0; }
#endif
