#include "SystemMonitor.h"
#include "Platform.h"

#ifdef Q_OS_WIN
// Include order matters: MinGW's netioapi.h only declares MIB_IF_TABLE2 /
// GetIfTable2 / FreeMibTable when _WS2IPDEF_ is defined, which requires
// <winsock2.h> + <ws2tcpip.h> to be included BEFORE <netioapi.h>.
// (With windows.h -> iphlpapi.h -> netioapi.h alone, the declarations are
// silently skipped and the build fails exactly like the v0.1 MSYS2 error.)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>

static quint64 fileTimeToU64(const FILETIME &ft) {
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

void SystemMonitor::sample() {
    SystemSnapshot s;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const quint64 i = fileTimeToU64(idle), k = fileTimeToU64(kernel), u = fileTimeToU64(user);
        if (havePrev_) {
            const quint64 di = i - prevIdle_;
            const quint64 dt = (k - prevKernel_) + (u - prevUser_); // kernel includes idle
            if (dt > 0) s.cpuPercent = 100.0 * double(dt - di) / double(dt);
        }
        prevIdle_ = i; prevKernel_ = k; prevUser_ = u; havePrev_ = true;
    }
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.memTotal = ms.ullTotalPhys;
        s.memUsed = ms.ullTotalPhys - ms.ullAvailPhys;
        s.memPercent = s.memTotal ? 100.0 * double(s.memUsed) / double(s.memTotal) : 0.0;
    }
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wchar_t drive[4] = { winDir[0], L':', L'\\', L'\0' };
    ULARGE_INTEGER freeAvail, total, totalFree;
    if (GetDiskFreeSpaceExW(drive, &freeAvail, &total, &totalFree)) {
        s.diskTotal = total.QuadPart;
        s.diskUsed = total.QuadPart - totalFree.QuadPart;
        s.diskPercent = s.diskTotal ? 100.0 * double(s.diskUsed) / double(s.diskTotal) : 0.0;
    }
    MIB_IF_TABLE2 *table = nullptr;
    const DWORD netRc = GetIfTable2(&table);
    if (netRc == NO_ERROR && table) {
        quint64 down = 0, up = 0;
        // v0.9.9: the OperStatus==Up filter was dropped. On the user's
        // machine the widget sat at "waiting for network data..." —
        // every interface was being filtered out (some drivers report
        // Dormant/LowerLayerDown while passing traffic). A down
        // interface contributes a constant, so it cannot fake a rate;
        // the widget guards against counter resets. Loopback stays
        // excluded, like the /proc path.
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2 &r = table->Table[i];
            if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            down += r.InOctets; up += r.OutOctets;
        }
        if (netDiagCount_ < 3) { // first samples: leave evidence in the log
            ++netDiagCount_;
            QString rows;
            for (ULONG i = 0; i < table->NumEntries && i < 12; ++i) {
                const MIB_IF_ROW2 &r = table->Table[i];
                rows += QStringLiteral(" [type=%1 st=%2 in=%3 out=%4]")
                            .arg(r.Type).arg(int(r.OperStatus))
                            .arg(r.InOctets).arg(r.OutOctets);
            }
            debugLog(QStringLiteral("net: entries=%1 sumDown=%2 sumUp=%3%4")
                         .arg(table->NumEntries).arg(down).arg(up).arg(rows));
        }
        FreeMibTable(table);
        s.netDownTotal = down; s.netUpTotal = up;
    } else if (netDiagCount_ < 3) {
        ++netDiagCount_;
        debugLog(QStringLiteral("net: GetIfTable2 failed rc=%1").arg(netRc));
    }
    s.valid = true;
    snap_ = s;
}

#else
#include <QFile>
#include <QTextStream>
#include <sys/statvfs.h>

void SystemMonitor::sample() {
    SystemSnapshot s;
    // CPU from /proc/stat
    QFile f(QStringLiteral("/proc/stat"));
    if (f.open(QIODevice::ReadOnly)) {
        const QString line = QString::fromUtf8(f.readLine());
        const auto parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.size() > 4 && parts[0] == QLatin1String("cpu")) {
            quint64 total = 0, idle = 0;
            for (int i = 1; i < parts.size(); ++i) {
                const quint64 v = parts[i].toULongLong();
                total += v;
                if (i == 4 || i == 5) idle += v; // idle + iowait
            }
            if (havePrev_ && total > prevTotal_) {
                const quint64 dt = total - prevTotal_, di = idle - prevIdle_;
                s.cpuPercent = 100.0 * double(dt - di) / double(dt);
            }
            prevIdle_ = idle; prevTotal_ = total; havePrev_ = true;
        }
    }
    // Memory from /proc/meminfo
    QFile mf(QStringLiteral("/proc/meminfo"));
    if (mf.open(QIODevice::ReadOnly)) {
        quint64 totalKb = 0, availKb = 0;
        QTextStream ts(&mf);
        QString l;
        while (ts.readLineInto(&l)) {
            if (l.startsWith(QLatin1String("MemTotal:"))) totalKb = l.split(QLatin1Char(' '), Qt::SkipEmptyParts).value(1).toULongLong();
            if (l.startsWith(QLatin1String("MemAvailable:"))) availKb = l.split(QLatin1Char(' '), Qt::SkipEmptyParts).value(1).toULongLong();
        }
        s.memTotal = totalKb * 1024; s.memUsed = (totalKb - availKb) * 1024;
        s.memPercent = totalKb ? 100.0 * double(totalKb - availKb) / double(totalKb) : 0.0;
    }
    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0) {
        s.diskTotal = quint64(vfs.f_blocks) * vfs.f_frsize;
        const quint64 freeB = quint64(vfs.f_bfree) * vfs.f_frsize;
        s.diskUsed = s.diskTotal - freeB;
        s.diskPercent = s.diskTotal ? 100.0 * double(s.diskUsed) / double(s.diskTotal) : 0.0;
    }
    // Network from /proc/net/dev (excluding lo)
    QFile nf(QStringLiteral("/proc/net/dev"));
    if (nf.open(QIODevice::ReadOnly)) {
        quint64 down = 0, up = 0;
        QTextStream ts(&nf);
        QString l; ts.readLineInto(&l); ts.readLineInto(&l);
        while (ts.readLineInto(&l)) {
            const int colon = l.indexOf(QLatin1Char(':'));
            if (colon < 0) continue;
            if (l.left(colon).trimmed() == QLatin1String("lo")) continue;
            const auto p = l.mid(colon + 1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (p.size() > 8) { down += p[0].toULongLong(); up += p[8].toULongLong(); }
        }
        s.netDownTotal = down; s.netUpTotal = up;
    }
    s.valid = true;
    snap_ = s;
}
#endif
