#pragma once
// Ponte entre o binário recompilado de BLACK (Xbox NTSC-U) e o núcleo de rede independente (libs/netcore).
// Arquivos mantidos em um único nível dentro de src/mp/ para casar com o glob src/*/*.cpp do SDK.

#include <array>
#include <cstdint>
#include <string>

#include "netcore/limits.hpp"

namespace mp {

class WebRtcTransport;

enum class Role : uint8_t {
    OfflineCampaign = 0,  // Campanha intacta (Black Enhanced): nenhuma lógica de multiplayer interfere
    Host = 1,             // Listen-server autoritativo (Black Enhanced Online)
    Client = 2,           // Cliente conectado a um host
};

struct HudPlayerRow {
    uint8_t player_id = 0;
    uint8_t team = 0;
    bool occupied = false;
    bool alive = false;
    uint8_t health = 0;
    uint8_t weapon = 0;
    uint8_t primary_weapon = netcore::kNoWeapon;
    uint8_t secondary_weapon = 0;
    uint8_t tactical_flags = 0;
    int16_t kills = 0;
    int16_t deaths = 0;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    std::string name;
};

struct BridgeTelemetry {
    uint8_t role = 0;
    uint8_t local_slot = 0;
    uint32_t tick = 0;
    uint32_t connected_peers = 0;
    bool client_joined = false;
    bool in_level = false;

    // Estado de Loadout / Economia / Combate local
    uint8_t primary_weapon = netcore::kNoWeapon;
    uint8_t secondary_weapon = 0;
    uint8_t active_weapon = 0;
    uint8_t active_slot = 0;
    uint8_t tactical_flags = 0;
    uint8_t last_loadout_result = 0;
    uint16_t credits = 0;
    uint16_t max_credits = 8000;
    uint16_t kit_cost = 0;
    uint16_t buy_ticks_left = 0;
    bool fixed_budget_mode = false;
    bool enforce_loadout = false;
    uint8_t local_health = 100;
    bool local_alive = true;
    int16_t local_kills = 0;
    int16_t local_deaths = 0;
    bool has_local_cam = false;
    float cam_x = 0.0f;
    float cam_y = 0.0f;
    float cam_z = 0.0f;
    float cam_fx = 0.0f;
    float cam_fy = 0.0f;
    float cam_fz = 1.0f;
    uint64_t hitmarker_seq = 0;
    uint32_t engine_weapon_count = 0;
    uint8_t active_engine_weapon_id = 0xFF;
    uint8_t arena_id = 0;
    uint8_t game_mode = 0;
    uint32_t available_weapon_mask = 0xFFFFFFFFu;

    // Telemetria específica do modo Territorial Assault (ta_active == false em Offline/PvP/Coop)
    bool ta_active = false;
    uint8_t ta_match_state = 0;
    uint8_t ta_attacker_team = 0;
    uint8_t ta_local_team = 0;
    uint8_t ta_current_objective = 0;
    uint8_t ta_objective_count = 2;
    uint8_t ta_captured_count = 0;
    uint8_t ta_current_obj_state = 0;
    uint16_t ta_progress_permille = 0;
    uint16_t ta_time_left_s = 0;
    bool ta_overtime = false;
    uint16_t ta_attacker_waves = 0;
    uint16_t ta_defender_waves = 0;
    uint8_t ta_winner_team = netcore::kNoPlayer;
    uint8_t ta_r1_captured = 0;
    uint16_t ta_r1_progress_permille = 0;
    uint16_t ta_r1_time_s = 0;
    uint8_t ta_r2_captured = 0;
    uint16_t ta_r2_progress_permille = 0;
    uint16_t ta_r2_time_s = 0;
    uint8_t ta_atk_wave_eta_s = 0;
    uint8_t ta_def_wave_eta_s = 0;
    uint8_t ta_zone_attackers = 0;
    uint8_t ta_zone_defenders = 0;
    bool ta_has_beacon = false;
    bool ta_in_zone = false;
    float ta_dist_m = 0.0f;
    int16_t ta_bearing_deg = 0;
    int16_t ta_elev_deg = 0;
    float ta_zone_x = 0.0f;
    float ta_zone_y = 0.0f;
    float ta_zone_z = 0.0f;
    float ta_zone_radius = 0.0f;

