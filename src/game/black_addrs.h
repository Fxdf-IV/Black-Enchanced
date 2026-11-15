#pragma once
// ATENCAO — ESTADO DE HIPOTESE LOCAL (CONFIANCA BAIXA ATE VERIFICAR NO XBE):
// Estes enderecos e offsets NAO foram verificados localmente contra o default.xbe real de BLACK.
// Proveniencia MIT documentada em build/refs/black-seamless-coop (src/game/coop*.cpp) e
// build/refs/Black-2006-Recomp (docs/plans/split-screen-coop.md, src/recomp_manual.c);
// exigem validacao direta no Ghidra + [xbe] trace na Fase 1 (consulte docs/re-log.md).

#include <bit>
#include <cstdint>
#include <cstring>

namespace black {

// ---- Globais hipoteticos do executavel (default.xbe alvo: Title ID 0x45410083, XDK 5849) --

inline constexpr uint32_t kGame = 0x002D45F0;            // Hipotese (Baixa): ponteiro para objeto principal do jogo
inline constexpr uint32_t kGameState = 0x00021070;       // Hipotese (Baixa): offset do estado atual dentro de kGame
inline constexpr uint32_t kInLevelState = 0x00020F78;    // Hipotese (Baixa): offset do estado "em fase" dentro de kGame
inline constexpr uint32_t kGameFrameTime = 0x002F3A38;   // Hipotese (Baixa): delta time alvo (1/30 s; ref. MIT Black-2006-Recomp)
inline constexpr uint32_t kFrameCounter = 0x002F4368;    // Hipotese (Baixa): contador de quadros incrementado em 0x20F6A0

inline constexpr uint32_t kWorld = 0x002D45D4;           // Hipotese (Baixa): ponteiro para container do mundo ativo
inline constexpr uint32_t kWorldPlayer = 0x00000030;     // Hipotese (Baixa): ator do jogador local em world + 0x30 (embutido)
inline constexpr uint32_t kWorldController = 0x00000520; // Hipotese (Baixa): PlayerController embutido em world + 0x520 (NAO e ponteiro)
inline constexpr uint32_t kWorldLastPos = 0x000001C0;    // Hipotese (Baixa): posicao no quadro anterior
inline constexpr uint32_t kWorldLevel = 0x00005AEC;      // Hipotese (Baixa): indice da fase atual
inline constexpr uint32_t kWorldUnit = 0x00005AF0;       // Hipotese (Baixa): unidade/subarea atual

inline constexpr uint32_t kPool = 0x002D1980;            // Hipotese (Baixa): pool de personagens/atores
inline constexpr uint32_t kAliveList = 0x000079B0;       // Hipotese (Baixa): lista de atores vivos em pool + 0x79B0 (flags +0, cap +4, count +8, array +0xC)
inline constexpr uint32_t kLogic = 0x002D45D0;           // Hipotese (Baixa): gerenciador de logica e spawners
inline constexpr uint32_t kLevel = 0x002D1990;           // Hipotese (Baixa): container da fase
inline constexpr uint32_t kDisplay = 0x002D67F4;         // Hipotese (Baixa): renderizador / camera do mundo

// ---- Offsets hipoteticos de Personagem / Ator (Character — CONFIANCA BAIXA) ----------------

inline constexpr uint32_t kCharName = 0x008;             // uint64 (name_lo, name_hi): identificador de 64 bits
inline constexpr uint32_t kCharPos = 0x0A0;              // float[3]: posição (x, y, z) dos pés do ator
inline constexpr uint32_t kCharView = 0x0D0;             // float[4][4]: matriz de visão (right, up, forward, eye)
inline constexpr uint32_t kCharWeapon = 0x29C;           // uint32: ponteiro para a arma equipada
inline constexpr uint32_t kCharHealth = 0x2F0;           // float: vida atual do ator
inline constexpr uint32_t kCharControl = 0x324;          // uint32: ponteiro para o controlador (IA ou jogador)
inline constexpr uint32_t kCharAnim = 0x328;             // uint32: controlador de animação
inline constexpr uint32_t kCharState = 0x384;            // uint32: 0 = vivo, 2 = morto, 3 = morte roteirizada
inline constexpr uint32_t kCharTeam = 0x39C;             // uint32: identificador de equipe/facção
inline constexpr uint32_t kCharCrouch = 0x3A1;           // uint8: 1 = agachado, 0 = em pé
inline constexpr uint32_t kCharKinematic = 0x3A7;        // uint8: 1 = cinemático (sem gravidade/física sobrescrevendo)
inline constexpr uint32_t kCharHit = 0x3AB;              // uint8: contador de reações a acerto
inline constexpr uint32_t kCharDeathControl = 0x630;     // Controlador de morte embutido no ator

// ---- Offsets hipoteticos do Controlador de IA / Personagem ([char + 0x324] — BAIXA) ---------

inline constexpr uint32_t kControlAngles = 0x010;        // float[4]: yaw (+0x10), torso (+0x14), aim (+0x18), pitch (+0x1C) em graus
inline constexpr uint32_t kControlSpeed = 0x020;         // float: velocidade escalar de deslocamento
inline constexpr uint32_t kControlCrouch = 0x040;        // uint8: flag de agachamento comandada
inline constexpr uint32_t kControlFire = 0x041;          // uint8: gatilho de disparo
inline constexpr uint32_t kControlReload = 0x044;        // uint8: comando de recarga
inline constexpr uint32_t kControlLocomotion = 0x04A;    // uint8: seletor de marcha (caminhada/corrida)
inline constexpr uint32_t kControlMelee = 0x04B;         // uint8: golpe corpo-a-corpo
inline constexpr uint32_t kControlThrow = 0x04C;         // uint8: arremesso de granada
inline constexpr uint32_t kControlMove = 0x060;          // float[3]: vetor direção de deslocamento
inline constexpr uint32_t kControlMoveDir = 0x070;       // float[3]: direção para blend de locomoção
inline constexpr uint32_t kControlRate = 0x084;          // float: taxa de blend de locomoção
inline constexpr uint32_t kControlChar = 0x08C;          // uint32: ponteiro de volta para o Character dono

// ---- Acessores seguros sobre a base da memória convidada -----------------------------------

inline uint8_t R8(const uint8_t* base, uint32_t va) {
    return base[va];
}

inline uint32_t R32(const uint8_t* base, uint32_t va) {
    uint32_t v;
    std::memcpy(&v, base + va, sizeof(v));
    return v;
}

inline float RF32(const uint8_t* base, uint32_t va) {
    float v;
    std::memcpy(&v, base + va, sizeof(v));
    return v;
}

inline void W8(uint8_t* base, uint32_t va, uint8_t v) {
    base[va] = v;
}

inline void W32(uint8_t* base, uint32_t va, uint32_t v) {
    std::memcpy(base + va, &v, sizeof(v));
}

inline void WF32(uint8_t* base, uint32_t va, float v) {
    std::memcpy(base + va, &v, sizeof(v));
}

// Verifica se há uma fase carregada e em execução (ou pausada dentro da fase).
inline bool IsInLevel(const uint8_t* base) {
    if (!base) return false;
    const uint32_t game = R32(base, kGame);
    if (!game) return false;
    return R32(base, game + kGameState) == game + kInLevelState;
}

// Devolve o endereço convidado do jogador local (world + 0x30), ou 0 se fora de fase.
inline uint32_t LocalPlayerAddr(const uint8_t* base) {
    if (!base) return 0;
    const uint32_t world = R32(base, kWorld);
    return world ? (world + kWorldPlayer) : 0;
}

}  // namespace black
