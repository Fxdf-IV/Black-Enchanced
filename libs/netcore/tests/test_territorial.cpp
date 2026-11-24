// Testes unitários da lógica pura do Territorial Assault (libs/netcore/territorial.hpp).
// Relógio injetado: todo o tempo é medido em chamadas de tick() (30 Hz no host).
#include <cstdio>
#include <cstdlib>

#include "netcore/territorial.hpp"

using namespace netcore::ta;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            std::exit(1);                                                             \
        }                                                                             \
    } while (0)

namespace {

Config small_cfg() {
    Config c;
    c.objective_count = 2;
    c.capture_ticks = 10;
    c.attacker_initial_waves = 2;
    c.defender_initial_waves = 3;
    c.wave_interval_ticks = 5;
    c.waves_per_objective = 5;
    c.max_waves = 6;
    c.initial_time_ticks = 100;
    c.bonus_time_ticks = 50;
    c.max_time_ticks = 120;
    c.overtime_cap_ticks = 20;
    return c;
}

// A=0,1 ; B=2,3. Round1: A ataca.
TerritorialMatch make_match(const Config& c) {
    TerritorialMatch m(c);
    CHECK(m.add_player(0, Team::A));
    CHECK(m.add_player(1, Team::A));
    CHECK(m.add_player(2, Team::B));
    CHECK(m.add_player(3, Team::B));
    CHECK(m.start_match());
    CHECK(m.begin_round());
    return m;
}

void step(TerritorialMatch& m, int n) {
    for (int i = 0; i < n; ++i) m.tick();
}

void test_capture_table() {
    TerritorialMatch m = make_match(small_cfg());
    CHECK(m.state() == MatchState::Round1);
    CHECK(m.attacker_team() == Team::A);
    CHECK(m.objective_state(0) == ObjectiveState::Available);
    CHECK(m.objective_state(1) == ObjectiveState::Locked);

    m.set_in_zone(0, true);  // 1x0 progride
    step(m, 3);
    CHECK(m.objective_state(0) == ObjectiveState::Capturing);
    CHECK(m.progress_permille() == 300);

    m.set_in_zone(1, true);  // 2x0: mesma velocidade
    step(m, 2);
    CHECK(m.progress_permille() == 500);

    m.set_in_zone(2, true);  // 2x1 progride
    step(m, 1);
    CHECK(m.progress_permille() == 600);
    m.set_in_zone(3, true);  // 2x2 bloqueada
    step(m, 5);
    CHECK(m.objective_state(0) == ObjectiveState::Contested);
    CHECK(m.progress_permille() == 600);

    m.set_in_zone(1, false);  // 1x2 bloqueada
    step(m, 2);
    CHECK(m.progress_permille() == 600);

    m.set_in_zone(0, false);  // 0xN: parada, progresso preservado
    step(m, 2);
    CHECK(m.objective_state(0) == ObjectiveState::Available);
    CHECK(m.progress_permille() == 600);
}

void test_capture_completes_once_and_unlocks() {
    Config c = small_cfg();
    TerritorialMatch m = make_match(c);
    m.set_in_zone(0, true);
    m.drain_events();
    step(m, 10);
    CHECK(m.captured_count() == 1);
    CHECK(m.current_objective() == 1);
    CHECK(m.objective_state(0) == ObjectiveState::Captured);
    CHECK(m.objective_state(1) == ObjectiveState::Available);
    CHECK(m.waves(Team::A) == 6);          // 2 + 5 limitado a max_waves
    CHECK(m.time_remaining_ticks() == 120);  // 90 + 50 limitado ao teto
    int captured_events = 0;
    for (const auto& e : m.drain_events())
        if (e.type == EventType::ObjectiveCaptured) ++captured_events;
    CHECK(captured_events == 1);
    CHECK(!m.find_player(0)->in_zone);  // host precisa reinformar a ocupação
    step(m, 5);                          // ninguém na zona: nada acontece
    CHECK(m.captured_count() == 1);

    m.set_in_zone(0, true);
    step(m, 10);
    CHECK(m.state() == MatchState::Intermission);
    CHECK(m.round_result(1).completed);
    CHECK(m.round_result(1).reason == RoundEndReason::AllCaptured);
    CHECK(m.round_result(1).completion_ticks == 25);
}

void test_waves() {
    TerritorialMatch m = make_match(small_cfg());
    step(m, 20);  // ninguém morreu: onda não é consumida
    CHECK(m.waves(Team::B) == 3);

    CHECK(m.kill_player(2));
    CHECK(!m.kill_player(2));
    step(m, 4);
    CHECK(!m.find_player(2)->alive);
    step(m, 1);  // onda coletiva no 5º tick após a primeira morte
    CHECK(m.find_player(2)->alive);
    CHECK(m.waves(Team::B) == 2);

    // Jogadores vivos não são afetados: matar um e ver o outro vivo continuar
    CHECK(m.kill_player(3));
    step(m, 5);
    CHECK(m.find_player(3)->alive);
    CHECK(m.waves(Team::B) == 1);
}

void test_no_waves_means_no_return_until_reward() {
    Config c = small_cfg();
    c.attacker_initial_waves = 0;
    TerritorialMatch m = make_match(c);
    CHECK(m.kill_player(1));
    step(m, 30);
    CHECK(!m.find_player(1)->alive);  // sem ondas, não volta
    CHECK(m.state() == MatchState::Round1);  // jogador 0 ainda vivo: rodada continua
    m.set_in_zone(0, true);
    step(m, 10);  // captura concede ondas
    CHECK(m.captured_count() == 1);
    step(m, 2);
    CHECK(m.find_player(1)->alive);
}

void test_elimination_rules() {
    Config c = small_cfg();
    c.attacker_initial_waves = 0;
    TerritorialMatch m = make_match(c);
    m.kill_player(0);
    m.kill_player(1);
    m.tick();
    CHECK(m.state() == MatchState::Intermission);
    CHECK(m.round_result(1).reason == RoundEndReason::AttackersEliminated);

    // Morte simultânea sem reforços: atacantes falham
    Config d = small_cfg();
    d.attacker_initial_waves = 0;
    d.defender_initial_waves = 0;
    TerritorialMatch s = make_match(d);
    for (uint8_t i = 0; i < 4; ++i) s.kill_player(i);
    s.tick();
    CHECK(s.round_result(1).reason == RoundEndReason::AttackersEliminated);

    // Defensores sem reforços e todos mortos: atacantes vencem a rodada
    Config e = small_cfg();
    e.defender_initial_waves = 0;
    TerritorialMatch t = make_match(e);
    t.kill_player(2);
    t.kill_player(3);
    t.tick();
    CHECK(t.round_result(1).reason == RoundEndReason::DefendersEliminated);
}

void test_time_and_overtime() {
    Config c = small_cfg();
    c.initial_time_ticks = 5;
    TerritorialMatch m = make_match(c);
    step(m, 5);
    CHECK(m.state() == MatchState::Intermission);
    CHECK(m.round_result(1).reason == RoundEndReason::TimeExpired);

    // Overtime com atacante na zona; termina ao sair
    TerritorialMatch o = make_match(c);
    o.set_in_zone(0, true);
    step(o, 4);  // progresso 4/10
    step(o, 1);  // tempo zera -> overtime
    CHECK(o.in_overtime());
    step(o, 3);
    CHECK(o.state() == MatchState::Round1);
    o.set_in_zone(0, false);
    o.tick();
    CHECK(o.state() == MatchState::Intermission);
    CHECK(o.round_result(1).reason == RoundEndReason::TimeExpired);
    CHECK(o.round_result(1).progress_permille == 800);

    // Captura em overtime concede bônus e sai do overtime
    TerritorialMatch p = make_match(c);
    p.set_in_zone(0, true);
    step(p, 5);
    CHECK(p.in_overtime());
    step(p, 5);
    CHECK(p.captured_count() == 1);
    CHECK(!p.in_overtime());
    CHECK(p.time_remaining_ticks() > 0);

    // Teto de overtime evita rodada travada (disputa 1x1 eterna)
    TerritorialMatch q = make_match(c);
    q.set_in_zone(0, true);
    q.set_in_zone(2, true);
    step(q, 5 + 25);
    CHECK(q.state() == MatchState::Intermission);
    CHECK(q.round_result(1).reason == RoundEndReason::OvertimeCap);
}

void test_full_match_and_winner() {
    Config c = small_cfg();
    TerritorialMatch m = make_match(c);
    // Rodada 1: A captura 1 objetivo e perde no tempo
    m.set_in_zone(0, true);
    step(m, 10);
    CHECK(m.captured_count() == 1);
    m.set_in_zone(0, false);
    step(m, 200);
    CHECK(m.state() == MatchState::Intermission);
    CHECK(m.round_result(1).captured == 1);

    // Intervalo: funções invertidas
    CHECK(m.begin_round());
    CHECK(m.state() == MatchState::Round2);
    CHECK(m.attacker_team() == Team::B);
    CHECK(m.role_of(Team::A) == Role::Defense);
    CHECK(m.captured_count() == 0);
    CHECK(m.find_player(0)->alive);

    // Rodada 2: B captura 2 e supera A (fim antecipado ao ultrapassar, no 2º objetivo -> completa)
    m.set_in_zone(2, true);
    step(m, 10);
    CHECK(m.captured_count() == 1);
    m.set_in_zone(2, true);
    step(m, 10);
    CHECK(m.state() == MatchState::Results);
    CHECK(m.winner() == 1);
    CHECK(m.has_winner());
}

void test_early_end_and_time_to_beat() {
    Config c = small_cfg();
    TerritorialMatch m = make_match(c);
    m.set_in_zone(0, true);
    step(m, 10);
    m.set_in_zone(0, true);
    step(m, 10);
    CHECK(m.state() == MatchState::Intermission);
    CHECK(m.round_result(1).completion_ticks == 20);

    m.begin_round();
    // Tempo a bater = 20 ticks; B captura só 1 e esgota o tempo: A vence
    m.set_in_zone(2, true);
    step(m, 10);
    CHECK(m.captured_count() == 1);
    m.set_in_zone(2, false);
    step(m, 20);
    CHECK(m.state() == MatchState::Results);
    CHECK(m.round_result(2).reason == RoundEndReason::TimeExpired);
    CHECK(m.winner() == 0);
}

void test_tie_break_hierarchy() {
    RoundResult a, b;
    a.captured = 2;
    b.captured = 1;
    CHECK(compare_results(a, b) == 1);
    CHECK(compare_results(b, a) == -1);
    a.captured = b.captured = 1;
    a.progress_permille = 400;
    b.progress_permille = 300;
    CHECK(compare_results(a, b) == 1);
    b.progress_permille = 400;
    CHECK(compare_results(a, b) == 0);
    a.completed = b.completed = true;
    a.completion_ticks = 100;
    b.completion_ticks = 90;
    CHECK(compare_results(a, b) == -1);
    b.completion_ticks = 100;
    CHECK(compare_results(a, b) == 0);
}

void test_abandonment_and_validation() {
    TerritorialMatch m = make_match(small_cfg());
    CHECK(!m.add_player(9, Team::A));  // fora do lobby
    CHECK(!m.remove_player(77));
    m.remove_player(2);
    CHECK(m.state() == MatchState::Round1);
    m.remove_player(3);  // equipe B inteira desconectada
    CHECK(m.state() == MatchState::Results);
    CHECK(m.round_result(1).reason == RoundEndReason::Abandonment);
    CHECK(m.winner() == 0);

    TerritorialMatch n;
    CHECK(!n.start_match());  // sem jogadores
    CHECK(n.add_player(1, Team::A));
    CHECK(!n.add_player(1, Team::B));  // id duplicado
    CHECK(!n.start_match());           // equipe B vazia
    CHECK(!n.begin_round());
    CHECK(!n.kill_player(1));          // fora de rodada
    CHECK(!n.set_in_zone(1, true));
    for (uint8_t i = 2; i < 40; ++i) n.add_player(i, Team::B);
    CHECK(n.players().size() <= 16);
}

void test_loadout_budget() {
    Config c;
    Loadout l;
    CHECK(!validate_loadout(l, c));  // sem primária
    l.has_primary = true;
    l.primary_cost = 6;
    l.secondary_cost = 2;
    l.accessories_cost = 3;
    l.explosives_cost = 2;
    l.tactical_cost = 1;
    l.protection_cost = 1;
    CHECK(validate_loadout(l, c));  // exatamente 15
    l.protection_cost = 2;
    CHECK(!validate_loadout(l, c));
    l = Loadout{};
    l.has_primary = true;
    l.primary_cost = 65535;
    l.secondary_cost = 65535;
    l.accessories_cost = 65535;
    CHECK(!validate_loadout(l, c));  // sem estouro de soma
}

void test_determinism() {
    auto run = [] {
        TerritorialMatch m = make_match(small_cfg());
        std::vector<Event> all;
        m.set_in_zone(0, true);
        for (int i = 0; i < 40; ++i) {
            if (i == 7) m.kill_player(2);
            if (i == 12) m.set_in_zone(1, true);
            m.tick();
            auto ev = m.drain_events();
            all.insert(all.end(), ev.begin(), ev.end());
        }
        return all;
    };
    auto x = run();
    auto y = run();
    CHECK(x.size() == y.size());
    for (size_t i = 0; i < x.size(); ++i) {
        CHECK(x[i].type == y[i].type && x[i].team == y[i].team && x[i].index == y[i].index &&
              x[i].count == y[i].count);
    }
}

}  // namespace

int main() {
    test_capture_table();
    test_capture_completes_once_and_unlocks();
    test_waves();
    test_no_waves_means_no_return_until_reward();
    test_elimination_rules();
    test_time_and_overtime();
    test_full_match_and_winner();
    test_early_end_and_time_to_beat();
    test_tie_break_hierarchy();
    test_abandonment_and_validation();
    test_loadout_budget();
    test_determinism();
    std::puts("[territorial_tests] All Territorial Assault logic tests PASSED.");
    return 0;
}
