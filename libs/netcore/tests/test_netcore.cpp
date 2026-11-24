#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <vector>

#include "netcore/buffer.hpp"
#include "netcore/limits.hpp"
#include "netcore/protocol.hpp"
#include "netcore/rules.hpp"
#include "netcore/session.hpp"
#include "netcore/transport.hpp"

using namespace netcore;

#define CHECK(cond)                                                                                    \
    do {                                                                                               \
        if (!(cond)) {                                                                                 \
            std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            std::abort();                                                                              \
        }                                                                                              \
    } while (0)

namespace {

void check_roundtrip(const Message& m) {
    auto encoded = encode(m);
    CHECK(!encoded.empty());
    auto res = decode(encoded);
    CHECK(res.error == DecodeError::None);
    CHECK(res.message.has_value());
    CHECK(*res.message == m);
}

void test_protocol_roundtrip_all_messages() {
    check_roundtrip(Hello{.protocol_version = kProtocolVersion, .rules_hash = 0x12345678u, .name = "Kellar"});
    check_roundtrip(Welcome{
        .player_id = 1,
        .max_players = 4,
        .tick_rate_hz = 30,
        .snapshot_rate_hz = 15,
        .game_mode = 1,
        .arena_id = 3,
        .available_weapon_mask = 0x0000FFFFu,
    });
    check_roundtrip(Reject{.reason = RejectReason::RoomFull});
    check_roundtrip(MatchStart{
        .start_tick = 10,
        .time_limit_s = 600,
        .kill_limit = 15,
        .arena_id = 2,
        .game_mode = 1,
        .available_weapon_mask = 0x0000F0FFu,
    });
    check_roundtrip(MatchEnd{.winner = 2});
    check_roundtrip(MatchEnd{.winner = kNoPlayer});
    check_roundtrip(PlayerState{
        .sequence = 42,
        .client_tick = 120,
        .position = Vec3{10.5f, -2.0f, 33.25f},
        .yaw = 1.25f,
        .pitch = -0.35f,
        .velocity = Vec3{4.0f, 0.0f, -1.5f},
        .anim_state = 3,
        .weapon = 7,
        .flags = kFlagAlive | kFlagAiming,
    });
    check_roundtrip(WorldSnapshot{
        .tick = 300,
        .time_left_s = 590,
        .phase = MatchPhase::Running,
        .players =
            {
                SnapshotPlayer{
                    .player_id = 0,
                    .position = Vec3{0.0f, 0.0f, 0.0f},
                    .yaw = 0.0f,
                    .pitch = 0.0f,
                    .velocity = Vec3{0.0f, 0.0f, 0.0f},
                    .anim_state = 0,
                    .weapon = 8,
                    .health = 100,
                    .flags = kFlagAlive,
                    .kills = 3,
                    .deaths = 1,
                },
                SnapshotPlayer{
                    .player_id = 1,
                    .position = Vec3{12.0f, 1.0f, -8.0f},
                    .yaw = 3.0f,
                    .pitch = 0.1f,
                    .velocity = Vec3{1.0f, 0.0f, 2.0f},
                    .anim_state = 2,
                    .weapon = 7,
                    .health = 66,
                    .flags = kFlagAlive | kFlagCrouched,
                    .kills = 1,
                    .deaths = 3,
                },
            },
    });
    check_roundtrip(FireEvent{
        .client_tick = 301,
        .shooter = 1,
        .weapon = 7,
        .origin = Vec3{12.0f, 1.5f, -8.0f},
        .direction = Vec3{0.0f, 0.0f, 1.0f},
        .claimed_target = 0,
    });
    check_roundtrip(DamageEvent{.attacker = 1, .target = 0, .amount = 34, .health_after = 66});
    check_roundtrip(DeathEvent{.victim = 0, .killer = 1, .weapon = 7});
    check_roundtrip(RespawnEvent{.player = 0, .spawn_point = 2, .position = Vec3{0.0f, 0.0f, 32.0f}, .yaw = -1.57f});
    check_roundtrip(Ping{.nonce = 99, .sent_ms = 123456});
    check_roundtrip(Pong{.nonce = 99, .echo_ms = 123456, .host_tick = 305});
    check_roundtrip(Leave{.reason = 0});
    check_roundtrip(Kick{.reason = KickReason::RateLimited});
    check_roundtrip(LoadoutRequest{
        .primary_weapon = 7,
        .secondary_weapon = 1,
        .tactical_flags = kLoadoutArmor | kLoadoutTactical,
    });
    check_roundtrip(LoadoutRequest{.primary_weapon = kNoWeapon, .secondary_weapon = 0, .tactical_flags = 0});
    check_roundtrip(LoadoutState{
        .player_id = 1,
        .primary_weapon = 8,
        .secondary_weapon = 2,
        .tactical_flags = kLoadoutArmor,
        .last_result = 0,
        .credits = 2450,
        .max_credits = 8000,
        .kit_cost = 4250,
        .buy_ticks_left = 300,
    });
    check_roundtrip(PlayerRoster{
        .players =
            {
                PlayerRosterEntry{.player_id = 0, .name = "HostAlpha"},
                PlayerRosterEntry{.player_id = 1, .name = "ClientBravo"},
            },
    });
    check_roundtrip(BlastEvent{
        .client_tick = 420,
        .attacker = 1,
        .target = 0,
        .weapon = 15,
        .flags = kBlastFlagClaim,
        .origin = Vec3{4.0f, 1.0f, -6.0f},
        .damage = 95.0f,
    });
    check_roundtrip(BlastEvent{
        .client_tick = 421,
        .attacker = kNoPlayer,
        .target = kNoPlayer,
        .weapon = kNoWeapon,
        .flags = kBlastFlagVisualOnly,
        .origin = Vec3{-10.0f, 0.0f, 18.0f},
        .damage = 80.0f,
    });
    check_roundtrip(TeamSelectRequest{.team = 0});
    check_roundtrip(TeamSelectRequest{.team = 1});
    check_roundtrip(TerritorialState{
        .tick = 1234,
        .match_state = 2,
        .attacker_team = 0,
        .current_objective = 1,
        .objective_count = 3,
        .captured_count = 1,
        .current_obj_state = 2,
        .progress_permille = 450,
        .time_left_s = 420,
        .overtime = 0,
        .attacker_waves = 8,
        .defender_waves = 14,
        .winner_team = kNoPlayer,
        .player_teams = {0, 1, 0, kNoPlayer},
        .r1_captured = 1,
        .r1_progress_permille = 450,
        .r1_time_s = 180,
        .r2_captured = 0,
        .r2_progress_permille = 0,
        .r2_time_s = 0,
    });
    check_roundtrip(TerritorialEvent{.event_type = 1, .team = 0, .index = 0, .count = 1});
    check_roundtrip(TerritorialEvent{.event_type = 2, .team = 1, .index = 0, .count = 2});
}

void test_hostile_and_malformed_inputs() {
    // 1. Buffer vazio ou curto demais
    CHECK(decode({}).error == DecodeError::TooShort);
    const uint8_t one_byte[1] = {0xAC};
    CHECK(decode(one_byte).error == DecodeError::TooShort);

    // 2. Buffer acima de kMaxPacketSize
    std::vector<uint8_t> huge(kMaxPacketSize + 1, 0);
    CHECK(decode(huge).error == DecodeError::TooLarge);

    // 3. Magic inválido
    const uint8_t bad_magic[4] = {0x00, 0x00, 0x01, 0x00};
    CHECK(decode(bad_magic).error == DecodeError::BadMagic);

    // 4. Tipo de mensagem desconhecido
    const uint8_t bad_type[4] = {uint8_t(kPacketMagic), uint8_t(kPacketMagic >> 8), 99, 0};
    CHECK(decode(bad_type).error == DecodeError::UnknownType);

    // 5. NaN / Inf / fora de limite em coordenadas
    PlayerState nan_ps{};
    nan_ps.position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK(encode(nan_ps).empty());

    PlayerState oob_ps{};
    oob_ps.position.x = kWorldLimit * 2.0f;
    CHECK(encode(oob_ps).empty());

    // 6. Auto-tiro inválido (shooter == claimed_target)
    FireEvent self_fire{
        .client_tick = 1,
        .shooter = 0,
        .weapon = 0,
        .origin = Vec3{0, 0, 0},
        .direction = Vec3{1, 0, 0},
        .claimed_target = 0,
    };
    CHECK(encode(self_fire).empty());

    // 7. Snapshot com IDs duplicados de jogador
    WorldSnapshot dup_snap{};
    SnapshotPlayer sp{};
    sp.player_id = 1;
    dup_snap.players.push_back(sp);
    dup_snap.players.push_back(sp);
    CHECK(encode(dup_snap).empty());

    // 8. Truncamento e bytes sobrando para cada mensagem válida
    auto valid_bytes = encode(PlayerState{
        .sequence = 1,
        .client_tick = 1,
        .position = Vec3{1, 2, 3},
        .yaw = 0.5f,
        .pitch = 0.1f,
        .velocity = Vec3{0, 0, 0},
        .anim_state = 1,
        .weapon = 2,
        .flags = kFlagAlive,
    });
    for (std::size_t cut = 3; cut < valid_bytes.size(); ++cut) {
        auto r = decode(std::span<const uint8_t>(valid_bytes.data(), cut));
        CHECK(!r.message.has_value());
        CHECK(r.error == DecodeError::Truncated || r.error == DecodeError::InvalidValue);
    }
    auto with_trailing = valid_bytes;
    with_trailing.push_back(0x00);
    CHECK(decode(with_trailing).error == DecodeError::TrailingBytes);

    // 8b. Loadout com flags inválidos ou arma fora de faixa deve ser rejeitado
    CHECK(!is_valid(Message{LoadoutRequest{.primary_weapon = kMaxWeapons, .secondary_weapon = 0, .tactical_flags = 0}}));
    CHECK(!is_valid(Message{LoadoutRequest{.primary_weapon = 7, .secondary_weapon = 0, .tactical_flags = 0x80}}));
    CHECK(!is_valid(Message{LoadoutState{.player_id = kMaxPlayers, .primary_weapon = 7, .secondary_weapon = 0, .tactical_flags = 0, .last_result = 0}}));
    CHECK(!is_valid(Message{PlayerRoster{.players = {PlayerRosterEntry{.player_id = 0, .name = "A"}, PlayerRosterEntry{.player_id = 0, .name = "B"}}}}));
    CHECK(!is_valid(Message{PlayerRoster{.players = {PlayerRosterEntry{.player_id = kMaxPlayers, .name = "Bad"}}}}));
    CHECK(!is_valid(Message{BlastEvent{.attacker = 0, .target = 0, .weapon = 15, .flags = kBlastFlagClaim, .origin = Vec3{0, 0, 0}, .damage = 251.0f}}));
    CHECK(!is_valid(Message{BlastEvent{.attacker = 0, .target = 0, .weapon = 15, .flags = kBlastFlagClaim, .origin = Vec3{0, 0, 0}, .damage = std::numeric_limits<float>::quiet_NaN()}}));
    CHECK(!is_valid(Message{BlastEvent{.attacker = 0, .target = 0, .weapon = 15, .flags = 0x80, .origin = Vec3{0, 0, 0}, .damage = 50.0f}}));
    CHECK(!is_valid(Message{BlastEvent{.attacker = 0, .target = 0, .weapon = kMaxWeapons, .flags = kBlastFlagClaim, .origin = Vec3{kWorldLimit + 10.0f, 0, 0}, .damage = 50.0f}}));
    CHECK(!is_valid(Message{Welcome{.player_id = 0, .max_players = 4, .tick_rate_hz = 30, .snapshot_rate_hz = 15, .game_mode = 9}}));
    CHECK(!is_valid(Message{TeamSelectRequest{.team = 2}}));
    CHECK(!is_valid(Message{TerritorialState{.match_state = 6, .objective_count = 2}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 0}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 17}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 2, .captured_count = 3}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 2, .progress_permille = 1001}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 2, .overtime = 2}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 2, .winner_team = 3}}));
    CHECK(!is_valid(Message{TerritorialState{.objective_count = 2, .player_teams = {0, 2, kNoPlayer, kNoPlayer}}}));
    CHECK(!is_valid(Message{TerritorialEvent{.event_type = 5, .team = 0, .index = 0, .count = 0}}));
    CHECK(!is_valid(Message{TerritorialEvent{.event_type = 1, .team = 2, .index = 0, .count = 0}}));
    CHECK(!is_valid(Message{TerritorialEvent{.event_type = 1, .team = 0, .index = 17, .count = 0}}));

    // Truncamento e trailing bytes nas novas mensagens Territorial Assault
    {
        auto raw_ts = encode(TerritorialState{.tick = 5, .objective_count = 2});
        CHECK(!raw_ts.empty());
        CHECK(decode(std::span<const uint8_t>(raw_ts.data(), raw_ts.size() - 1)).error == DecodeError::Truncated);
        raw_ts.push_back(0x00);
        CHECK(decode(raw_ts).error == DecodeError::TrailingBytes);
    }

    // 9. Fuzz determinístico (10.000 pacotes pseudoaleatórios nunca podem causar falha ou leitura inválida)
    std::mt19937 rng(0xB1AC2006u);
    for (int i = 0; i < 10000; ++i) {
        const std::size_t len = rng() % (kMaxPacketSize + 32);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);
        if (len >= 3 && (rng() % 2 == 0)) {
            buf[0] = uint8_t(kPacketMagic);
            buf[1] = uint8_t(kPacketMagic >> 8);
            buf[2] = static_cast<uint8_t>(1 + (rng() % 22));
        }
        auto res = decode(buf);
        if (res.message.has_value()) {
            CHECK(is_valid(*res.message));
        }
    }
}

