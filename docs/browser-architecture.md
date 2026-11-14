# Trilha de Pesquisa WebAssembly / Navegador (Fase 6) — Black Enhanced Online

> [!IMPORTANT]
> **Estado atual: Trilha de pesquisa (Fase 6 — NÃO implementada nem validada em execução).**
> Conforme o princípio **PC primeiro, navegador por último** ([`PLANO_DE_IMPLEMENTACAO.md`](PLANO_DE_IMPLEMENTACAO.md)), nenhum trabalho de runtime WebAssembly/WebGPU bloqueia ou substitui as **Fases 0 a 3 no Windows nativo (D3D12)**. Os documentos nesta página registram o levantamento real do `sdk/runtime/gpu/backend.h` e as restrições conhecidas para quando a Fase 6 começar.

---

## 1. O que a Inspeção do `sdk/runtime/gpu/backend.h` Realmente Mostra

A interface `gpu::Backend` em [`sdk/runtime/gpu/backend.h`](../sdk/runtime/gpu/backend.h) (72 linhas) isola o `d3d8.cpp` do backend D3D12 através de chamadas de alto nível, **porém inclui operações de sincronização e leitura GPU -> CPU que complicam o WebGPU**:

1. **Comandos de desenho unidirecionais (fáceis de enfileirar):**
   * `Init(width, height)`
   * `Clear(state, count, rects, flags, color, z, stencil)`
   * `Draw(state, prim, start, count, indices)`
   * `Copy(src, dst, count, src_rects_va, dst_points_va, src_kx, src_ky)`
   * `Present(front)` / `Flush()` / `SetGamma(ramp)`
2. **Leituras GPU -> CPU, visibilidade e cercas (ponto crítico no WebGPU):**
   * `Sync(data_va, size)` — a CPU vai ler/alterar memória de recurso: exige que a GPU devolva resultados antes do acesso da CPU.
   * `SyncLate(data_va)` + `DeliverLate(va)` / `RecordLate(va)` — lock tipo `NOOVERWRITE` tolerando atraso de 1–2 quadros.
   * `ClearReport()` / `GetReport(report)` — testes de visibilidade (occlusion queries) que gravam relatório de 16 bytes na memória convidada após os draws concluírem.
   * `Fence()`, `FencePending(id)`, `WaitFence(id)`, `Busy(resource)`, `WaitResource(resource)`, `WaitGpuFrame(n)` — barreiras de conclusão da GPU.
   * **Por que complica no WebGPU:** No WebGPU, leituras de buffer (`buffer.mapAsync`) e conclusão de fila (`onSubmittedWorkDone`) são **assíncronas** por design e não podem bloquear de forma síncrona na main thread do navegador. Uma implementação futura de `gpu/webgpu.cpp` (e a extensão em `src/common/gpu_command_stream.h`) precisará usar worker threads dedicadas com `Atomics.wait` sobre `SharedArrayBuffer`, ring buffers de read-back adiado (`DeliverLate`/`RecordLate`) ou caching assíncrono dos relatórios de visibilidade.

---

## 2. Alvo Primário de Memória WASM (Inconsistência Corrigida)

No Windows x64 (`sdk/runtime/kernel/memory.cpp`), o runtime reserva 4 GB e espelha a RAM de 64 MB do Xbox em `0x80000000` e `0xF0000000` (write-combine) usando `VirtualAlloc` + `MapViewOfFileEx`. No navegador não existe `MapViewOfFileEx`.

* **Desenho primário adotado para pesquisa: `wasm32` com arena convidada compacta de 256 MB.**
  * Em `wasm32`, todo o espaço linear máximo tem 4 GB compartilhado com o heap C++ do runtime e com os stacks de pthreads.
  * Como o Xbox físico possui 64 MB de RAM e o executável fica bem abaixo desse teto, um passe no gerador C++ do `xbe_recomp` dobraria endereços de espelho (`>= 0x80000000`) para dentro de uma arena convidada compacta de **256 MB** sem exigir `wasm64`.
* **Alternativa futura (apenas referência secundária): `wasm64` (Memory64).**
  * Eliminaria a dobragem de 256 MB ao permitir reserva virtual ampla, mas traz overhead de endereçamento de 64 bits em WASM e menor maturidade de tooling. Mantida apenas como opção de reserva, não mais misturada no mesmo desenho de 256 MB.

---

## 3. Outros Bloqueadores do Port para Navegador

| Bloqueador no `sdk/runtime` | Causa no Navegador | Caminho de Pesquisa |
| :--- | :--- | :--- |
| **Threads do Kernel Xbox (`runtime/kernel/threads.cpp`)** | Usa primitivas Win32. | Emscripten `-pthread` sobre `SharedArrayBuffer` (exige HTTP `COOP: same-origin` e `COEP: require-corp`). |
| **Áudio (`runtime/audio/xaudio2.cpp`)** | Exclusivo de Windows (`XAudio2`). | Implementar `runtime/audio/webaudio.cpp` sobre `sdk/runtime/audio/audio.h` usando `AudioWorklet` a 48 kHz. |
| **Conversão dos shaders** | `gpu/shaders.cpp` gera bytecode/HLSL para D3D12. | Adaptar o gerador de shaders do SDK para emitir WGSL compatível com WebGPU. |

---

## 4. Modelo de Rede no Navegador (Protótipo Isolado em `web/` + `libs/netcore`)

* **Não usar Lockstep em BLACK:** `cricket07` (MIT) usa lockstep single-thread; BLACK usa múltiplas threads e ponto flutuante x87/SSE, tornando lockstep inviável para 4 jogadores na internet.
* **Host Autoritativo com Snapshots (`libs/netcore`):** C++20 puro sem dependências de SO. Compila igual para Windows e WASM, usando `RTCDataChannel` (`Reliable` ordenado e `Unreliable` `ordered: false, maxRetransmits: 0`) no navegador.
* **Estado atual da pasta `web/`:** é **exclusivamente uma bancada de teste P2P em JavaScript** para validar o protocolo `0xB1AC` e o verificador local OPFS (`default.xbe` real `> 1 MB`, `Title ID 0x45410083`). **Não executa o jogo no navegador hoje.**

---

## 5. Decisão Pendente de Distribuição e Questão Jurídica do `.wasm`

1. **Assets originais:** O repositório nunca hospeda ISO, `default.xbe` ou conteúdo da EA/Criterion. O modelo atual exige que o jogador selecione sua própria cópia no navegador (copiada uma vez para o OPFS local, sem upload).
2. **Conflito "Clicar e Jogar" vs. WASM derivado:** O usuário manifestou preferência por "clicar e jogar" estilo `nfsu2.xyz`, mas distribuir em servidor os assets ou o `.wasm` recompilado entra em conflito com a política do projeto (o projeto `Virtual-On-Web` alerta que o `.wasm` recompilado contém instruções traduzidas do executável original). Mesmo usando OPFS para os assets, **como gerar o `.wasm` a partir do `default.xbe` do usuário dentro/fora do navegador** segue em aberto até a Fase 6.
