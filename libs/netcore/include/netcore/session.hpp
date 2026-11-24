#pragma once
// Máquinas de estado de Host autoritativo (listen-server) e Cliente para o MVP 0.1.
// Implementa exatamente a divisão de responsabilidades da seção 9 do plano:
// - Cliente decide seu movimento local e reporta acertos (FireEvent com claimed_target).
// - Host valida limites de velocidade/posição, cadência, munição, alcance e estados de vida;
//   decide dano, mortes, respawns, pontuação e fim de partida; e expulsa clientes hostis.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "netcore/protocol.hpp"
#include "netcore/rules.hpp"
#include "netcore/territorial.hpp"
#include "netcore/transport.hpp"

namespace netcore {

struct HostPlayerSlot {
    bool occupied = false;
    bool is_local_host = false;
    uint8_t player_id = 0;
    uint8_t team = 0;  // 0=Team A, 1=Team B (usado em TerritorialAssault)
    PeerId peer = kInvalidPeer;
    std::string name;

    Vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    Vec3 velocity{};
    uint8_t anim_state = 0;
    uint8_t weapon = 0;
    uint8_t health = 100;
    uint8_t flags = kFlagAlive;
    int16_t kills = 0;
    int16_t deaths = 0;

    uint32_t last_sequence = 0;
    uint32_t last_state_tick = 0;
    uint32_t last_fire_tick = 0;
    uint16_t ammo_in_mag = 30;
    uint32_t respawn_at_tick = 0;

    // Economia e Loadout autoritativo (CS-style)
    uint16_t credits = 0;
    uint8_t owned_primary = kNoWeapon;
    uint8_t owned_secondary = 0;
    uint8_t tactical_flags = 0;
    uint8_t last_loadout_result = 0;
    uint32_t spawn_tick = 0;
    uint32_t respawn_grace_until_tick = 0;
    bool entered_level = false;

    // Contadores de segurança por cliente
    uint16_t packets_in_window = 0;
    uint8_t violations = 0;
};

class HostSession {
public:
    HostSession(ITransport& transport, RulesConfig rules, std::string host_name = "Host");

    // Inicia a partida (Warmup -> Running), posicionando todos os jogadores conectados nos spawns.
    // Em TerritorialAssault, transita Lobby -> Preparation -> Round1 (ou Intermission -> Round2).
    void start_match();

    // Controles específicos do modo Territorial Assault (Lobby -> Preparation -> Round1/Round2).
    bool set_player_team(uint8_t player_id, ta::Team team);
    bool enter_preparation();
    bool begin_next_round();
    const ta::TerritorialMatch* territorial_match() const { return ta_match_.get(); }
    TerritorialState current_territorial_state() const;
    const std::vector<TerritorialEvent>& recent_territorial_events() const { return recent_ta_events_; }

    // Atualiza os pontos de respawn da partida para coordenadas reais da fase sem alterar rules_hash_.
    void set_spawn_points(std::vector<SpawnPoint> spawns);

    // Sincroniza fase ativa e máscara de armas nativas carregadas (sem alterar rules_hash_).
    void set_level_context(uint8_t arena_id, uint32_t available_weapon_mask);

    // Atualiza o estado do jogador local do host (slot 0).
    void set_local_player_state(const PlayerState& state);

    // O próprio host dispara / reivindica dano de explosão passando pela mesma validação dos clientes.
    bool local_fire(const FireEvent& ev);
    bool local_blast(BlastEvent ev);

    // O próprio host compra/equipa loadout passando pela mesma validação de orçamento e janela de compra.
    bool local_buy_loadout(const LoadoutRequest& req);
    LoadoutState slot_loadout_state(uint8_t player_id) const;

    // Reabre a janela de compra do slot quando o controle jogável inicia após cutscene.
    void refresh_buy_window(uint8_t player_id);

    // Aplica dano autoritativo de um atacante para uma vítima (usado pelo host após validação no motor).
    bool apply_damage(uint8_t attacker_id, uint8_t victim_id, uint16_t amount, uint8_t weapon_id = 0);

    // Avança 1 tick da simulação do servidor (processa pacotes de rede, respawns, tempo, snapshots).
    void tick();