void test_four_player_deathmatch_simulation() {
    auto rules = RulesConfig::default_deathmatch();
    rules.kill_limit = 2;  // 2 eliminações para encerrar a partida no teste
    rules.respawn_delay_ticks = 6;

    LoopbackEndpoint host_ep;
    LoopbackEndpoint c1_ep, c2_ep, c3_ep, c4_ep;

    auto [h_to_c1, c1_to_h] = LoopbackEndpoint::link(host_ep, c1_ep);
    auto [h_to_c2, c2_to_h] = LoopbackEndpoint::link(host_ep, c2_ep);
    auto [h_to_c3, c3_to_h] = LoopbackEndpoint::link(host_ep, c3_ep);
    auto [h_to_c4, c4_to_h] = LoopbackEndpoint::link(host_ep, c4_ep);
    (void)h_to_c1;
    (void)h_to_c2;
    (void)h_to_c3;
    (void)h_to_c4;

    HostSession host(host_ep, rules, "HostKellar");
    ClientSession c1(c1_ep, c1_to_h, rules, "Blackwood");
    ClientSession c2(c2_ep, c2_to_h, rules, "MacCarver");
    ClientSession c3(c3_ep, c3_to_h, rules, "Lennox");
    ClientSession c4(c4_ep, c4_to_h, rules, "FifthWheel");

    CHECK(c1.send_hello());
    CHECK(c2.send_hello());
    CHECK(c3.send_hello());
    CHECK(c4.send_hello());

    host.tick();
    c1.poll();
    c2.poll();
    c3.poll();
    c4.poll();

    // Até 4 jogadores (Host + 3 clientes); o 5º deve ser recusado com RoomFull
    CHECK(c1.is_joined() && c1.local_player_id() == 1);
    CHECK(c2.is_joined() && c2.local_player_id() == 2);
    CHECK(c3.is_joined() && c3.local_player_id() == 3);
    CHECK(!c4.is_joined() && c4.is_rejected() && c4.reject_reason() == RejectReason::RoomFull);
    CHECK(host.player_count() == 4);

    // Inicia a partida
    host.start_match();
    host.tick();
    c1.poll();
    c2.poll();
    c3.poll();
    CHECK(c1.phase() == MatchPhase::Running);

    // Cliente 1 move e dispara no Host (jogador 0) com WA2000 (arma 13, dano 95) duas vezes para abater
    const Vec3 c1_pos = host.slots()[1].position;
    CHECK(c1.send_local_state(PlayerState{
        .sequence = 0,
        .client_tick = host.current_tick(),
        .position = c1_pos,
        .yaw = 0.0f,
        .pitch = 0.0f,
        .velocity = Vec3{0, 0, 0},
        .anim_state = 1,
        .weapon = 13,
        .flags = kFlagAlive,
    }));
    CHECK(c1.send_fire(FireEvent{
        .client_tick = host.current_tick(),
        .shooter = 1,
        .weapon = 13,
        .origin = c1_pos,
        .direction = Vec3{1.0f, 0.0f, 0.0f},
        .claimed_target = 0,
    }));

    host.tick();
    c1.poll();
    CHECK(host.slots()[0].health == 5);

    // Avança 20 ticks para respeitar a cadência da WA2000 (min_fire_interval_ticks = 20)
    for (int i = 0; i < 20; ++i) host.tick();
    c1.poll();

    // Segundo tiro abate o jogador 0
    CHECK(c1.send_fire(FireEvent{
        .client_tick = host.current_tick(),
        .shooter = 1,
        .weapon = 13,
        .origin = c1_pos,
        .direction = Vec3{1.0f, 0.0f, 0.0f},
        .claimed_target = 0,
    }));
    host.tick();
    c1.poll();
    CHECK(host.slots()[0].health == 0);
    CHECK((host.slots()[0].flags & kFlagAlive) == 0);
    CHECK(host.slots()[1].kills == 1);
    CHECK(!c1.recent_deaths().empty() && c1.recent_deaths().back().victim == 0);

    // Respawn automático após respawn_delay_ticks (6 ticks)
    for (int i = 0; i < 7; ++i) host.tick();
    c1.poll();
    CHECK((host.slots()[0].flags & kFlagAlive) != 0);
    CHECK(host.slots()[0].health == 100);

    // Abate o jogador 2 com RPG-7 (arma 15, dano 100) para atingir kill_limit = 2 e encerrar a partida
    for (int i = 0; i < 60; ++i) host.tick();
    CHECK(c1.send_fire(FireEvent{
        .client_tick = host.current_tick(),
        .shooter = 1,
        .weapon = 15,
        .origin = c1_pos,
        .direction = Vec3{0.0f, 0.0f, 1.0f},
        .claimed_target = 2,
    }));
    host.tick();
    c1.poll();
    c2.poll();
    c3.poll();

    CHECK(host.phase() == MatchPhase::Ended);
    CHECK(host.winner() == 1);
    CHECK(c1.phase() == MatchPhase::Ended);
    CHECK(c1.winner() == 1);
}

