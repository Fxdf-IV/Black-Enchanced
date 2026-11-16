#include "audio/audio.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/audio_webaudio.h"
#include "core/log.h"
#include "web/web_stats.h"

namespace audio {

namespace {

std::atomic<uint32_t> g_mode{kOutputStereo};
std::atomic<bool> g_started{false};
std::mutex g_start_mu;
std::function<void(float*, uint32_t)> g_render;
audio_web::StereoRingBuffer g_ring;

void* AudioProducerMain(void*) {
    using Clock = std::chrono::steady_clock;
    constexpr uint32_t kMaxChunk = 512;
    std::vector<float> mix6(size_t(kMaxChunk) * kChannels, 0.0f);
    std::vector<float> stereo(size_t(kMaxChunk) * 2u, 0.0f);

    auto next_tick = Clock::now();
    const auto slice = std::chrono::microseconds(10666);  // ~512 frames @ 48kHz

    for (;;) {
        auto now = Clock::now();
        if (now < next_tick) {
            std::this_thread::sleep_for(next_tick - now);
            now = Clock::now();
        }
        // Prevent spiral of death if browser tab is backgrounded
        if (now - next_tick > std::chrono::milliseconds(100)) {
            next_tick = now;
        }
        next_tick += slice;

        if (g_render) {
            std::fill(mix6.begin(), mix6.end(), 0.0f);
            g_render(mix6.data(), kMaxChunk);
            audio_web::Downmix51ToStereo(mix6.data(), stereo.data(), kMaxChunk);
            if (IsMono()) {
                for (uint32_t i = 0; i < kMaxChunk; ++i) {
                    const float m = std::clamp(0.5f * (stereo[i * 2u] + stereo[i * 2u + 1u]), -1.0f, 1.0f);
                    stereo[i * 2u] = m;
                    stereo[i * 2u + 1u] = m;
                }
            }
            const uint32_t pushed = g_ring.push(stereo.data(), kMaxChunk);
            if (pushed > 0) {
                web_stats::g_audio_frames_pushed.fetch_add(pushed, std::memory_order_relaxed);
            }
        }
    }
    return nullptr;
}

}  // namespace

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    std::lock_guard<std::mutex> lock(g_start_mu);
    g_render = std::move(render);
    if (g_started.exchange(true)) return;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 2u << 20);
    pthread_t tid{};
    const int rc = pthread_create(&tid, &attr, &AudioProducerMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("audio_webaudio: pthread_create failed (%d)", rc);
        g_started.store(false);
    } else {
        LOGI("audio_webaudio: 48kHz stereo ring producer started");
    }
}

bool IsSurround() { return false; }

bool IsMono() { return g_mode.load(std::memory_order_relaxed) == kOutputMono; }

void SetOutputMode(uint32_t mode) { g_mode.store(mode, std::memory_order_relaxed); }

}  // namespace audio

extern "C" uint32_t BlackWebDrainAudio(float* dst_interleaved_stereo, uint32_t max_frames) {
    if (!dst_interleaved_stereo || max_frames == 0) return 0;
    return audio::g_ring.pop(dst_interleaved_stereo, max_frames);
}
