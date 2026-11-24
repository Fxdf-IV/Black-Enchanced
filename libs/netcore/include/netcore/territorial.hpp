#pragma once
// Territorial Assault (GDD v1.0): lógica pura e determinística do modo, sem dependência do motor, do
// relógio do sistema ou de rede. O host injeta o tempo chamando tick() a 30 Hz e informa mortes e ocupação
// de zona já validadas (velocidade/posição). Valores numéricos são hipóteses de balanceamento (Config).
// Decisões para as lacunas do GDD estão em docs/TERRITORIAL_ASSAULT_REGRAS.md.

#include <cstdint>
#include <vector>

namespace netcore::ta {

enum class Team : uint8_t { A = 0, B = 1 };
enum class Role : uint8_t { Attack = 0, Defense = 1 };
enum class MatchState : uint8_t { Lobby, Preparation, Round1, Intermission, Round2, Results };
enum class ObjectiveState : uint8_t { Locked, Available, Capturing, Contested, Captured, Disabled };
enum class RoundEndReason : uint8_t {
    None,
    AllCaptured,
    AttackersEliminated,
    DefendersEliminated,
    TimeExpired,
    OvertimeCap,
    Abandonment,
    OutcomeDecided,
};
enum class EventType : uint8_t { StateChanged, ObjectiveCaptured, WaveSpawned, OvertimeStarted, RoundEnded };

struct Config {
    uint16_t tick_rate_hz = 30;
    uint8_t max_players = 8;
    uint8_t objective_count = 3;           // 1..16
    uint32_t capture_ticks = 30u * 30u;    // 30 s com superioridade numérica (velocidade constante)
    uint16_t attacker_initial_waves = 5;
    uint16_t defender_initial_waves = 15;
    uint32_t wave_interval_ticks = 20u * 30u;
    uint16_t waves_per_objective = 5;      // concedidas aos atacantes
    uint16_t max_waves = 30;
    uint32_t initial_time_ticks = 10u * 60u * 30u;
    uint32_t bonus_time_ticks = 5u * 60u * 30u;
    uint32_t max_time_ticks = 25u * 60u * 30u;  // teto do tempo restante
    bool overtime_enabled = true;
    uint32_t overtime_cap_ticks = 90u * 30u;   // teto do overtime (evita rodada travada)
    bool round2_early_end = true;
    uint16_t supply_points = 15;
};

struct Loadout {
    uint16_t primary_cost = 0;
    uint16_t secondary_cost = 0;
    uint16_t accessories_cost = 0;
    uint16_t explosives_cost = 0;
    uint16_t tactical_cost = 0;
    uint16_t protection_cost = 0;
    bool has_primary = false;
};

// Orçamento individual fixo; soma em 32 bits para impedir estouro por mensagem maliciosa.
bool validate_loadout(const Loadout& l, const Config& cfg);

struct RoundResult {
    Team attacker = Team::A;
    uint8_t captured = 0;
    int8_t last_objective = -1;
    uint16_t progress_permille = 0;   // progresso do próximo objetivo no encerramento
    uint32_t elapsed_ticks = 0;
    uint32_t completion_ticks = 0;    // válido se completed
    uint16_t attacker_waves_left = 0;
    uint16_t defender_waves_left = 0;
    bool completed = false;
    RoundEndReason reason = RoundEndReason::None;
};

// +1: a melhor, -1: b melhor, 0: empate. Hierarquia do GDD §8.2.
int compare_results(const RoundResult& a, const RoundResult& b);

struct Event {
    EventType type = EventType::StateChanged;
    Team team = Team::A;
    uint8_t index = 0;   // objetivo ou novo estado
    uint16_t count = 0;  // jogadores reapareceram / ondas restantes
};

struct PlayerTA {
    uint8_t id = 0;
    Team team = Team::A;
    bool alive = true;
    bool in_zone = false;
};

class TerritorialMatch {
public:
    explicit TerritorialMatch(const Config& cfg = Config{});

    bool add_player(uint8_t id, Team team);  // somente no Lobby
    bool remove_player(uint8_t id);          // desconexão em qualquer estado
    bool start_match();                      // Lobby -> Preparation (>= 1 jogador por equipe)
    bool begin_round();                      // Preparation -> Round1; Intermission -> Round2
    void tick();                             // um passo de simulação (30 Hz)

    bool kill_player(uint8_t id);
    bool set_in_zone(uint8_t id, bool in_zone);

    MatchState state() const { return state_; }
    Team attacker_team() const;
    Role role_of(Team t) const;
    uint8_t current_objective() const { return objective_; }
    ObjectiveState objective_state(uint8_t idx) const;
    uint16_t progress_permille() const;
    uint32_t time_remaining_ticks() const { return time_remaining_; }
    bool in_overtime() const { return overtime_; }
    uint16_t waves(Team t) const { return t == attacker_team() ? atk_waves_ : def_waves_; }
    uint8_t captured_count() const { return captured_; }
    const PlayerTA* find_player(uint8_t id) const;
    const std::vector<PlayerTA>& players() const { return players_; }
    const RoundResult& round_result(int round) const { return results_[round == 2 ? 1 : 0]; }
    bool has_winner() const { return state_ == MatchState::Results; }
    // 0 = Equipe A, 1 = Equipe B, -1 = empate
    int winner() const;
    std::vector<Event> drain_events();

private:
    bool in_round() const { return state_ == MatchState::Round1 || state_ == MatchState::Round2; }
    int round_index() const { return state_ == MatchState::Round2 || round2_started_ ? 1 : 0; }
    void set_state(MatchState s);
    void init_round();
    uint32_t alive_count(Team t) const;
    uint32_t dead_count(Team t) const;
    uint32_t connected_count(Team t) const;
    uint32_t zone_count(Team t) const;
    void update_capture();
    void update_waves(Team t);
    bool check_end();
    void end_round(RoundEndReason reason);
    RoundResult snapshot(RoundEndReason reason) const;
    void push(EventType t, Team team, uint8_t idx, uint16_t count);

    Config cfg_;
    MatchState state_ = MatchState::Lobby;
    bool round2_started_ = false;
    std::vector<PlayerTA> players_;
    std::vector<Event> events_;
    RoundResult results_[2];

    uint8_t objective_ = 0;
    uint8_t captured_ = 0;
    uint32_t progress_ticks_ = 0;
    bool contested_ = false;
    bool capturing_ = false;
    uint32_t elapsed_ = 0;
    uint32_t time_remaining_ = 0;
    uint32_t time_cap_total_ = 0;  // 0 = sem "tempo a bater"
    bool overtime_ = false;
    uint32_t overtime_ticks_ = 0;
    uint16_t atk_waves_ = 0;
    uint16_t def_waves_ = 0;
    bool wave_active_[2] = {false, false};
    uint32_t wave_timer_[2] = {0, 0};
    bool abandoned_ = false;
    Team abandoned_team_ = Team::A;
};

}  // namespace netcore::ta
