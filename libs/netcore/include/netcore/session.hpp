#pragma once
// Máquinas de estado de Host autoritativo (listen-server) e Cliente para o MVP 0.1.
// Implementa exatamente a divisão de responsabilidades da seção 9 do plano:
// - Cliente decide seu movimento local e reporta acertos (FireEvent com claimed_target).
// - Host valida limites de velocidade/posição, cadência, munição, alcance e estados de vida;
//   decide dano, mortes, respawns, pontuação e fim de partida; e expulsa clientes hostis.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "netcore/protocol.hpp"
#include "netcore/rules.hpp"
#include "netcore/transport.hpp"

namespace netcore {

struct HostPlayerSlot {
    bool occupied = false;
    bool is_local_host = false;
    uint8_t player_id = 0;
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

    // Contadores de segurança por cliente
    uint16_t packets_in_window = 0;
    uint8_t violations = 0;
};

class HostSession {
public:
    HostSession(ITransport& transport, RulesConfig rules, std::string host_name = "Host");

    // Inicia a partida (Warmup -> Running), posicionando todos os jogadores conectados nos spawns.
    void start_match();

    // Atualiza o estado do jogador local do host (slot 0).
    void set_local_player_state(const PlayerState& state);

    // O próprio host dispara (passa pela mesma validação de arma/cadência/alcance que os clientes).
    bool local_fire(const FireEvent& ev);

    // Avança 1 tick da simulação do servidor (processa pacotes de rede, respawns, tempo, snapshots).
    void tick();

    uint32_t current_tick() const { return tick_; }
    MatchPhase phase() const { return phase_; }
    uint8_t winner() const { return winner_; }
    const RulesConfig& rules() const { return rules_; }
    const std::array<HostPlayerSlot, kMaxPlayers>& slots() const { return slots_; }
    std::size_t player_count() const;

private:
    void handle_packet(PeerId peer, const std::vector<uint8_t>& payload);
    void handle_disconnect(PeerId peer);
    void handle_hello(PeerId peer, const Hello& msg);
    void handle_player_state(HostPlayerSlot& slot, const PlayerState& msg);
    void handle_fire(HostPlayerSlot& shooter, const FireEvent& msg);

    bool apply_validated_fire(HostPlayerSlot& shooter, const FireEvent& msg);
    void spawn_player(HostPlayerSlot& slot, uint8_t spawn_idx);
    void check_match_end();
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

    std::array<HostPlayerSlot, kMaxPlayers> slots_{};
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

    // Envia um evento de disparo ao host (canal confiável).
    bool send_fire(FireEvent ev);

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
    const std::vector<DamageEvent>& recent_damages() const { return recent_damages_; }
    const std::vector<DeathEvent>& recent_deaths() const { return recent_deaths_; }
    const std::vector<RespawnEvent>& recent_respawns() const { return recent_respawns_; }
    std::optional<Pong> last_pong() const { return last_pong_; }

    void clear_events() {
        recent_damages_.clear();
        recent_deaths_.clear();
        recent_respawns_.clear();
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
    std::vector<DamageEvent> recent_damages_;
    std::vector<DeathEvent> recent_deaths_;
    std::vector<RespawnEvent> recent_respawns_;
    std::optional<Pong> last_pong_;
};

}  // namespace netcore