    uint32_t current_tick() const { return tick_; }
    MatchPhase phase() const { return phase_; }
    uint8_t winner() const { return winner_; }
    const RulesConfig& rules() const { return rules_; }
    const std::array<HostPlayerSlot, kMaxPlayers>& slots() const { return slots_; }
    uint8_t active_arena_id() const { return active_arena_id_; }
    uint32_t available_weapon_mask() const { return available_weapon_mask_; }
    std::size_t player_count() const;
    const std::vector<FireEvent>& recent_fires() const { return recent_fires_; }
    const std::vector<BlastEvent>& recent_blasts() const { return recent_blasts_; }
    void clear_events() {
        recent_fires_.clear();
        recent_blasts_.clear();
        recent_ta_events_.clear();
    }

private:
    void handle_packet(PeerId peer, const std::vector<uint8_t>& payload);
    void handle_disconnect(PeerId peer);
    void handle_hello(PeerId peer, const Hello& msg);
    void handle_player_state(HostPlayerSlot& slot, const PlayerState& msg);
    void handle_fire(HostPlayerSlot& shooter, const FireEvent& msg);
    void handle_blast(HostPlayerSlot& sender, const BlastEvent& msg);
    void handle_loadout_request(HostPlayerSlot& slot, const LoadoutRequest& msg);
    void handle_team_select(HostPlayerSlot& slot, const TeamSelectRequest& msg);
    void drain_territorial_events();
    void spawn_player_for_territorial_role(HostPlayerSlot& slot, uint8_t spawn_cursor);
    void broadcast_territorial_state();

    bool apply_validated_fire(HostPlayerSlot& shooter, const FireEvent& msg);
    bool apply_validated_blast(HostPlayerSlot* sender, const BlastEvent& msg);
    bool apply_damage_internal(uint8_t attacker_id, uint8_t victim_id, uint16_t amount,
                               uint8_t weapon_id, bool allow_self_or_env);
    bool apply_loadout(HostPlayerSlot& slot, const LoadoutRequest& req);
    LoadoutState build_loadout_state(const HostPlayerSlot& slot, LoadoutResult result) const;
    uint32_t compute_kit_cost(uint8_t primary_weapon, uint8_t secondary_weapon, uint8_t tactical_flags) const;
    void init_slot_loadout(HostPlayerSlot& slot);
    void spawn_player(HostPlayerSlot& slot, uint8_t spawn_idx);
    void check_match_end();
    void broadcast_roster();
    void broadcast_snapshot();
    void penalize(PeerId peer, HostPlayerSlot* slot, KickReason reason_if_kicked);
    void kick_peer(PeerId peer, HostPlayerSlot* slot, KickReason reason);
    HostPlayerSlot* find_slot_by_peer(PeerId peer);

    ITransport& transport_;
    RulesConfig rules_;
    uint32_t rules_hash_ = 0;
    uint32_t tick_ = 0;
    uint32_t match_start_tick_ = 0;
    MatchPhase phase_ = MatchPhase::Warmup;
    uint8_t winner_ = kNoPlayer;
    uint8_t next_spawn_cursor_ = 0;
    uint8_t active_arena_id_ = 0;
    uint32_t available_weapon_mask_ = 0xFFFFFFFFu;

    std::array<HostPlayerSlot, kMaxPlayers> slots_{};
    std::vector<FireEvent> recent_fires_;
    std::vector<BlastEvent> recent_blasts_;
    std::vector<TerritorialEvent> recent_ta_events_;
    std::unique_ptr<ta::TerritorialMatch> ta_match_;
    uint32_t ta_intermission_remaining_ = 0;
    std::unordered_map<PeerId, uint16_t> unauthenticated_packets_;
    std::unordered_map<PeerId, uint8_t> unauthenticated_violations_;
};

class ClientSession {
public:
    ClientSession(ITransport& transport, PeerId host_peer, RulesConfig rules, std::string player_name);

    // Envia o Hello inicial ao host.
    bool send_hello();

    // Envia o estado atual do jogador local ao host (canal não confiável, com número de sequência).
    bool send_local_state(PlayerState state);