void test_rate_limit_and_malformed_kick() {
    auto rules = RulesConfig::default_deathmatch();
    rules.max_violations_before_kick = 3;

    LoopbackEndpoint host_ep, bad_ep;
    auto [h_to_b, b_to_h] = LoopbackEndpoint::link(host_ep, bad_ep);
    (void)h_to_b;

    HostSession host(host_ep, rules, "Host");
    ClientSession bad_client(bad_ep, b_to_h, rules, "Griefer");
    CHECK(bad_client.send_hello());
    host.tick();
    bad_client.poll();
    CHECK(bad_client.is_joined());

    // Injeta 3 pacotes malformados: o host deve expulsar o cliente e liberar o slot
    const uint8_t garbage[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
    for (int i = 0; i < 3; ++i) {
        bad_ep.inject_raw_to(b_to_h, Channel::Reliable, garbage);
    }
    host.tick();
    bad_client.poll();

    CHECK(bad_client.is_kicked());
    CHECK(bad_client.kick_reason() == KickReason::Malformed);
    CHECK(host.player_count() == 1);
}

void test_host_apply_damage_and_kill_attribution() {
    auto rules = RulesConfig::default_deathmatch();
    rules.kill_limit = 2;
    rules.respawn_delay_ticks = 4;

    LoopbackEndpoint host_ep, c1_ep;
    auto [h1, p1] = LoopbackEndpoint::link(host_ep, c1_ep);
    (void)h1;

    HostSession host(host_ep, rules, "Host");
    ClientSession c1(c1_ep, p1, rules, "Client1");
    CHECK(c1.send_hello());
    host.tick();
    c1.poll();
    CHECK(c1.is_joined() && c1.local_player_id() == 1);

    // Em Warmup (antes de start_match) não aceita dano autoritativo
    CHECK(!host.apply_damage(0, 1, 25, 7));

    host.start_match();
    c1.poll();
    c1.clear_events();

    // Validações de limites e alvos inválidos
    CHECK(!host.apply_damage(0, 0, 10, 0));              // auto-dano
    CHECK(!host.apply_damage(kMaxPlayers, 1, 10, 0));    // atacante fora da faixa
    CHECK(!host.apply_damage(0, kMaxPlayers, 10, 0));    // vítima fora da faixa
    CHECK(!host.apply_damage(0, 2, 10, 0));              // slot desocupado
    CHECK(!host.apply_damage(0, 1, 0, 0));               // dano zero
    CHECK(!host.apply_damage(0, 1, 10, kMaxWeapons));    // arma inválida

    // Dano não letal de Host (0) para Client1 (1)
    CHECK(host.apply_damage(0, 1, 40, 7));
    host.tick();
    c1.poll();
    CHECK(host.slots()[1].health == 60);
    CHECK((host.slots()[1].flags & kFlagAlive) != 0);
    CHECK(!c1.recent_damages().empty());
    CHECK(c1.recent_damages().back().attacker == 0);
    CHECK(c1.recent_damages().back().target == 1);
    CHECK(c1.recent_damages().back().amount == 40);
    CHECK(c1.recent_damages().back().health_after == 60);
    CHECK(c1.recent_deaths().empty());
    c1.clear_events();

    // Dano letal com atribuição de kill/death e prevenção de duplo abate no mesmo corpo morto
    CHECK(host.apply_damage(0, 1, 75, 7));
    CHECK(!host.apply_damage(0, 1, 10, 7));  // vítima já morta
    host.tick();
    c1.poll();
    CHECK(host.slots()[1].health == 0);
    CHECK((host.slots()[1].flags & kFlagAlive) == 0);
    CHECK(host.slots()[0].kills == 1);
    CHECK(host.slots()[1].deaths == 1);
    CHECK(!c1.recent_deaths().empty());
    CHECK(c1.recent_deaths().back().victim == 1);
    CHECK(c1.recent_deaths().back().killer == 0);
    CHECK(c1.recent_deaths().back().weapon == 7);
    c1.clear_events();

    // Aguarda respawn automático após respawn_delay_ticks e finaliza no kill_limit = 2
    for (int i = 0; i < 5; ++i) host.tick();
    c1.poll();
    CHECK((host.slots()[1].flags & kFlagAlive) != 0);
    CHECK(host.slots()[1].health == 100);

    CHECK(host.apply_damage(0, 1, 100, 15));
    host.tick();
    c1.poll();
    CHECK(host.phase() == MatchPhase::Ended);
    CHECK(host.winner() == 0);
    CHECK(c1.phase() == MatchPhase::Ended);
    CHECK(c1.winner() == 0);

    // Nova sessão para validar resiliência de Hello reenviado, set_spawn_points e raycast de disparo
    HostSession host2(host_ep, rules, "Host2");
    ClientSession c2(c1_ep, p1, rules, "Client2");
    CHECK(c2.send_hello());
    host2.tick();
    c2.poll();
    CHECK(c2.is_joined() && c2.local_player_id() == 1);
    const std::size_t count_before = host2.player_count();

    // Reenvio de Hello pelo mesmo peer mantém mesmo player_id e contagem de jogadores
    CHECK(c2.send_hello());
    host2.tick();
    c2.poll();
    CHECK(c2.is_joined() && c2.local_player_id() == 1);
    CHECK(host2.player_count() == count_before);

    // Atualiza spawns para coordenadas reais da fase sem alterar rules_hash
    host2.set_spawn_points({
        SpawnPoint{Vec3{-10.0f, 0.0f, 50.0f}, 0.0f},
        SpawnPoint{Vec3{-10.0f, 0.0f, 60.0f}, 3.14f},
    });
    host2.start_match();
    c2.poll();

    // Primeiro PlayerState após spawn/teleport não deve disparar kick anti-cheat por deslocamento
    host2.set_local_player_state(PlayerState{
        .sequence = 1,
        .client_tick = host2.current_tick(),
        .position = Vec3{-10.0f, 0.0f, 50.0f},
        .yaw = 0.0f,
        .pitch = 0.0f,
        .velocity = Vec3{0.0f, 0.0f, 0.0f},
        .anim_state = 0,
        .weapon = 0,
        .flags = kFlagAlive,
    });
    CHECK(c2.send_local_state(PlayerState{
        .sequence = 0,
        .client_tick = host2.current_tick(),
        .position = Vec3{-10.0f, 0.0f, 60.0f},
        .yaw = 3.14f,
        .pitch = 0.0f,
        .velocity = Vec3{0.0f, 0.0f, 0.0f},
        .anim_state = 0,
        .weapon = 0,
        .flags = kFlagAlive,
    }));
    host2.tick();
    c2.poll();
    CHECK(c2.is_joined() && !c2.is_kicked());

    // Disparo sem claimed_target (kNoPlayer) alinhado do Host para Client2 acerta via raycast no host
    const uint8_t hp_before = host2.slots()[1].health;
    CHECK(host2.local_fire(FireEvent{
        .client_tick = host2.current_tick(),
        .shooter = 0,
        .weapon = 0,
        .origin = Vec3{-10.0f, 0.0f, 50.0f},
        .direction = Vec3{0.0f, 0.0f, 1.0f},
        .claimed_target = kNoPlayer,
    }));
    CHECK(host2.slots()[1].health < hp_before);

    // Client2 recebe FireEvent do host; HostSession recebe FireEvent de Client2 via recent_fires()
    c2.poll();
    CHECK(c2.recent_fires().size() == 1);
    CHECK(c2.recent_fires()[0].shooter == 0);
    c2.clear_events();
    CHECK(c2.recent_fires().empty());

    host2.clear_events();
    CHECK(c2.send_fire(FireEvent{
        .client_tick = host2.current_tick(),
        .shooter = 1,
        .weapon = 0,
        .origin = Vec3{-10.0f, 0.0f, 60.0f},
        .direction = Vec3{0.0f, 0.0f, -1.0f},
        .claimed_target = 0,
    }));
    host2.tick();
    CHECK(host2.recent_fires().size() == 1);
    CHECK(host2.recent_fires()[0].shooter == 1);
    host2.clear_events();
    CHECK(host2.recent_fires().empty());
}

void test_cs_loadout_economy_and_limits() {
    auto rules = RulesConfig::cs_loadout_deathmatch();
    rules.respawn_delay_ticks = 6;
    rules.buy_window_ticks = 12;

    LoopbackEndpoint host_ep;
    LoopbackEndpoint c1_ep;
    auto [h_to_c1, c1_to_h] = LoopbackEndpoint::link(host_ep, c1_ep);
    (void)h_to_c1;

    HostSession host(host_ep, rules, "HostCS");
    ClientSession c1(c1_ep, c1_to_h, rules, "ClientCS");

    CHECK(c1.send_hello());
    host.tick();
    c1.poll();
    CHECK(c1.is_joined());

    host.start_match();
    host.tick();
    c1.poll();
    CHECK(c1.phase() == MatchPhase::Running);
    CHECK(c1.local_loadout().credits == rules.starting_credits);
    CHECK(c1.local_loadout().primary_weapon == kNoWeapon);
    CHECK(c1.local_loadout().secondary_weapon == 0);

    // 1. Tentativa de comprar AK-47 ($2700) com apenas $1500 deve falhar
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 7, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::InsufficientCredits));
    CHECK(c1.local_loadout().primary_weapon == kNoWeapon);
    CHECK(c1.local_loadout().credits == 1500);

    // 2. Compra válida: MP5 ($1250) -> sobram $250 e arma primária vira MP5 (id 3)
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 3, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));
    CHECK(c1.local_loadout().primary_weapon == 3);
    CHECK(c1.local_loadout().credits == 250);

    // 3. Item inválido (pistola no slot primário) é rejeitado com InvalidItem
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 1, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::InvalidItem));

    // 4. Com enforce_loadout=true, disparar arma não comprada (AK-47 id 7) falha
    const Vec3 c1_pos = host.slots()[1].position;
    const Vec3 host_pos = host.slots()[0].position;
    CHECK(c1.send_fire(FireEvent{
        .client_tick = host.current_tick(),
        .shooter = 1,
        .weapon = 7,
        .origin = c1_pos,
        .direction = Vec3{1.0f, 0.0f, 0.0f},
        .claimed_target = kNoPlayer,
    }));
    host.tick();
    CHECK(host.slots()[1].violations >= 1);

    // 5. Compra válida pelo host: MAC-10 ($1050) + Glock ($0) = $1050 -> restam $450
    CHECK(host.local_buy_loadout(LoadoutRequest{.primary_weapon = 4, .secondary_weapon = 0, .tactical_flags = 0}));
    CHECK(host.slot_loadout_state(0).credits == 450);
    CHECK(host.slot_loadout_state(0).primary_weapon == 4);

    // 6. Fechar janela de compra após 12 ticks bloqueia compras novas até morrer/respawnar
    for (int i = 0; i < 15; ++i) {
        host.tick();
    }
    c1.poll();
    CHECK(!host.local_buy_loadout(LoadoutRequest{.primary_weapon = 4, .secondary_weapon = 0, .tactical_flags = kLoadoutTactical}));
    CHECK(host.slot_loadout_state(0).last_result == static_cast<uint8_t>(LoadoutResult::BuyWindowClosed));

    // 7. Disparo válido da MAC-10 do host abatendo Client1 concede +$450 ao host e +$600 bônus de morte ao Client1
    CHECK(host.local_fire(FireEvent{
        .client_tick = host.current_tick(),
        .shooter = 0,
        .weapon = 4,
        .origin = host_pos,
        .direction = Vec3{1.0f, 0.0f, 0.0f},
        .claimed_target = 1,
    }));
    // Dano suficiente para abater Client1 diretamente pelo servidor
    CHECK(host.apply_damage(0, 1, 120, 4));
    CHECK((host.slots()[1].flags & kFlagAlive) == 0);
    CHECK(host.slot_loadout_state(0).credits == 450 + rules.kill_reward_credits);

    // Comprando enquanto morto deve falhar com NotAliveOrEnded
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 3, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::NotAliveOrEnded));
    CHECK(c1.local_loadout().credits == 250 + rules.death_bonus_credits);
    CHECK(c1.local_loadout().primary_weapon == kNoWeapon);

    // Avança ticks até Client1 respawnar (nova janela de compra + colete dá +25 HP)
    for (int i = 0; i < 8; ++i) {
        host.tick();
    }
    c1.poll();
    CHECK((host.slots()[1].flags & kFlagAlive) != 0);
    // Client1 tem $850: compra Colete ($650) -> sobra $200 e HP passa para 125
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = kNoWeapon, .secondary_weapon = 0, .tactical_flags = kLoadoutArmor}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));
    CHECK(c1.local_loadout().credits == 200);
    CHECK(host.slots()[1].health == 125);

    // 8. Modo Orçamento Fixo (fixed_budget_mode = true, teto $3000)
    RulesConfig fixed_rules = RulesConfig::cs_loadout_deathmatch();
    fixed_rules.fixed_budget_mode = true;
    fixed_rules.max_credits = 3000;
    fixed_rules.buy_window_ticks = 0;  // sempre aberto

    // Hash precisa diferir das regras dinâmicas
    CHECK(fixed_rules.compute_hash() != rules.compute_hash());

    LoopbackEndpoint f_host_ep;
    LoopbackEndpoint f_c1_ep;
    auto [fh1, fc1] = LoopbackEndpoint::link(f_host_ep, f_c1_ep);
    (void)fh1;
    HostSession fhost(f_host_ep, fixed_rules, "FixedHost");
    ClientSession fclient(f_c1_ep, fc1, fixed_rules, "FixedClient");
    CHECK(fclient.send_hello());
    fhost.tick();
    fclient.poll();
    fhost.start_match();
    fhost.tick();
    fclient.poll();

    // Kit AK-47 ($2700) + Magnum ($700) = $3400 > $3000 -> ExceedsBudgetCap
    CHECK(fclient.send_loadout_request(LoadoutRequest{.primary_weapon = 7, .secondary_weapon = 2, .tactical_flags = 0}));
    fhost.tick();
    fclient.poll();
    CHECK(fclient.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::ExceedsBudgetCap));

    // Kit AK-47 ($2700) + Glock ($0) + Tático ($300) = $3000 <= $3000 -> Ok
    CHECK(fclient.send_loadout_request(LoadoutRequest{.primary_weapon = 7, .secondary_weapon = 0, .tactical_flags = kLoadoutTactical}));
    fhost.tick();
    fclient.poll();
    CHECK(fclient.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));
    CHECK(fclient.local_loadout().kit_cost == 3000);

    // Troca de kit completa no modo orçamento fixo continua funcionando dentro de $3000
    CHECK(fclient.send_loadout_request(LoadoutRequest{.primary_weapon = 6, .secondary_weapon = 0, .tactical_flags = kLoadoutArmor | kLoadoutTactical}));
    fhost.tick();
    fclient.poll();
    CHECK(fclient.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));
    CHECK(fclient.local_loadout().primary_weapon == 6);
    CHECK(fclient.local_loadout().kit_cost == 1800 + 650 + 300);

    // 9. Roster de nomes replicado ao cliente e janela de compra reaberta na entrada em gameplay (entered_level)
    CHECK(c1.player_name(0) == "HostCS");
    CHECK(c1.player_name(1) == "ClientCS");

    RulesConfig late_rules = RulesConfig::cs_loadout_deathmatch();
    late_rules.buy_window_ticks = 10;
    LoopbackEndpoint lh_ep;
    LoopbackEndpoint lc_ep;
    auto [l_h2c, l_c2h] = LoopbackEndpoint::link(lh_ep, lc_ep);
    (void)l_h2c;
    HostSession lhost(lh_ep, late_rules, "AlphaHost");
    ClientSession lclient(lc_ep, l_c2h, late_rules, "BravoGuest");
    CHECK(lclient.send_hello());
    lhost.tick();
    lclient.poll();
    lhost.start_match();
    // Avança 25 ticks enquanto o cliente ainda estaria nos menus
    for (int i = 0; i < 25; ++i) lhost.tick();
    lclient.poll();
    CHECK(lclient.player_name(0) == "AlphaHost");
    // Primeiro PlayerState vivo reabre a janela de compra do cliente na fase
    CHECK(lclient.send_local_state(PlayerState{
        .client_tick = 26,
        .position = lhost.slots()[1].position,
        .yaw = 0.0f,
        .pitch = 0.0f,
        .velocity = Vec3{0.0f, 0.0f, 0.0f},
        .anim_state = 0,
        .weapon = 0,
        .flags = kFlagAlive,
    }));
    lhost.tick();
    lclient.poll();
    CHECK(lclient.local_loadout().buy_ticks_left > 0);
    CHECK(lclient.send_loadout_request(LoadoutRequest{.primary_weapon = 3, .secondary_weapon = 0, .tactical_flags = 0}));
    lhost.tick();
    lclient.poll();
    CHECK(lclient.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));

    // Depois da janela expirar no host (cutscene), refresh_buy_window reabre a compra para o slot.
    for (int i = 0; i < 20; ++i) lhost.tick();
    CHECK(lhost.slot_loadout_state(0).buy_ticks_left == 0);
    lhost.refresh_buy_window(0);
    CHECK(lhost.slot_loadout_state(0).buy_ticks_left == late_rules.buy_window_ticks);
    CHECK(lhost.local_buy_loadout(LoadoutRequest{.primary_weapon = 3, .secondary_weapon = 0, .tactical_flags = 0}));

    // Pacote atrasado pré-respawn distante do spawn durante janela de graça de 1s é descartado sem penalizar o slot.
    CHECK(lhost.apply_damage(0, 1, 100, 3));
    for (uint32_t t = 0; t <= late_rules.respawn_delay_ticks + 1; ++t) lhost.tick();
    lclient.poll();
    CHECK((lhost.slots()[1].flags & kFlagAlive) != 0);
    const Vec3 far_pre_respawn{lhost.slots()[1].position.x + 25.0f, 0.0f, 0.0f};
    CHECK(lclient.send_local_state(PlayerState{
        .client_tick = 500,
        .position = far_pre_respawn,
        .yaw = 0.0f,
        .pitch = 0.0f,
        .velocity = Vec3{0.0f, 0.0f, 0.0f},
        .anim_state = 0,
        .weapon = 0,
        .flags = kFlagAlive,
    }));
    lhost.tick();
    CHECK(lhost.slots()[1].occupied);
    CHECK(lhost.slots()[1].violations == 0);
}

}  // namespace

