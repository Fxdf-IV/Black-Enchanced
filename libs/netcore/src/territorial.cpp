#include "netcore/territorial.hpp"

#include <algorithm>

namespace netcore::ta {

namespace {
constexpr uint8_t kMaxObjectives = 16;
inline int idx(Team t) { return static_cast<int>(t); }
inline Team other(Team t) { return t == Team::A ? Team::B : Team::A; }
}  // namespace

bool validate_loadout(const Loadout& l, const Config& cfg) {
    if (!l.has_primary) return false;
    const uint32_t total = uint32_t(l.primary_cost) + l.secondary_cost + l.accessories_cost + l.explosives_cost +
                           l.tactical_cost + l.protection_cost;
    return total <= cfg.supply_points;
}

int compare_results(const RoundResult& a, const RoundResult& b) {
    if (a.captured != b.captured) return a.captured > b.captured ? 1 : -1;
    if (a.completed && b.completed) {
        if (a.completion_ticks != b.completion_ticks) return a.completion_ticks < b.completion_ticks ? 1 : -1;
        return 0;
    }
    if (a.progress_permille != b.progress_permille) return a.progress_permille > b.progress_permille ? 1 : -1;
    return 0;
}

TerritorialMatch::TerritorialMatch(const Config& cfg) : cfg_(cfg) {
    cfg_.objective_count = std::clamp<uint8_t>(cfg_.objective_count, 1, kMaxObjectives);
    cfg_.capture_ticks = std::max<uint32_t>(cfg_.capture_ticks, 1);
    cfg_.wave_interval_ticks = std::max<uint32_t>(cfg_.wave_interval_ticks, 1);
    cfg_.max_players = std::clamp<uint8_t>(cfg_.max_players, 2, 16);
    cfg_.max_time_ticks = std::max(cfg_.max_time_ticks, cfg_.initial_time_ticks);
}

void TerritorialMatch::push(EventType t, Team team, uint8_t i, uint16_t count) {
    events_.push_back(Event{t, team, i, count});
}

std::vector<Event> TerritorialMatch::drain_events() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

void TerritorialMatch::set_state(MatchState s) {
    state_ = s;
    push(EventType::StateChanged, Team::A, static_cast<uint8_t>(s), 0);
}

Team TerritorialMatch::attacker_team() const { return round2_started_ ? Team::B : Team::A; }

Role TerritorialMatch::role_of(Team t) const { return t == attacker_team() ? Role::Attack : Role::Defense; }

const PlayerTA* TerritorialMatch::find_player(uint8_t id) const {
    for (const auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

uint32_t TerritorialMatch::connected_count(Team t) const {
    uint32_t n = 0;
    for (const auto& p : players_) n += (p.team == t);
    return n;
}
uint32_t TerritorialMatch::alive_count(Team t) const {
    uint32_t n = 0;
    for (const auto& p : players_) n += (p.team == t && p.alive);
    return n;
}
uint32_t TerritorialMatch::dead_count(Team t) const { return connected_count(t) - alive_count(t); }
uint32_t TerritorialMatch::zone_count(Team t) const {
    uint32_t n = 0;
    for (const auto& p : players_) n += (p.team == t && p.alive && p.in_zone);
    return n;
}

bool TerritorialMatch::add_player(uint8_t id, Team team) {
    if (state_ != MatchState::Lobby) return false;
    if (team != Team::A && team != Team::B) return false;
    if (players_.size() >= cfg_.max_players || find_player(id)) return false;
    players_.push_back(PlayerTA{id, team, true, false});
    return true;
}

bool TerritorialMatch::start_match() {
    if (state_ != MatchState::Lobby) return false;
    if (connected_count(Team::A) == 0 || connected_count(Team::B) == 0) return false;
    set_state(MatchState::Preparation);
    return true;
}

void TerritorialMatch::init_round() {
    objective_ = 0;
    captured_ = 0;
    progress_ticks_ = 0;
    capturing_ = contested_ = false;
    elapsed_ = 0;
    time_remaining_ = cfg_.initial_time_ticks;
    time_cap_total_ = 0;
    if (round2_started_ && results_[0].completed && results_[0].completion_ticks > 0) {
        time_cap_total_ = results_[0].completion_ticks;  // "tempo a bater"
        time_remaining_ = std::min(time_remaining_, time_cap_total_);
    }
    overtime_ = false;
    overtime_ticks_ = 0;
    atk_waves_ = std::min(cfg_.attacker_initial_waves, cfg_.max_waves);
    def_waves_ = std::min(cfg_.defender_initial_waves, cfg_.max_waves);
    wave_active_[0] = wave_active_[1] = false;
    wave_timer_[0] = wave_timer_[1] = 0;
    for (auto& p : players_) {
        p.alive = true;
        p.in_zone = false;
    }
}

bool TerritorialMatch::begin_round() {
    if (state_ == MatchState::Preparation) {
        set_state(MatchState::Round1);
        init_round();
        return true;
    }
    if (state_ == MatchState::Intermission) {
        round2_started_ = true;
        set_state(MatchState::Round2);
        init_round();
        return true;
    }
    return false;
}

bool TerritorialMatch::remove_player(uint8_t id) {
    auto it = std::find_if(players_.begin(), players_.end(), [id](const PlayerTA& p) { return p.id == id; });
    if (it == players_.end()) return false;
    const Team t = it->team;
    players_.erase(it);
    const bool active = state_ == MatchState::Preparation || state_ == MatchState::Intermission || in_round();
    if (active && connected_count(t) == 0) {
        abandoned_ = true;
        abandoned_team_ = t;
        if (in_round()) {
            end_round(RoundEndReason::Abandonment);
        } else {
            set_state(MatchState::Results);
        }
    }
    return true;
}

bool TerritorialMatch::kill_player(uint8_t id) {
    if (!in_round()) return false;
    for (auto& p : players_) {
        if (p.id == id) {
            if (!p.alive) return false;
            p.alive = false;
            p.in_zone = false;
            return true;
        }
    }
    return false;
}

bool TerritorialMatch::set_in_zone(uint8_t id, bool in_zone) {
    if (!in_round()) return false;
    for (auto& p : players_) {
        if (p.id == id) {
            if (!p.alive) return false;
            p.in_zone = in_zone;
            return true;
        }
    }
    return false;
}

ObjectiveState TerritorialMatch::objective_state(uint8_t i) const {
    if (i >= cfg_.objective_count) return ObjectiveState::Disabled;
    if (i < captured_) return ObjectiveState::Captured;
    if (!in_round()) {
        return (state_ == MatchState::Intermission || state_ == MatchState::Results) ? ObjectiveState::Disabled
                                                                                    : ObjectiveState::Locked;
    }
    if (i > objective_) return ObjectiveState::Locked;
    if (capturing_) return ObjectiveState::Capturing;
    if (contested_) return ObjectiveState::Contested;
    return ObjectiveState::Available;
}

uint16_t TerritorialMatch::progress_permille() const {
    const uint32_t p = uint32_t(uint64_t(progress_ticks_) * 1000u / cfg_.capture_ticks);
    return static_cast<uint16_t>(std::min<uint32_t>(p, 999));
}

void TerritorialMatch::update_capture() {
    const Team atk = attacker_team();
    const uint32_t a = zone_count(atk);
    const uint32_t d = zone_count(other(atk));
    capturing_ = contested_ = false;
    if (a > d) {
        capturing_ = true;
        ++progress_ticks_;
        if (progress_ticks_ >= cfg_.capture_ticks) {
            ++captured_;
            push(EventType::ObjectiveCaptured, atk, objective_, captured_);
            ++objective_;
            progress_ticks_ = 0;
            capturing_ = false;
            atk_waves_ = static_cast<uint16_t>(
                std::min<uint32_t>(cfg_.max_waves, uint32_t(atk_waves_) + cfg_.waves_per_objective));
            uint32_t rem = std::min<uint64_t>(uint64_t(time_remaining_) + cfg_.bonus_time_ticks, cfg_.max_time_ticks);
            if (time_cap_total_ > 0) {
                const uint32_t room = time_cap_total_ > elapsed_ ? time_cap_total_ - elapsed_ : 0;
                rem = std::min(rem, room);
            }
            time_remaining_ = rem;
            if (time_remaining_ > 0) overtime_ = false;
            for (auto& p : players_) p.in_zone = false;  // o host recalcula para a nova zona
            if (captured_ >= cfg_.objective_count) end_round(RoundEndReason::AllCaptured);
        }
    } else if (a > 0) {
        contested_ = true;
    }
}

void TerritorialMatch::update_waves(Team t) {
    const uint32_t dead = dead_count(t);
    const int i = idx(t);
    if (dead == 0) {
        wave_active_[i] = false;
        return;
    }
    if (!wave_active_[i]) {
        wave_active_[i] = true;
        wave_timer_[i] = cfg_.wave_interval_ticks;
    }
    if (wave_timer_[i] > 0) --wave_timer_[i];
    if (wave_timer_[i] > 0) return;
    uint16_t& w = (t == attacker_team()) ? atk_waves_ : def_waves_;
    if (w == 0) return;  // sem reforços: mortos não retornam (timer fica pronto)
    --w;
    uint16_t revived = 0;
    for (auto& p : players_) {
        if (p.team == t && !p.alive) {
            p.alive = true;
            p.in_zone = false;
            ++revived;
        }
    }
    wave_active_[i] = false;
    push(EventType::WaveSpawned, t, 0, revived);
}

RoundResult TerritorialMatch::snapshot(RoundEndReason reason) const {
    RoundResult r;
    r.attacker = attacker_team();
    r.captured = captured_;
    r.last_objective = static_cast<int8_t>(static_cast<int>(captured_) - 1);
    r.completed = captured_ >= cfg_.objective_count;
    r.progress_permille = r.completed ? 0 : progress_permille();
    r.elapsed_ticks = elapsed_;
    r.completion_ticks = r.completed ? elapsed_ : 0;
    r.attacker_waves_left = atk_waves_;
    r.defender_waves_left = def_waves_;
    r.reason = reason;
    return r;
}

void TerritorialMatch::end_round(RoundEndReason reason) {
    const int ri = (state_ == MatchState::Round2) ? 1 : 0;
    results_[ri] = snapshot(reason);
    push(EventType::RoundEnded, attacker_team(), static_cast<uint8_t>(reason), captured_);
    if (state_ == MatchState::Round1 && !abandoned_) {
        set_state(MatchState::Intermission);
    } else {
        set_state(MatchState::Results);
    }
}

bool TerritorialMatch::check_end() {
    if (state_ == MatchState::Round2 && cfg_.round2_early_end && captured_ > results_[0].captured) {
        end_round(RoundEndReason::OutcomeDecided);
        return true;
    }
    const Team atk = attacker_team();
    const bool atk_out = alive_count(atk) == 0 && atk_waves_ == 0;
    const bool def_out = alive_count(other(atk)) == 0 && def_waves_ == 0;
    if (atk_out) {  // inclui morte simultânea: atacantes falharam, defensores vencem
        end_round(RoundEndReason::AttackersEliminated);
        return true;
    }
    if (def_out) {
        end_round(RoundEndReason::DefendersEliminated);
        return true;
    }
    if (time_remaining_ == 0) {
        const bool dispute = cfg_.overtime_enabled && time_cap_total_ == 0 && zone_count(atk) > 0;
        if (!dispute) {
            end_round(RoundEndReason::TimeExpired);
            return true;
        }
        if (!overtime_) {
            overtime_ = true;
            overtime_ticks_ = 0;
            push(EventType::OvertimeStarted, atk, objective_, 0);
        }
        if (++overtime_ticks_ > cfg_.overtime_cap_ticks) {
            end_round(RoundEndReason::OvertimeCap);
            return true;
        }
    }
    return false;
}

void TerritorialMatch::tick() {
    if (!in_round()) return;
    ++elapsed_;
    if (!overtime_ && time_remaining_ > 0) --time_remaining_;
    update_capture();
    if (!in_round()) return;
    update_waves(Team::A);
    update_waves(Team::B);
    check_end();
}

int TerritorialMatch::winner() const {
    if (abandoned_) return idx(other(abandoned_team_));
    if (state_ != MatchState::Results) return -1;
    const int c = compare_results(results_[0], results_[1]);
    if (c > 0) return 0;
    if (c < 0) return 1;
    return -1;
}

}  // namespace netcore::ta