    // Envia um evento de disparo / reivindicação de explosão ao host (canal confiável).
    bool send_fire(FireEvent ev);
    bool send_blast(BlastEvent ev);

    // Envia pedido de compra/equipamento de loadout ao host (canal confiável).
    bool send_loadout_request(const LoadoutRequest& req);

    // Envia pedido de escolha de equipe no Lobby do Territorial Assault (canal confiável).
    bool send_team_select(ta::Team team);

    // Envia um Ping para medir RTT e relógio do host.
    bool send_ping(uint32_t nonce, uint32_t now_ms);

    // Processa pacotes recebidos do host.
    void poll();

    bool is_joined() const { return joined_; }
    bool is_rejected() const { return rejected_.has_value(); }
    bool is_kicked() const { return kicked_.has_value(); }
    std::optional<RejectReason> reject_reason() const { return rejected_; }
    std::optional<KickReason> kick_reason() const { return kicked_; }
    uint8_t local_player_id() const { return player_id_; }
    MatchPhase phase() const { return latest_snapshot_.phase; }
    uint8_t winner() const { return winner_; }
    const WorldSnapshot& latest_snapshot() const { return latest_snapshot_; }
    const std::vector<FireEvent>& recent_fires() const { return recent_fires_; }
    const std::vector<BlastEvent>& recent_blasts() const { return recent_blasts_; }
    const std::vector<DamageEvent>& recent_damages() const { return recent_damages_; }
    uint8_t active_arena_id() const { return active_arena_id_; }
    uint8_t game_mode() const { return game_mode_; }
    uint32_t available_weapon_mask() const { return available_weapon_mask_; }
    const std::vector<DeathEvent>& recent_deaths() const { return recent_deaths_; }
    const std::vector<RespawnEvent>& recent_respawns() const { return recent_respawns_; }
    const std::array<LoadoutState, kMaxPlayers>& loadouts() const { return loadouts_; }
    LoadoutState local_loadout() const {
        return player_id_ < kMaxPlayers ? loadouts_[player_id_] : LoadoutState{};
    }
    const std::vector<LoadoutState>& recent_loadouts() const { return recent_loadouts_; }
    std::string player_name(uint8_t player_id) const {
        return player_id < kMaxPlayers ? player_names_[player_id] : std::string{};
    }
    std::optional<Pong> last_pong() const { return last_pong_; }
    const std::optional<TerritorialState>& latest_territorial_state() const { return latest_ta_state_; }
    const std::vector<TerritorialEvent>& recent_territorial_events() const { return recent_ta_events_; }

    void clear_events() {
        recent_fires_.clear();
        recent_blasts_.clear();
        recent_damages_.clear();
        recent_deaths_.clear();
        recent_respawns_.clear();
        recent_loadouts_.clear();
        recent_ta_events_.clear();
    }

private:
    void handle_message(const Message& msg);

    ITransport& transport_;
    PeerId host_peer_ = kInvalidPeer;
    RulesConfig rules_;
    std::string name_;

    bool joined_ = false;
    uint8_t player_id_ = kNoPlayer;
    uint32_t next_sequence_ = 1;
    uint8_t winner_ = kNoPlayer;

    std::optional<RejectReason> rejected_;
    std::optional<KickReason> kicked_;
    WorldSnapshot latest_snapshot_{};
    std::vector<FireEvent> recent_fires_;
    std::vector<BlastEvent> recent_blasts_;
    std::vector<DamageEvent> recent_damages_;
    std::vector<DeathEvent> recent_deaths_;
    std::vector<RespawnEvent> recent_respawns_;
    std::array<LoadoutState, kMaxPlayers> loadouts_{};
    std::array<std::string, kMaxPlayers> player_names_{};
    std::vector<LoadoutState> recent_loadouts_;
    std::optional<Pong> last_pong_;
    std::optional<TerritorialState> latest_ta_state_;
    std::vector<TerritorialEvent> recent_ta_events_;
    uint8_t active_arena_id_ = 0;
    uint8_t game_mode_ = 0;
    uint32_t available_weapon_mask_ = 0xFFFFFFFFu;
};

}  // namespace netcore
