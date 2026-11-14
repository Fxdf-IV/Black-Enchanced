#pragma once
// Ponte de áudio lock-free (48 kHz) compatível com sdk/runtime/audio/audio.h
// para alimentar um AudioWorkletProcessor no navegador via SharedArrayBuffer.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace audio_web {

inline constexpr uint32_t kSampleRate = 48000;
inline constexpr uint32_t kRingFrames = 4096;  // ~85 ms buffer estéreo em potências de 2
inline constexpr float kCenterGain = 0.70710678f;
inline constexpr float kSurroundGain = 0.70710678f;
inline constexpr float kLfeGain = 0.5f;

// Converte saída surround 5.1 do mixer DirectSound (FL, FR, C, LFE, BL, BR) em estéreo (L, R)
inline void Downmix51ToStereo(const float* in6, float* out2, uint32_t frames) noexcept {
    for (uint32_t i = 0; i < frames; ++i) {
        const float fl = in6[i * 6 + 0];
        const float fr = in6[i * 6 + 1];
        const float c = in6[i * 6 + 2];
        const float lfe = in6[i * 6 + 3];
        const float bl = in6[i * 6 + 4];
        const float br = in6[i * 6 + 5];

        const float left = fl + c * kCenterGain + bl * kSurroundGain + lfe * kLfeGain;
        const float right = fr + c * kCenterGain + br * kSurroundGain + lfe * kLfeGain;

        out2[i * 2 + 0] = std::clamp(left, -1.0f, 1.0f);
        out2[i * 2 + 1] = std::clamp(right, -1.0f, 1.0f);
    }
}

class StereoRingBuffer {
public:
    uint32_t push(const float* interleaved_stereo, uint32_t frames) noexcept {
        const uint32_t w = write_idx_.load(std::memory_order_relaxed);
        const uint32_t r = read_idx_.load(std::memory_order_acquire);
        const uint32_t free_frames = kRingFrames - (w - r) - 1u;
        const uint32_t count = std::min(frames, free_frames);

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = (w + i) & (kRingFrames - 1u);
            samples_[slot * 2 + 0] = interleaved_stereo[i * 2 + 0];
            samples_[slot * 2 + 1] = interleaved_stereo[i * 2 + 1];
        }

        write_idx_.store(w + count, std::memory_order_release);
        return count;
    }

    uint32_t pop(float* out_stereo, uint32_t frames) noexcept {
        const uint32_t r = read_idx_.load(std::memory_order_relaxed);
        const uint32_t w = write_idx_.load(std::memory_order_acquire);
        const uint32_t avail = w - r;
        const uint32_t count = std::min(frames, avail);

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t slot = (r + i) & (kRingFrames - 1u);
            out_stereo[i * 2 + 0] = samples_[slot * 2 + 0];
            out_stereo[i * 2 + 1] = samples_[slot * 2 + 1];
        }

        read_idx_.store(r + count, std::memory_order_release);
        return count;
    }

    uint32_t available() const noexcept {
        return write_idx_.load(std::memory_order_acquire) - read_idx_.load(std::memory_order_acquire);
    }

private:
    std::array<float, kRingFrames * 2> samples_{};
    std::atomic<uint32_t> write_idx_{0};
    std::atomic<uint32_t> read_idx_{0};
};

}  // namespace audio_web