void test_p3a_blast_weapon_mask_and_coop_ff() {
    // 1. PvP: level weapon mask + BlastEvent claim + anti-spoof distance check + self/barrel blast
    auto rules = RulesConfig::cs_loadout_deathmatch();
    rules.starting_credits = 5000;
    rules.spawns = {SpawnPoint{Vec3{0, 0, 0}, 0}, SpawnPoint{Vec3{2, 0, 0}, 0}};
    LoopbackEndpoint host_ep, c1_ep;
    auto [h_c1, c1_h] = LoopbackEndpoint::link(host_ep, c1_ep);
    (void)h_c1;

    HostSession host(host_ep, rules, "Host");
    // Apenas armas 0 (Glock), 7 (AK-47) e 15 (RPG-7) carregadas na fase
    const uint32_t mask = (1u << 0) | (1u << 7) | (1u << 15);
    host.set_level_context(4, mask);
    host.start_match();

    ClientSession c1(c1_ep, c1_h, rules, "P2");
    CHECK(c1.send_hello());
    for (int i = 0; i < 4; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK(c1.is_joined());
    CHECK(c1.active_arena_id() == 4);
    CHECK(c1.available_weapon_mask() == mask);

    // Tentativa de comprar arma 8 (M16A2, fora da máscara) deve falhar com InvalidItem
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 8, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::InvalidItem));

    // Comprar arma 7 (AK-47, na máscara) deve funcionar
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 7, .secondary_weapon = 0, .tactical_flags = 0}));
    host.tick();
    c1.poll();
    CHECK(c1.local_loadout().last_result == static_cast<uint8_t>(LoadoutResult::Ok));
    CHECK(c1.local_loadout().primary_weapon == 7);

    // Atualizar posição próxima do host/cliente e validar BlastEvent fora do raio vs dentro do raio
    PlayerState ps0{};
    ps0.sequence = 1;
    ps0.position = Vec3{1.0f, 0.0f, 0.0f};
    ps0.weapon = 0;
    ps0.flags = kFlagAlive;
    host.set_local_player_state(ps0);

    // Spoof distante (> 30u longe do alvo) deve ser ignorado sem causar dano
    CHECK(c1.send_blast(BlastEvent{
        .client_tick = 5,
        .attacker = 1,
        .target = 0,
        .weapon = 15,
        .flags = kBlastFlagClaim,
        .origin = Vec3{80.0f, 0.0f, 0.0f},
        .damage = 75.0f,
    }));
    host.tick();
    CHECK(host.slots()[0].health == 100);

    // Explosão próxima ao slot 0 (dentro do raio) aplica dano autoritativo e emite DamageEvent
    c1.clear_events();
    CHECK(c1.send_blast(BlastEvent{
        .client_tick = 6,
        .attacker = 1,
        .target = 0,
        .weapon = 15,
        .flags = kBlastFlagClaim,
        .origin = Vec3{3.0f, 0.0f, 0.0f},
        .damage = 65.0f,
    }));
    host.tick();
    c1.poll();
    CHECK(host.slots()[0].health == 35);
    CHECK(!c1.recent_damages().empty());
    CHECK(c1.recent_damages().back().attacker == 1);
    CHECK(c1.recent_damages().back().target == 0);
    CHECK(!c1.recent_blasts().empty());

    // Explosão ambiental/auto-dano fatal no slot 0 não dá kill para ninguém e respawna depois
    CHECK(host.local_blast(BlastEvent{
        .client_tick = 7,
        .attacker = kNoPlayer,
        .target = 0,
        .weapon = kNoWeapon,
        .flags = kBlastFlagClaim,
        .origin = Vec3{1.5f, 0.0f, 0.0f},
        .damage = 60.0f,
    }));
    CHECK(host.slots()[0].health == 0);
    CHECK(host.slots()[0].deaths == 1);
    CHECK(host.slots()[1].kills == 0);

    // 2. Co-op: friendly_fire = false impede dano entre aliados (tiro ou explosão)
    auto coop_rules = RulesConfig::default_deathmatch();
    coop_rules.game_mode = GameMode::Coop;
    coop_rules.friendly_fire = false;
    coop_rules.spawns = rules.spawns;
    LoopbackEndpoint ch_ep, cc_ep;
    auto [ch_cc, cc_ch] = LoopbackEndpoint::link(ch_ep, cc_ep);
    (void)ch_cc;
    HostSession coop_host(ch_ep, coop_rules, "CoopHost");
    coop_host.start_match();
    ClientSession coop_c(cc_ep, cc_ch, coop_rules, "Ally");
    CHECK(coop_c.send_hello());
    for (int i = 0; i < 4; ++i) {
        coop_host.tick();
        coop_c.poll();
    }
    CHECK(coop_c.is_joined());
    CHECK(coop_c.game_mode() == static_cast<uint8_t>(GameMode::Coop));
    CHECK(!coop_host.apply_damage(1, 0, 50, 7));
    CHECK(coop_host.slots()[0].health == 100);
}

