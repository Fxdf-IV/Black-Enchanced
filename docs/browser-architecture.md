# Arquitetura WebAssembly / Navegador — Black Enhanced Online

Este documento detalha o caminho técnico e jurídico para levar o **Black Enhanced Online** ao navegador sem violar direitos autorais e aproveitando as costuras reais do `XboxXBE-Recomp` (`sdk/`).

---

## 1. Descobertas na Auditoria do `sdk/runtime`

A inspeção direta do código-fonte de `TGP482/XboxXBERecomp` (`cc9edda3a821a4204dc28783f429b88a27ade4f9`) revelou três fatos arquiteturais decisivos:

1. **Costura limpa de GPU (`sdk/runtime/gpu/backend.h`):**
   O `d3d8.cpp` do SDK **não** chama Direct3D 12 diretamente. Ele conversa com a interface abstrata `gpu::Backend` (apenas 72 linhas), que possui:
   * `Init(width, height)`
   * `Clear(state, count, rects, flags, color, z, stencil)`
   * `Draw(state, prim, start, count, indices)`
   * `Copy(src, dst, count, src_rects_va, dst_points_va, src_kx, src_ky)`
   * `Present(front)`
   * `Sync(data_va, size)` / `SyncLate(data_va)`
   * `Flush()` / `SetGamma(ramp)`
   Para rodar no navegador, **não precisamos traduzir Direct3D 12 para WebGPU**. Basta implementar `gpu/webgpu.cpp` herdando de `gpu::Backend` (exatamente no nível D3D8 -> WebGPU validado pelo `cricket07-browser`).
2. **Assinatura uniforme de funções recompiladas (`sdk/runtime/cpu/x86_context.h`):**
   Toda função gerada pelo recompilador tem a assinatura:
   ```cpp
   using X86Func = void(X86Context& __restrict ctx, uint8_t* base);
   ```
   Ou seja, todos os acessos à memória convidada já são relativos ao ponteiro `uint8_t* base` (`g_base`), e não ponteiros absolutos do processo de 64 bits do host.
3. **Mapeamento de memória (`sdk/runtime/kernel/memory.cpp`):**
   No Windows x64, `memory.cpp` reserva uma janela virtual de 4 GB (`1ull << 32`) via `VirtualAlloc` e mapeia a RAM física de 64 MB do Xbox em duas janelas espelhadas (`0x80000000` e `0xF0000000` para write-combine) usando `MapViewOfFileEx`.

---

## 2. Os 4 Bloqueadores do Port para WebAssembly e Como Resolvê-los

| # | Bloqueador no `sdk/runtime` | Causa no Navegador | Solução Técnica |
| :--- | :--- | :--- | :--- |
| **B1** | **Espaço convidado de 4 GB e janelas `0x80000000` / `0xF0000000`** | Em `wasm32`, a memória linear inteira tem no máximo 4 GB e é compartilhada com o heap do próprio runtime C++; não há `MapViewOfFileEx` para criar espelhos virtuais. | **Opção A (`wasm64` / Memory64):** Suportado nativamente sem flags no Chrome 133+ e Firefox 143+; permite alocar uma região contígua de 4 GB (`g_base`) dentro do espaço de 64 bits e sincronizar a janela `0xF0000000` (64 MB) no `gpu::Backend::Sync`/`Flush` ou mascarar o bit alto no acesso do recompilador.<br>**Opção B (`wasm32` com máscara de endereço no recompilador):** Como o Xbox tem apenas 64 MB de RAM física e o executável fica abaixo de `0x02000000`, um passe no gerador C++ do `tools/recomp` pode dobrar endereços `>= 0x80000000` para uma janela compacta de 256 MB. |
| **B2** | **Threads do Kernel Xbox (`runtime/kernel/threads.cpp`)** | Depende de primitivas Win32 e assume preempção do SO. | Compilar com Emscripten `-pthread` sobre `SharedArrayBuffer` (requer cabeçalhos HTTP `Cross-Origin-Opener-Policy: same-origin` e `Cross-Origin-Embedder-Policy: require-corp`), mantendo a thread de renderização separada via gravação de comandos (padrão `cricket07-browser`). |
| **B3** | **Renderizador (`runtime/gpu/d3d12.cpp`)** | Exclusivo de Windows (DXGI / D3D12). | Implementar `runtime/gpu/webgpu.cpp` implementando `gpu::Backend` sobre `<webgpu/webgpu.h>` do Emscripten, traduzindo os shaders gerados em `gpu/shaders.cpp` para WGSL. |
| **B4** | **Áudio (`runtime/audio/xaudio2.cpp`)** | Exclusivo de Windows (`XAudio2`). | Implementar `runtime/audio/webaudio.cpp` cumprindo `runtime/audio/audio.h` via `AudioWorklet` / SDL3 Audio a 48 kHz. |

---

## 3. Modelo de Rede no Navegador: Snapshot Autoritativo vs. Lockstep

A análise comparativa (validada na revisão técnica com o Hermes) confirma a escolha do plano original:

* **Não usar Lockstep (`cricket07-browser`) em BLACK:** O `cricket07` é single-thread e determinístico. BLACK é multithread (threads de kernel, áudio e streaming de disco), usa física e balística em ponto flutuante x87/SSE e para partidas de 4 jogadores o lockstep trava todos na latência do par mais lento.
* **Manter Host Autoritativo com Snapshots (`libs/netcore`):**
  * O código em `libs/netcore` é C++20 puro, sem chamadas Win32 nem sockets diretos: ele compila inalterado tanto para Windows nativo quanto para `wasm32`/`wasm64` via Emscripten.
  * No Windows nativo, `ITransport` usa UDP/ENet (ou WebRTC nativo via `libdatachannel`).
  * No navegador, `ITransport` usa dois `RTCDataChannel` por par:
    * `Channel::Reliable`: `ordered: true`
    * `Channel::Unreliable`: `ordered: false, maxRetransmits: 0`
  * Isso permite **partidas híbridas na Fase Web Inicial**: um host nativo no Windows hospedando a sala enquanto até 3 clientes entram direto pelo navegador via WebRTC, antes mesmo de otimizar o host dentro do navegador.

---

## 4. Distribuição Legal no Navegador (Zero Assets / Zero Código da EA Hospedado)

1. **Arquivos do jogo (ISO / pastas):** Nunca são hospedados em servidor. No primeiro acesso à página web, o jogador seleciona sua própria pasta/ISO local via **File System Access API**; os arquivos são copiados uma única vez para o **OPFS (Origin Private File System)** do navegador e nunca saem da máquina do usuário (mesmo modelo do `cricket07-browser`).
2. **Código recompilado do XBE:**
   * **Modo auto-hospedado / geração local (recomendado):** O repositório distribui apenas o runtime open-source (MIT), o `libs/netcore` e o script de build local. O usuário gera o módulo `.wasm` localmente a partir do seu próprio `default.xbe` (assim como o `BLACK-XBERecomp` já gera `BLACK.dll` localmente no primeiro boot) ou o carrega do disco para o OPFS ao lado dos assets.
   * Dessa forma, **nenhum byte derivado do `default.xbe` nem nenhum asset da EA/Criterion é distribuído no repositório público ou em CDN pública**.