    std::array<HudPlayerRow, netcore::kMaxPlayers> players{};
};

// Inicializa a ponte multiplayer (lê flags/configurações sem alterar o modo campanha padrão).
void InitBridge();
void InitBridgeFromArgs(int argc, const char* const* argv);

// Inicia ou encerra sessão UDP real da Fase 4 sobre libs/netcore (Desktop/LAN).
bool StartHostUdp(uint16_t port = 45410, const std::string& player_name = "Host");
bool StartClientUdp(const std::string& remote_host, uint16_t port = 45410, const std::string& player_name = "Player");

// Inicia ou encerra sessão WebRTC (Navegador / DataChannels) sobre libs/netcore.
bool StartHostWebRtc(const std::string& player_name = "Host",
                     bool cs_loadout = true,
                     bool fixed_budget = false,
                     uint16_t budget_cap = 8000,
                     uint8_t game_mode = 0);
bool StartClientWebRtc(const std::string& player_name = "Player",
                       uint32_t host_peer = 1,
                       bool cs_loadout = true,
                       bool fixed_budget = false,
                       uint16_t budget_cap = 8000,
                       uint8_t game_mode = 0);

// Controles específicos do modo Territorial Assault (Lobby / Preparação / Rodadas).
bool SelectTerritorialTeam(uint8_t team);
bool EnterTerritorialPreparation();
bool BeginTerritorialRound();
// Configura o mapa, time e kit (15 SP) escolhidos no pré-lobby antes de entrar no motor 3D.
void ConfigurePreMatchLaunch(uint8_t mission_idx, uint8_t local_team, uint8_t primary_weapon,
                             uint8_t secondary_weapon, uint8_t tactical_flags);
bool IsDirectMapLaunchPending();
WebRtcTransport* ActiveWebRtcTransport();
BridgeTelemetry GetTelemetry(const uint8_t* guest_base);

// Sistema de Loadout CS-style (compra, orçamento, troca de slot 1/2 e aplicação no Character+0x278).
bool BuyLoadout(uint8_t primary_weapon, uint8_t secondary_weapon, uint8_t tactical_flags);
bool RequestEquipSlot(uint8_t slot_idx);
std::string GetWeaponCatalogJson(const uint8_t* guest_base);
bool ConsumeWeaponApplyRequest(const uint8_t* base, uint8_t& out_primary_eng_id,
                               uint8_t& out_secondary_eng_id, uint8_t& out_equip_slot,
                               uint8_t& out_tactical_flags);
bool ConsumeRemoteWeaponApplyRequest(const uint8_t* base, uint32_t& out_actor_va, uint8_t& out_eng_weapon_id);
bool ConsumeSlotSwitchRequest(const uint8_t* base, uint8_t& out_slot_idx);
void NotifyHitmarker(uint8_t target_slot, bool kill);
uint64_t HitmarkerSeq();

void StopMultiplayer();
bool IsClientJoined();

// Chamado quando a imagem convidada (default.xbe) é carregada na memória base (g_base).
void OnImageLoaded(uint8_t* guest_base);

// Chamado antes e depois da função de quadro do jogo (sub_0020F6A0).
void OnPreFrame(uint8_t* guest_base);
void OnPostFrame(uint8_t* guest_base);

// Teste #1 / #2 da Fase 2: vincula um ator convidado (ex.: aliado criado via 0x1AF880) a um slot de jogador remoto.
void BindRemoteActor(uint8_t player_id, uint32_t actor_va);
void UnbindRemoteActor(uint8_t player_id);
bool IsRemoteActor(uint32_t actor_va);
bool PrepareRemoteActorSpawn(uint8_t* guest_base, uint8_t& out_slot, uint32_t& out_name_va,
                             uint32_t& out_seq_va, uint32_t& out_arch, uint32_t& out_whash_lo,
                             uint32_t& out_whash_hi, uint32_t& out_spawner_va);
void CompleteRemoteActorSpawn(uint8_t* guest_base, uint8_t slot, uint32_t ai_controller_va);

// Teste #2 da Fase 2 (sub_001E5F70): retorna true se o controlador pertence a um ator remoto cujo cérebro
// de IA deve ser ignorado neste quadro.
bool ShouldSkipAiThink(uint32_t controller_va, const uint8_t* guest_base);

// Testes #7 e #8 da Fase 2 (sub_001D54C0 / sub_001BD360): em partida multiplayer, captura os dados do jogador
// local antes da morte e impede que o jogo dispare "Missão Fracassada" enquanto aguarda o RespawnEvent.
void OnLocalPlayerDying(uint8_t* guest_base);
bool ShouldSuppressMissionFailed(const uint8_t* guest_base);

// Interceptação de dano em partida multiplayer (sub_001E74B0 / sub_001DF830):
// Retorna true quando a função original do XBE deve ser suprimida (::kernel::Return(ctx, 0, 7)).
bool OnCharacterTakeBulletDamage(uint8_t* guest_base, uint32_t victim_va, uint32_t attacker_va, float raw_damage);
bool OnCharacterTakeBlastDamage(uint8_t* guest_base, uint32_t victim_va, float raw_damage, uint32_t blast_info_va);

// Sincronização espacial (posição, bbox e partição world+0x4960) via kFnCharSetPos em sub_001CCBB0:
// Preenche o vetor alinhado na memória convidada e devolve true para slots remotos ativos na fase.
bool PrepareRemoteActorTransform(uint8_t* guest_base, uint8_t slot, uint32_t& out_actor_va, uint32_t& out_pos_vec_va);

// Seleção de animação de locomoção em sub_0017E280 (kFnAnimSelect):
// Injeta temporariamente velocidade horizontal/marcha do snapshot sem alterar a simulação física.
bool OnAnimSelectPre(uint8_t* guest_base, uint32_t anim_va, float& out_saved_speed, uint8_t& out_saved_move, float& out_saved_ctrl_speed);
void OnAnimSelectPost(uint8_t* guest_base, uint32_t anim_va, float saved_speed, uint8_t saved_move, float saved_ctrl_speed);

// Proteção contra morte por esmagamento / colisão física (sub_001DFC30 / sub_001D6000) sobre slots de jogadores.
bool ShouldProtectActorFromCrush(const uint8_t* guest_base, uint32_t actor_va);

// Teste #9 da Fase 2: captura disparo real do jogador local em sub_001DBD50 (kFnWeaponFire)
// e prepara emissão visual/sonora (kFnTracerEmit / kFnWeaponFireSfx) de tiros remotos em sub_001CCBB0.
bool OnLocalWeaponFirePre(uint8_t* guest_base, uint32_t weapon_va);
void OnLocalWeaponFirePost(uint8_t* guest_base, uint32_t weapon_va);
bool PrepareRemoteFireRender(uint8_t* guest_base, uint32_t& out_tracer_mgr_va,
                             uint32_t& out_start_vec_va, uint32_t& out_end_vec_va,
                             uint32_t& out_weapon_va);

// Papel atual da instância. Por padrão é OfflineCampaign (garante que a campanha nunca quebre).
Role CurrentRole();
void SetRole(Role r);

uint64_t FrameCount();

}  // namespace mp