void test_territorial_assault_host_client_match() {
    auto rules = RulesConfig::territorial_assault();
    rules.territorial.objective_count = 2;
    rules.territorial.capture_ticks = 6;
    rules.territorial.attacker_initial_waves = 2;
    rules.territorial.defender_initial_waves = 2;
    rules.territorial.wave_interval_ticks = 4;
    rules.territorial.waves_per_objective = 3;
    rules.territorial.max_waves = 10;
    rules.territorial.initial_time_ticks = 60;
    rules.territorial.bonus_time_ticks = 30;
    rules.territorial.max_time_ticks = 90;
    rules.territorial.overtime_cap_ticks = 10;
    rules.objective_zones = {
        CaptureZone{Vec3{20.0f, 0.0f, 0.0f}, 6.0f, 5.0f},
        CaptureZone{Vec3{50.0f, 0.0f, 0.0f}, 6.0f, 5.0f},
    };
    rules.attacker_spawns = {SpawnPoint{Vec3{0.0f, 0.0f, 0.0f}, 0.0f}};
    rules.defender_spawns = {SpawnPoint{Vec3{70.0f, 0.0f, 0.0f}, 3.14159f}};

    LoopbackEndpoint host_ep, c1_ep, late_ep;
    auto [h_c1, c1_h] = LoopbackEndpoint::link(host_ep, c1_ep);
    auto [h_late, late_h] = LoopbackEndpoint::link(host_ep, late_ep);
    (void)h_c1;
    (void)h_late;

    HostSession host(host_ep, rules, "HostA");
    ClientSession c1(c1_ep, c1_h, rules, "ClientB");
    CHECK(c1.send_hello());
    for (int i = 0; i < 3; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK(c1.is_joined() && c1.local_player_id() == 1);
    CHECK(c1.game_mode() == static_cast<uint8_t>(GameMode::TerritorialAssault));
    CHECK(c1.latest_territorial_state().has_value());
    CHECK(c1.latest_territorial_state()->player_teams[0] == 0);
    CHECK(c1.latest_territorial_state()->player_teams[1] == 1);

    // Troca de equipe permitida no Lobby e depois restaurada para B
    CHECK(c1.send_team_select(ta::Team::A));
    host.tick();
    c1.poll();
    CHECK(host.slots()[1].team == 0);
    CHECK(!host.enter_preparation());  // Equipe B vazia não pode iniciar
    CHECK(c1.send_team_select(ta::Team::B));
    host.tick();
    c1.poll();
    CHECK(host.slots()[1].team == 1);

    // Entra em Preparação e valida orçamento de 15 Supply Points
    CHECK(host.enter_preparation());
    host.tick();
    c1.poll();
    CHECK(c1.latest_territorial_state()->match_state == static_cast<uint8_t>(ta::MatchState::Preparation));

    // Cliente entrando tarde após início é recusado com NotAccepting
    ClientSession late(late_ep, late_h, rules, "LateJoin");
    CHECK(late.send_hello());
    host.tick();
    late.poll();
    CHECK(late.is_rejected() && late.reject_reason() == RejectReason::NotAccepting);

    // Sem arma primária -> rejeitado (GDD §5.3)
    CHECK(!host.local_buy_loadout(LoadoutRequest{.primary_weapon = kNoWeapon, .secondary_weapon = 0, .tactical_flags = 0}));
    CHECK(host.slot_loadout_state(0).last_result == static_cast<uint8_t>(LoadoutResult::InvalidItem));
    // Estouro de orçamento (RPG-7=10 + Magnum=2 + Colete=3 + Tático=2 = 17 > 15) -> rejeitado
    CHECK(!host.local_buy_loadout(LoadoutRequest{
        .primary_weapon = 15, .secondary_weapon = 2, .tactical_flags = kLoadoutArmor | kLoadoutTactical}));
    CHECK(host.slot_loadout_state(0).last_result == static_cast<uint8_t>(LoadoutResult::ExceedsBudgetCap));
    // Kit válido: AK-47 (7) + DC3 (1) + Colete (3) + Tático (2) = 13 <= 15
    CHECK(host.local_buy_loadout(LoadoutRequest{
        .primary_weapon = 7, .secondary_weapon = 1, .tactical_flags = kLoadoutArmor | kLoadoutTactical}));
    CHECK(c1.send_loadout_request(LoadoutRequest{.primary_weapon = 3, .secondary_weapon = 0, .tactical_flags = kLoadoutArmor}));
    host.tick();
    c1.poll();
    CHECK(host.slots()[1].owned_primary == 3);

    // Inicia Rodada 1 (Equipe A ataca, Equipe B defende)
    CHECK(host.begin_next_round());
    c1.poll();
    CHECK(host.phase() == MatchPhase::Running);
    CHECK(c1.latest_territorial_state()->match_state == static_cast<uint8_t>(ta::MatchState::Round1));
    CHECK(c1.latest_territorial_state()->attacker_team == 0);

    // Alteração de loadout bloqueada em combate (GDD §5.1)
    CHECK(!host.local_buy_loadout(LoadoutRequest{.primary_weapon = 9, .secondary_weapon = 0, .tactical_flags = 0}));
    CHECK(host.slot_loadout_state(0).last_result == static_cast<uint8_t>(LoadoutResult::BuyWindowClosed));

    // Cliente morre (defensor), preserva loadout e reaparece na onda coletiva após 4 ticks
    c1.clear_events();
    CHECK(host.apply_damage(0, 1, 200, 7));
    CHECK((host.slots()[1].flags & kFlagAlive) == 0);
    CHECK(host.slots()[1].owned_primary == 3);  // kit preservado dentro da rodada
    for (int i = 0; i < 4; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK((host.slots()[1].flags & kFlagAlive) != 0);
    CHECK(host.territorial_match()->waves(ta::Team::B) == 1);
    bool saw_wave = false;
    for (const auto& ev : c1.recent_territorial_events()) {
        if (ev.event_type == static_cast<uint8_t>(ta::EventType::WaveSpawned) && ev.team == 1) saw_wave = true;
    }
    CHECK(saw_wave);

    // Host (atacante) entra na zona do Objetivo 0 -> captura em 6 ticks
    host.set_local_player_state(PlayerState{.position = Vec3{20.0f, 0.0f, 0.0f}, .weapon = 7, .flags = kFlagAlive});
    c1.clear_events();
    for (int i = 0; i < 6; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK(host.territorial_match()->captured_count() == 1);
    CHECK(host.territorial_match()->waves(ta::Team::A) == 5);  // 2 + 3 recompensa
    bool saw_cap = false;
    for (const auto& ev : c1.recent_territorial_events()) {
        if (ev.event_type == static_cast<uint8_t>(ta::EventType::ObjectiveCaptured) && ev.index == 0) saw_cap = true;
    }
    CHECK(saw_cap);

    // Host captura Objetivo 1 -> Rodada 1 encerra -> Intermission
    host.set_local_player_state(PlayerState{.position = Vec3{50.0f, 0.0f, 0.0f}, .weapon = 7, .flags = kFlagAlive});
    for (int i = 0; i < 6; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK(host.territorial_match()->state() == ta::MatchState::Intermission);
    CHECK(host.phase() == MatchPhase::Warmup);
    CHECK(c1.latest_territorial_state()->match_state == static_cast<uint8_t>(ta::MatchState::Intermission));
    CHECK(c1.latest_territorial_state()->r1_captured == 2);

    // Em Intermission, troca de loadout volta a ser permitida
    CHECK(host.local_buy_loadout(LoadoutRequest{.primary_weapon = 9, .secondary_weapon = 0, .tactical_flags = 0}));

    // Rodada 2: funções invertidas (B ataca, A defende); B não captura nada até estourar o "tempo a bater" -> A vence
    CHECK(host.begin_next_round());
    c1.poll();
    CHECK(c1.latest_territorial_state()->match_state == static_cast<uint8_t>(ta::MatchState::Round2));
    CHECK(c1.latest_territorial_state()->attacker_team == 1);
    for (int i = 0; i < 40; ++i) {
        host.tick();
        c1.poll();
    }
    CHECK(host.phase() == MatchPhase::Ended);
    CHECK(host.winner() == 0);
    CHECK(c1.phase() == MatchPhase::Ended);
    CHECK(c1.winner() == 0);
    CHECK(c1.latest_territorial_state()->winner_team == 0);

    // Teste de segurança: cliente tentando falsificar TerritorialState é expulso (KickReason::Malformed)
    // e fogo amigo entre aliados da mesma equipe é bloqueado.
    LoopbackEndpoint sh_ep, s1_ep, s2_ep;
    auto [sh_1, s1_h] = LoopbackEndpoint::link(sh_ep, s1_ep);
    auto [sh_2, s2_h] = LoopbackEndpoint::link(sh_ep, s2_ep);
    (void)sh_1;
    (void)sh_2;
    HostSession sec_host(sh_ep, rules, "SecHost");
    ClientSession ally(s1_ep, s1_h, rules, "AllyA");
    ClientSession foe(s2_ep, s2_h, rules, "FoeB");
    CHECK(ally.send_hello() && foe.send_hello());
    sec_host.tick();
    ally.poll();
    foe.poll();
    CHECK(ally.send_team_select(ta::Team::A));
    CHECK(foe.send_team_select(ta::Team::B));
    sec_host.tick();
    sec_host.start_match();
    // Fogo amigo entre Host (0, A) e AllyA (1, A) bloqueado
    CHECK(!sec_host.apply_damage(0, 1, 50, 7));
    CHECK(sec_host.slots()[1].health == 100);
    // Cliente envia TerritorialState falso (exclusivo do host) -> penalizado até kick
    const auto fake_ta = encode(TerritorialState{.tick = 999, .objective_count = 2, .captured_count = 2});
    for (int i = 0; i < rules.max_violations_before_kick + 1; ++i) {
        s2_ep.send(s2_h, Channel::Reliable, fake_ta);
        sec_host.tick();
        foe.poll();
    }
    CHECK(foe.is_kicked() && foe.kick_reason() == KickReason::Malformed);
}

int main() {
    test_protocol_roundtrip_all_messages();
    test_hostile_and_malformed_inputs();
    test_four_player_deathmatch_simulation();
    test_rate_limit_and_malformed_kick();
    test_host_apply_damage_and_kill_attribution();
    test_cs_loadout_economy_and_limits();
    test_p3a_blast_weapon_mask_and_coop_ff();
    test_territorial_assault_host_client_match();
    std::printf("[netcore_tests] All protocol, fuzzing, session and security tests PASSED.\n");
    return 0;
}
