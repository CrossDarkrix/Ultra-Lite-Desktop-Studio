#include "AudioEngine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <vector>

QVector<float> AudioEngine::spectrum() const {
    std::lock_guard<std::mutex> g(mutex_);
    return bands_;
}
void AudioEngine::start() {
    if (thread_.joinable()) return;
    stopFlag_ = false;
    thread_ = std::thread([this] { run(); });
}
void AudioEngine::stop() {
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    active_ = false;
}
AudioEngine::~AudioEngine() { stop(); }

// Iterative radix-2 FFT (size must be a power of two).
static void fft(std::vector<std::complex<float>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * float(M_PI) / float(len);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

#ifdef Q_OS_WIN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>

void AudioEngine::run() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    DWORD taskIndex = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioCaptureClient *capture = nullptr;
    WAVEFORMATEX *format = nullptr;
    auto cleanup = [&] {
        if (client) client->Stop();
        if (format) CoTaskMemFree(format);
        if (capture) capture->Release();
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (avrt) AvRevertMmThreadCharacteristics(avrt);
        CoUninitialize();
    };
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void **)&enumerator))) { cleanup(); return; }
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) { cleanup(); return; }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client))) { cleanup(); return; }
    if (FAILED(client->GetMixFormat(&format))) { cleanup(); return; }
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  10000000, 0, format, nullptr))) { cleanup(); return; }
    if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), (void **)&capture))) { cleanup(); return; }
    if (FAILED(client->Start())) { cleanup(); return; }
    active_ = true;

    constexpr int kFftSize = 2048;
    std::vector<std::complex<float>> buf(kFftSize);
    std::vector<float> window(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window[i] = 0.5f * (1.0f - std::cos(2.0f * float(M_PI) * i / (kFftSize - 1)));
    std::vector<float> mono;
    mono.reserve(kFftSize * 2);
    const int channels = std::max(1, (int)format->nChannels);
    const bool isFloat = (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->wBitsPerSample == 32);
    const int bits = format->wBitsPerSample;

    while (!stopFlag_) {
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        if (packet == 0) { Sleep(5); continue; }
        while (packet > 0 && !stopFlag_) {
            BYTE *data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                float v = 0.0f;
                if (!silent) {
                    if (isFloat) {
                        const float *p = reinterpret_cast<const float *>(data) + i * channels;
                        for (int c = 0; c < channels; ++c) v += p[c];
                    } else if (bits == 16) {
                        const short *p = reinterpret_cast<const short *>(data) + i * channels;
                        for (int c = 0; c < channels; ++c) v += p[c] / 32768.0f;
                    } else if (bits == 32) {
                        const int *p = reinterpret_cast<const int *>(data) + i * channels;
                        for (int c = 0; c < channels; ++c) v += p[c] / 2147483648.0f;
                    }
                    v /= channels;
                }
                mono.push_back(v);
            }
            capture->ReleaseBuffer(frames);
            if (FAILED(capture->GetNextPacketSize(&packet))) packet = 0;
        }
        if ((int)mono.size() < kFftSize) continue;
        for (int i = 0; i < kFftSize; ++i)
            buf[i] = { mono[i] * window[i], 0.0f };
        mono.erase(mono.begin(), mono.begin() + kFftSize / 2); // 50% overlap
        fft(buf);
        // Python pipeline (runtime.py): magnitudes of the first half of the
        // rfft (kFftSize/4 bins), sampled at geomspace bin indices, log1p,
        // normalized by the current max, smoothed 0.72 old + 0.28 new.
        constexpr int kBins = kFftSize / 4;
        QVector<float> raw(kBands, 0.0f);
        float maxRaw = 0.0f;
        for (int b = 0; b < kBands; ++b) {
            const int i0 = std::max(1, int(std::pow(double(kBins - 1), double(b) / (kBands - 1))));
            const int i1 = (b + 1 < kBands)
                ? std::max(i0 + 1, int(std::pow(double(kBins - 1), double(b + 1) / (kBands - 1))))
                : kBins;
            float mag = 0.0f;
            for (int i = i0; i < std::min(i1, kBins); ++i) mag = std::max(mag, std::abs(buf[i]));
            raw[b] = std::log1p(mag);
            maxRaw = std::max(maxRaw, raw[b]);
        }
        float peak = 0.0f;
        {
            std::lock_guard<std::mutex> g(mutex_);
            for (int b = 0; b < kBands; ++b) {
                const float v = maxRaw > 0.0f ? raw[b] / maxRaw : 0.0f;
                bands_[b] = bands_[b] * 0.72f + v * 0.28f;
                peak = std::max(peak, bands_[b]);
            }
        }
        level_ = peak;
    }
    cleanup();
}
#else
#include <cstdlib>
void AudioEngine::run() {
    // No loopback capture outside Windows: stay silent, unless the dev
    // fake mode is requested (port of runtime.py _run_fake, used by the
    // Linux smoke tests to exercise the visualizer painters).
    if (!std::getenv("LDS_FAKE_AUDIO")) {
        while (!stopFlag_) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return;
    }
    active_ = true;
    double t = 0.0;
    unsigned seed = 12345;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
    while (!stopFlag_) {
        {
            std::lock_guard<std::mutex> g(mutex_);
            float peak = 0.0f;
            for (int i = 0; i < kBands; ++i) {
                const float v = std::sin(float(t * 2.0 + i * 0.35)) * 0.4f +
                                std::sin(float(t * 4.7 + i * 0.12)) * 0.25f + rnd() * 0.25f;
                const float target = std::max(0.03f, std::min(1.0f, std::abs(v)));
                bands_[i] = bands_[i] * 0.75f + target * 0.25f;
                peak = std::max(peak, bands_[i]);
            }
            level_ = peak;
        }
        t += 0.08;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
#endif
