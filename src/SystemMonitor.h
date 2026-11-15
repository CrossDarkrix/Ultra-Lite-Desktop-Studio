#pragma once
#include <QtGlobal>

// One shared snapshot, sampled once per second by the central tick.
// Windows: Win32 API only (no psutil-like layer).
// Other platforms: /proc based fallback, mainly so the code builds and can
// be smoke-tested outside Windows.
struct SystemSnapshot {
    double cpuPercent = -1.0; // -1 until two samples exist
    double memPercent = 0.0;
    quint64 memUsed = 0, memTotal = 0;
    double diskPercent = 0.0;
    quint64 diskUsed = 0, diskTotal = 0;
    quint64 netDownTotal = 0, netUpTotal = 0; // cumulative bytes
    bool valid = false;
};

class SystemMonitor {
public:
    void sample();
    const SystemSnapshot &snapshot() const { return snap_; }
private:
    SystemSnapshot snap_;
#ifdef Q_OS_WIN
    quint64 prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    bool havePrev_ = false;
    int netDiagCount_ = 0; // first GetIfTable2 samples go to the debug log
#else
    quint64 prevIdle_ = 0, prevTotal_ = 0;
    bool havePrev_ = false;
#endif
};
