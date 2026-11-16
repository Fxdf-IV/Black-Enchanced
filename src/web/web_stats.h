#pragma once

#include <atomic>
#include <cstdint>

namespace web_stats {

inline std::atomic<bool> g_gpu_ready{false};
inline std::atomic<uint64_t> g_presents{0};
inline std::atomic<uint64_t> g_draws{0};
inline std::atomic<uint64_t> g_clears{0};
inline std::atomic<uint64_t> g_copies{0};
inline std::atomic<uint64_t> g_shaders_compiled{0};
inline std::atomic<uint32_t> g_last_sync_va{0};
inline std::atomic<int32_t> g_boot_state{0};  // 0=idle, 1=loading, 2=running, -1=error
inline std::atomic<uint64_t> g_audio_frames_pushed{0};

}  // namespace web_stats
