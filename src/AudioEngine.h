#pragma once
#include <QVector>
#include <atomic>
#include <mutex>
#include <thread>

// WASAPI loopback capture on a single worker thread + a small iterative FFT.
// Publishes 52 band levels (0..1) computed exactly like the Python build
// (runtime.py): FFT magnitudes sampled at geomspace bin indices, log1p
// compression, normalization by the current maximum, smoothing 0.72/0.28.
// All 25 visualizer styles were designed against that value distribution.
// On non-Windows platforms it stays inactive (all zeros) so the UI can be
// smoke-tested, unless LDS_FAKE_AUDIO is set (port of the Python fake mode).
class AudioEngine {
public:
    static constexpr int kBands = 52;
    ~AudioEngine();
    void start();
    void stop();
    bool active() const { return active_.load(); }
    float level() const { return level_.load(); }
    QVector<float> spectrum() const;
private:
    void run();
    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> active_{false};
    std::atomic<float> level_{0.0f};
    mutable std::mutex mutex_;
    QVector<float> bands_ = QVector<float>(kBands, 0.0f);
};
