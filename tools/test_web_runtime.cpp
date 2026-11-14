#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common/audio_webaudio.h"
#include "common/gpu_command_stream.h"
#include "common/gpu_webgpu.h"
#include "common/wasm_memory.h"

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            std::abort();                                                                           \
        }                                                                                           \
    } while (0)

int main() {
    // 1. Testes da arena wasm32 (wasm_mem::FoldGuestVa)
    CHECK(wasm_mem::FoldGuestVa(0x00010000u) == 0x00010000u);
    CHECK(wasm_mem::FoldGuestVa(0x01C063ECu) == 0x01C063ECu);
    // Espelhamento exato entre RAM Física (0x80000000) e Write-Combine (0xF0000000)
    CHECK(wasm_mem::FoldGuestVa(0x80123456u) == 0x08123456u);
    CHECK(wasm_mem::FoldGuestVa(0xF0123456u) == 0x08123456u);
    CHECK(wasm_mem::FoldGuestVa(0x83FFFFFFu) == wasm_mem::FoldGuestVa(0xF3FFFFFFu));
    // Pools de sistema e pilhas dentro dos 256 MB
    CHECK(wasm_mem::FoldGuestVa(0xB0001000u) == 0x0C001000u);
    CHECK(wasm_mem::FoldGuestVa(0xD0010000u) == 0x0E010000u);
    CHECK(wasm_mem::FoldGuestVa(0xFFFFFFFFu) < wasm_mem::kArenaSize);

    // 2. Testes da fila GPU / WebGPU stream
    gpu_stream::CommandRecorder recorder;
    gpu_webgpu::WebGpuStreamBackend backend(recorder);
    gpu::State dummy_state{};
    gpu::Surface front{};
    front.data = 0x80200000u;

    CHECK(backend.Init(1280, 720));
    backend.Clear(dummy_state, 0, nullptr, gpu::kClearTarget | gpu::kClearZ, 0xFF000000u, 1.0f, 0);
    const uint16_t indices[3] = {0, 1, 2};
    backend.Draw(dummy_state, 4, 0, 3, reinterpret_cast<const uint8_t*>(indices));
    backend.Present(front);

    CHECK(recorder.submitted_frames() == 1);
    std::vector<gpu_stream::Command> frame;
    CHECK(recorder.pop_completed_frame(frame));
    CHECK(frame.size() == 4);
    CHECK(frame[0].op == gpu_stream::OpCode::Init && frame[0].a0 == 1280 && frame[0].a1 == 720);
    CHECK(frame[2].op == gpu_stream::OpCode::Draw && frame[2].inline_bytes.size() == 6);
    CHECK(frame[3].op == gpu_stream::OpCode::Present && frame[3].a0 == 0x80200000u);

    // 3. Testes de áudio 5.1 -> estéreo 48 kHz + ring buffer SPSC
    float in6[6 * 4] = {};
    for (int i = 0; i < 4; ++i) {
        in6[i * 6 + 0] = 0.5f;   // FL
        in6[i * 6 + 1] = -0.5f;  // FR
        in6[i * 6 + 2] = 0.2f;   // Center
    }
    float out2[2 * 4] = {};
    audio_web::Downmix51ToStereo(in6, out2, 4);
    CHECK(out2[0] > 0.6f && out2[1] < -0.3f);

    audio_web::StereoRingBuffer ring;
    CHECK(ring.push(out2, 4) == 4);
    CHECK(ring.available() == 4);
    float popped[2 * 4] = {};
    CHECK(ring.pop(popped, 4) == 4);
    CHECK(ring.available() == 0);
    CHECK(popped[0] == out2[0] && popped[1] == out2[1]);

    std::printf("[web_runtime_tests] All wasm_memory, gpu_webgpu and audio_webaudio tests PASSED.\n");
    return 0;
}
