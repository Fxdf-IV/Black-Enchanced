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
    check_roundtrip(Welcome{.player_id = 1, .max_players = 4, .tick_rate_hz = 30, .snapshot_rate_hz = 15});
    check_roundtrip(Reject{.reason = RejectReason::RoomFull});
    check_roundtrip(MatchStart{.start_tick = 10, .time_limit_s = 600, .kill_limit = 15, .arena_id = 2});
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

    // 9. Fuzz determinístico (10.000 pacotes pseudoaleatórios nunca podem causar falha ou leitura inválida)
    std::mt19937 rng(0xB1AC2006u);
    for (int i = 0; i < 10000; ++i) {
        const std::size_t len = rng() % (kMaxPacketSize + 32);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(rng() & 0xFF);
        if (len >= 3 && (rng() % 2 == 0)) {
            buf[0] = uint8_t(kPacketMagic);
            buf[1] = uint8_t(kPacketMagic >> 8);
            buf[2] = static_cast<uint8_t>(1 + (rng() % 16));
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

}  // namespace

int main() {
    test_protocol_roundtrip_all_messages();
    test_hostile_and_malformed_inputs();
    test_four_player_deathmatch_simulation();
    test_rate_limit_and_malformed_kick();
    std::printf("[netcore_tests] All protocol, fuzzing, session and security tests PASSED.\n");
    return 0;
}
