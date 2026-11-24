#pragma once
// ESTADO DE VERIFICACAO:
// - Globais e offsets base da Fase 0 verificados EM RUNTIME no `assets/default.xbe` real
//   (Missao 1 / Level_00, log `black_enhanced_001.log`, Title ID 0x45410083).
// - Enderecos/offsets de combate e dano da Fase 1/2 conferidos estaticamente via `generated/game/`
//   sobre o mesmo `assets/default.xbe` (proveniencia MIT: `black-seamless-coop` e `Black-2006-Recomp`).

#include <bit>
#include <cstdint>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include "common/wasm_memory.h"
#endif

namespace black {

inline uint32_t FoldVa(uint32_t va) {
#ifdef __EMSCRIPTEN__
    return wasm_mem::FoldGuestVa(va);
#else
    return va;
#endif
}

// ---- Globais do executavel (estatico no default.xbe real; validacao em runtime pendente) --

inline constexpr uint32_t kGame = 0x002D45F0;            // Estatico (XBE): ponteiro principal Game (lido em sub_001BD360, sub_001D5570)
inline constexpr uint32_t kGameTargetLevel = 0x0002020C; // Estatico (XBE): uint8 ID interno do mapa (tabela 0x288648 -> {0,1,5,3,4,6,7,8}; sub_001132E0/sub_00122DE0)
inline constexpr uint32_t kGameTargetDiff = 0x0002020D;  // Estatico (XBE): uint8 modo/dificuldade de inicio da fase (=1 em sub_001132E0)
inline constexpr uint32_t kGameTargetUnit = 0x0002020E;  // Estatico (XBE): uint8 unidade inicial da fase (=1 em sub_001132E0)
inline constexpr uint32_t kFrontEndState = 0x00020220;   // Estatico (XBE): estado embutido do menu FrontEnd em kGame (sub_00020210)
inline constexpr uint32_t kGameState = 0x00021070;       // Estatico (XBE): offset do ponteiro de estado atual em kGame (sub_0020F6A0)
inline constexpr uint32_t kInLevelState = 0x00020F78;    // Estatico (XBE): estado embutido em kGame (vtable 0x295338 em sub_00020210)
inline constexpr uint32_t kFrontEnd = 0x002D199C;        // Estatico (XBE): ponteiro global do gerenciador de FrontEnd (sub_001132E0, sub_0020DCA0)
inline constexpr uint32_t kFeNextState = 0x00003880;     // Estatico (XBE): ponteiro de proximo estado consumido em 0x001244AC -> sub_0020DCA0
inline constexpr uint32_t kFeTriggerA = 0x00003884;      // Estatico (XBE): flag (=1) de transicao do FrontEnd (sub_001132E0)
inline constexpr uint32_t kFeTriggerB = 0x00003888;      // Estatico (XBE): flag (=1) de confirmacao de troca de estado (0x00124496)
inline constexpr uint8_t kMissionInternalLevelIds[8] = {0, 1, 5, 3, 4, 6, 7, 8};  // Tabela em 0x00288648 no default.xbe NTSC-U
inline constexpr uint32_t kGameFrameTime = 0x002F3A38;   // Estatico (XBE): delta time de quadro (sub_000BE380)
inline constexpr uint32_t kFrameCounter = 0x002F4368;    // Estatico (XBE): contador global de quadros incrementado em sub_0020F6A0

inline constexpr uint32_t kWorld = 0x002D45D4;           // Estatico (XBE): ponteiro para World ativo (sub_00014490, sub_0020F6A0)
inline constexpr uint32_t kWorldPlayer = 0x00000030;     // Estatico (XBE): Character do jogador embutido em world + 0x30 (vtable 0x28CFE0)
inline constexpr uint32_t kWorldController = 0x00000520; // Estatico (XBE): PlayerController embutido em world + 0x520 (vtable 0x28CF94; NAO e ponteiro)
inline constexpr uint32_t kWorldLastPos = 0x000001C0;    // Estatico (XBE): equiv. player+0x190 (world+0x30+0x190 = prevPos XMM; sub_001C9270)
inline constexpr uint32_t kWorldLevel = 0x00005AEC;      // Estatico (XBE): indice da fase atual — ATENCAO: uint8/int8 (X86_MEM8), NAO uint32!
inline constexpr uint32_t kWorldUnit = 0x00005AF0;       // Estatico (XBE): unidade/subarea atual (uint32 em 0x000B448A/0x000B44D2)

inline constexpr uint32_t kPool = 0x002D1980;            // Estatico (XBE): container de pool de personagens/atores (sub_001DF340, sub_001AF880)
inline constexpr uint32_t kAliveList = 0x000079B0;       // Estatico (XBE): lista em pool + 0x79B0 (flags +0, cap +4, array +0xC via sub_001C4A90)
inline constexpr uint32_t kLogic = 0x002D45D0;           // Estatico (XBE): gerenciador global de logica
inline constexpr uint32_t kLevel = 0x002D1990;           // Estatico (XBE): container global da fase (sub_001EC450)
inline constexpr uint32_t kTracerMgrOffset = 0x0004E2C0; // Estatico (XBE): subobjeto emissor de tracantes/faiscas em *kLevel + 0x4E2C0 (sub_001DBD50 -> sub_00168E20)
inline constexpr uint32_t kDisplay = 0x002D67F4;         // Estatico (XBE): gerenciador global de display/camera (sub_001D25D0)
inline constexpr uint32_t kCamera = 0x002D12B8;          // Estatico (XBE): ponteiro da camera em primeira pessoa (sub_001BB110)
inline constexpr uint32_t kCameraFovDeg = 0x0750;        // Estatico (XBE): float FOV em graus em *kCamera + 0x750 (sub_001BB110)
inline constexpr uint32_t kCameraOverrideFov = 0x16A4;   // Estatico (XBE): uint8 != 0 quando FOV ativo da arma aplica zoom (sub_001BB110)
inline constexpr uint32_t kWeaponSystem = 0x002D12D0;    // Estatico (XBE): tabela de WeaponDefs e pool de objetos Weapon* (sub_00014090 / sub_001C9F00)

// ---- Offsets de Personagem / Ator (Character — estatico no default.xbe real, sem runtime) ---

inline constexpr uint32_t kCharName = 0x008;             // uint64 (name_lo, name_hi): zerado na morte (sub_001D54C0)
inline constexpr uint32_t kCharPos = 0x0A0;              // float[3]: posicao (x, y, z) do ator (sub_001DFD90, sub_001DF830)
inline constexpr uint32_t kCharView = 0x0D0;             // float[4][4]: matriz de visao 64 bytes (montada em sub_001DFD90, copiada em sub_001EC450)
inline constexpr uint32_t kCharSlotMgr = 0x278;          // WeaponSlotMgr embutido em Character (sub_001C2690 / sub_001C9F00)
inline constexpr uint32_t kCharReserveAmmoPtr = 0x28C;   // uint8*: array de carregadores reserva por engine weapon_id
inline constexpr uint32_t kCharSlotsPtr = 0x298;         // uint32: ponteiro para Weapon*[2] (slot 0 primaria, slot 1 secundaria)
inline constexpr uint32_t kCharWeapon = 0x29C;           // uint32: ponteiro para a arma equipada (sub_001EC450, sub_001F0860)
inline constexpr uint32_t kCharZoomFactor = 0x2A4;       // float: fator atual de zoom/ADS em sub_001BB110 e sub_001BD3F0
inline constexpr uint32_t kCharSlotCount = 0x2BA;        // uint8: numero de slots de arma em WeaponSlotMgr (+0x278 + 0x42)
inline constexpr uint32_t kCharActiveSlot = 0x2BB;       // uint8: indice do slot ativo (WeaponSlotMgr + 0x43)
inline constexpr uint32_t kCharActiveWeaponId = 0x2BC;   // uint8: id interno da arma ativa (WeaponSlotMgr + 0x44)
inline constexpr uint32_t kCharMeasuredSpeed = 0x2D8;    // float: velocidade escalar lida por sub_0017E280
inline constexpr uint32_t kCharHealth = 0x2F0;           // float: vida atual (100.0f em sub_001AF880, dano em sub_001DF830)
inline constexpr uint32_t kCharControl = 0x324;          // uint32: ponteiro para Controller ativo (IA ou PlayerController)
inline constexpr uint32_t kCharAnim = 0x328;             // uint32: controlador de animacao (sub_001E5FA0, sub_001F0860)
inline constexpr uint32_t kCharState = 0x384;            // uint32: 0 = vivo (sub_001DF830), 2 = morto (sub_001D54C0 / sub_001BD360)
inline constexpr uint32_t kCharTeam = 0x39C;             // uint32: identificador de equipe/faccao (comparado em sub_001D5B00 / sub_001DEC20)
inline constexpr uint32_t kCharCrouch = 0x3A1;           // uint8: estado de agachamento
inline constexpr uint32_t kCharKinematic = 0x3A7;        // uint8: flag de fisica/cinematica (sub_001DF630 / sub_001E03D0; flag distinta em +0x3AA)
inline constexpr uint32_t kCharHit = 0x3AB;              // uint8: sinalizado (= 1) no handler de impacto/dano sub_001E74B0
inline constexpr uint32_t kCharDeathControl = 0x630;     // Controlador de morte embutido em Character (sub_001D54C0, sub_001E5FA0)

// ---- Offsets do Controlador ([char + 0x324] — estatico no default.xbe real, sem runtime) ---

inline constexpr uint32_t kControlAngles = 0x010;        // float[4]: espelho (+0x10), init (+0x14), aim yaw (+0x18 [-180..360)), pitch (+0x1C [-70..70]) EM GRAUS
inline constexpr uint32_t kControlSpeed = 0x020;         // float: velocidade escalar calculada (sub_001CC490 -> sub_001CE1A0)
inline constexpr uint32_t kControlCrouch = 0x040;        // uint8: agachamento comandado (sub_001EC450 / sub_001CC490)
inline constexpr uint32_t kControlFire = 0x041;          // uint8: gatilho de disparo (sub_001EC450 / sub_001F0860)
inline constexpr uint32_t kControlZoom = 0x042;          // uint8: mira/zoom ADS (sub_001EC450 / sub_001F0860)
inline constexpr uint32_t kControlReload = 0x044;        // uint8: comando de recarga (sub_001EC450 / sub_001F0860)
inline constexpr uint32_t kControlFireMode = 0x045;      // uint8: seletor de modo de tiro alternado (sub_001EC450 / sub_001F0860)
inline constexpr uint32_t kControlLocomotion = 0x04A;    // uint8: marcha S_MOV lida por sub_0017E280 (e setada em sub_001D5360..sub_001D5570)
inline constexpr uint32_t kControlMelee = 0x04B;         // uint8: golpe corpo-a-corpo (sub_001EC450 / sub_001F0860)
inline constexpr uint32_t kControlThrow = 0x04C;         // uint8: acao secund./arremesso passada de ctrl+0x4C em sub_001F0860 -> sub_001F0650
inline constexpr uint32_t kControlMove = 0x060;          // vec4: vetor movimento calculado (sub_001CC490 -> sub_001CE1A0)
inline constexpr uint32_t kControlMoveDir = 0x070;       // vec4: vetor direcao inicializado em sub_001BDA70 e passado a sub_001CE1A0
inline constexpr uint32_t kControlRate = 0x084;          // float: zerado no init do controlador em sub_001D5360/sub_001D5570/sub_001D20C0
inline constexpr uint32_t kControlChar = 0x08C;          // uint32: ponteiro de volta para o Character dono (sub_001E5F70, sub_001EC450, sub_001D54C0, sub_001BD360)
inline constexpr uint32_t kControlSensYaw = 0x0B8;       // float: fator de sensibilidade/sinal de yaw em sub_001BD3F0
inline constexpr uint32_t kControlSensPitch = 0x0BC;     // float: fator de sensibilidade/sinal de pitch em sub_001BD3F0

// ---- Enderecos de funcoes verificados estaticamente no default.xbe -------------------------

inline constexpr uint32_t kFnWeaponDefByHash = 0x00014090;       // Busca engine weapon_id por {hash_lo, hash_hi} (ecx = *kWeaponSystem, ret 0x08)
inline constexpr uint32_t kFnTracerEmit = 0x00168E20;            // Emite tracante/faisca visual (edx = *kLevel + 0x4E2C0, bl = 0, stack start*, end*, ret 0x08)
inline constexpr uint32_t kFnSpawnAllyFromPool = 0x001AF880;     // Spawn de aliado a partir do pool (12 args na pilha, ret 0x30)
inline constexpr uint32_t kFnAnimSelect = 0x0017E280;            // Selecao de animacao/locomocao (esi = char+0x328, plain ret)
inline constexpr uint32_t kFnCameraFovUpdate = 0x001BB110;       // Atualiza zoom/FOV de primeira pessoa em *kCamera + 0x750
inline constexpr uint32_t kFnPlayerLookUpdate = 0x001BD3F0;      // Atualiza mira (yaw/pitch) do jogador (esi = ctrl, ret 0x08)
inline constexpr uint32_t kFnWeaponEquip = 0x001C2690;           // Equipa Weapon* no slot_mgr (ecx = char+0x278, edx = Weapon*, plain ret)
inline constexpr uint32_t kFnWeaponFreeAll = 0x001C9F00;         // Libera todos os slots do WeaponSlotMgr (stack char+0x278, ret 0x04)
inline constexpr uint32_t kFnPoolUpdateAll = 0x001CCBB0;         // Atualizacao geral dos atores no pool (esi = pool, ret 0x04)
inline constexpr uint32_t kFnCharTeleport = 0x001CCCA0;          // Teleporte completo com matriz e pos ant. (esi = char, ebx = &{x,y,z,yaw}, plain ret)
inline constexpr uint32_t kFnCharSetPos = 0x001CE660;            // Atualiza pos, bbox e particao world+0x4960 (ecx = char, eax = &{x,y,z,w}, plain ret)
inline constexpr uint32_t kFnWeaponSpawn = 0x001CEE10;           // Aloca Weapon* no pool (stack {char, engine_weapon_id, flag}, ret 0x0C -> eax)
inline constexpr uint32_t kFnCharCrush = 0x001D6000;             // Morte por esmagamento fisico (edx = vitima, ret 0x04)
inline constexpr uint32_t kFnFireBulletPrimary = 0x001DB7B0;     // Player_FireBulletPrimary (ret 0x14, pool edx+0x6E0)
inline constexpr uint32_t kFnWeaponFireSfx = 0x001DB860;         // Toca efeito sonoro de disparo da arma (eax = Weapon*, plain ret)
inline constexpr uint32_t kFnWeaponFire = 0x001DBD50;            // Rotina unificada de disparo da arma (stack Weapon*, ret 0x04)
inline constexpr uint32_t kFnCharTakeBlastDamage = 0x001DF830;   // Dano de explosao/impacto em Character (ret 0x1C)
inline constexpr uint32_t kFnCharBodyHit = 0x001DFC30;           // Colisao fatal por corpo em movimento (ecx = vitima, ret 0x10)
inline constexpr uint32_t kFnCharTakeBulletDamage = 0x001E74B0;  // Dano de projetil/golpe com atacante em [esp+0x18] (ret 0x1C)

// ---- Acessores seguros sobre a base da memória convidada -----------------------------------

inline uint8_t R8(const uint8_t* base, uint32_t va) {
    return base[FoldVa(va)];
}

inline uint32_t R32(const uint8_t* base, uint32_t va) {
    uint32_t v;
    std::memcpy(&v, base + FoldVa(va), sizeof(v));
    return v;
}

inline float RF32(const uint8_t* base, uint32_t va) {
    float v;
    std::memcpy(&v, base + FoldVa(va), sizeof(v));
    return v;
}

inline void W8(uint8_t* base, uint32_t va, uint8_t v) {
    base[FoldVa(va)] = v;
}

inline uint16_t R16(const uint8_t* base, uint32_t va) {
    uint16_t v = 0;
    std::memcpy(&v, base + FoldVa(va), sizeof(v));
    return v;
}

inline void W16(uint8_t* base, uint32_t va, uint16_t v) {
    std::memcpy(base + FoldVa(va), &v, sizeof(v));
}

inline void W32(uint8_t* base, uint32_t va, uint32_t v) {
    std::memcpy(base + FoldVa(va), &v, sizeof(v));
}

inline void WF32(uint8_t* base, uint32_t va, float v) {
    std::memcpy(base + FoldVa(va), &v, sizeof(v));
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
