// Teste de integração do transporte UDP real (Winsock2/POSIX em 127.0.0.1) + bridge multiplayer.
// Escopo explícito:
//   - Testes 1..5 exercitam sockets UDP reais do SO em 127.0.0.1 com libs/netcore + src/mp/mp_udp.cpp.
//   - Teste 6 valida mp_bridge (role padrão OfflineCampaign, CLI UDP e correção de world + 0x520)
//     sobre um buffer sintético de RAM convidada (NÃO valida o default.xbe real do jogo).

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "game/black_addrs.h"
#include "mp/mp_bridge.h"
#include "mp/mp_udp.h"
#include "netcore/rules.hpp"
#include "netcore/session.hpp"

// Stubs leves de logx para compilar mp_bridge.cpp fora do executável completo do SDK.
namespace logx {
void Init(const std::string&, const std::string&, bool) {}
void Write(Level, const char* fmt, ...) {
    (void)fmt;
}
void Flush() {}
const std::string& Path() {
    static const std::string kEmpty;
    return kEmpty;
}
}  // namespace logx

#define CHECK(cond)                                                                                 \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            std::fprintf(stderr, "CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            std::abort();                                                                           \
        }                                                                                           \
    } while (0)

namespace {

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void PumpPair(netcore::HostSession& host, netcore::ClientSession& c1, netcore::ClientSession* c2, int steps) {
    for (int i = 0; i < steps; ++i) {
        host.tick();
        c1.poll();
        if (c2) c2->poll();
        SleepMs(4);
    }
}

void SendRawGarbageUdp(uint16_t port, const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s != INVALID_SOCKET);
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CHECK(s >= 0);
#endif
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr);
    sendto(s, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
           reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

void TestUdpReliableOrderingUnderLoss() {
    mp::UdpTransportConfig cfg{};
    cfg.retransmit_interval_ms = 20;
    cfg.keepalive_interval_ms = 50;
    cfg.peer_timeout_ms = 2000;

    auto host = mp::UdpTransport::CreateHost(0, "127.0.0.1", cfg);
    CHECK(host && host->is_valid() && host->local_port() != 0);

    auto client = mp::UdpTransport::CreateClient("127.0.0.1", host->local_port(), 0, cfg);
    CHECK(client && client->is_valid() && client->local_port() != 0);

    // Derruba apenas o primeiro datagrama (#1) para forçar o #2 chegar antes no socket real
    // e exercitar reordenação + retransmissão ordenada na camada confiável.
    client->drop_next_outgoing_for_test(1);

    const std::vector<uint8_t> pkt1 = {'P', 'K', 'T', '_', '1'};
    const std::vector<uint8_t> pkt2 = {'P', 'K', 'T', '_', '2'};
    const std::vector<uint8_t> pkt3 = {'P', 'K', 'T', '_', '3'};

    CHECK(client->send(client->host_peer_id(), netcore::Channel::Reliable, pkt1));
    CHECK(client->send(client->host_peer_id(), netcore::Channel::Reliable, pkt2));
    CHECK(client->send(client->host_peer_id(), netcore::Channel::Reliable, pkt3));
    CHECK(client->packets_dropped_for_test() == 1);

    std::vector<std::vector<uint8_t>> received;
    bool saw_connected = false;
    for (int i = 0; i < 40 && received.size() < 3; ++i) {
        SleepMs(8);
        client->poll();
        for (const auto& ev : host->poll()) {
            if (ev.type == netcore::TransportEventType::Connected) saw_connected = true;
            if (ev.type == netcore::TransportEventType::Packet) received.push_back(ev.payload);
        }
    }

    CHECK(saw_connected);
    CHECK(received.size() == 3);
    CHECK(received[0] == pkt1);
    CHECK(received[1] == pkt2);
    CHECK(received[2] == pkt3);
    CHECK(client->retransmissions_for_test() >= 1);
    std::printf("[PASS] TestUdpReliableOrderingUnderLoss (retransmissions=%llu)\n",
                static_cast<unsigned long long>(client->retransmissions_for_test()));
}

void TestUdpHandshakeCombatAndCleanup() {
    mp::UdpTransportConfig cfg{};
    cfg.retransmit_interval_ms = 25;
    cfg.keepalive_interval_ms = 60;
    cfg.peer_timeout_ms = 2000;

    auto rules = netcore::RulesConfig::default_deathmatch();
    rules.respawn_delay_ticks = 6;

    auto host_tr = mp::UdpTransport::CreateHost(0, "127.0.0.1", cfg);
    CHECK(host_tr && host_tr->is_valid());
    netcore::HostSession host(*host_tr, rules, "Host");
    host.start_match();

    auto c1_tr = mp::UdpTransport::CreateClient("127.0.0.1", host_tr->local_port(), 0, cfg);
    auto c2_tr = mp::UdpTransport::CreateClient("127.0.0.1", host_tr->local_port(), 0, cfg);
    CHECK(c1_tr && c1_tr->is_valid());
    CHECK(c2_tr && c2_tr->is_valid());

    netcore::ClientSession c1(*c1_tr, c1_tr->host_peer_id(), rules, "Alpha");
    netcore::ClientSession c2(*c2_tr, c2_tr->host_peer_id(), rules, "Bravo");
    CHECK(c1.send_hello());
    CHECK(c2.send_hello());

    for (int i = 0; i < 40 && (!c1.is_joined() || !c2.is_joined()); ++i) {
        PumpPair(host, c1, &c2, 1);
    }
    CHECK(c1.is_joined() && c2.is_joined());
    CHECK(host.player_count() == 3);
    CHECK(c1.local_player_id() != c2.local_player_id());

    // Envia pacotes inválidos/corrompidos no mesmo socket real do host para validar blindagem
    SendRawGarbageUdp(host_tr->local_port(), {0x01});
    SendRawGarbageUdp(host_tr->local_port(), {0xDE, 0xAD, 0xBE, 0xEF});
    std::vector<uint8_t> fake_oversized(24, 0);
    fake_oversized[0] = 0x4D;
    fake_oversized[1] = 0x42;  // 0x424D LE
    fake_oversized[2] = 2;     // Unreliable
    fake_oversized[16] = 0xDC;
    fake_oversized[17] = 0x05;  // claimed payload_len = 1500 > 1200
    SendRawGarbageUdp(host_tr->local_port(), fake_oversized);
    PumpPair(host, c1, &c2, 3);
    CHECK(host.player_count() == 3);

    // Move c1 e c2 perto um do outro sobre UDP e valida combate autoritativo + kill + respawn
    netcore::PlayerState s1{};
    s1.client_tick = host.current_tick();
    s1.position = {10.0f, 0.0f, 10.0f};
    s1.flags = netcore::kFlagAlive;

    netcore::PlayerState s2{};
    s2.client_tick = host.current_tick();
    s2.position = {18.0f, 0.0f, 10.0f};
    s2.flags = netcore::kFlagAlive;

    CHECK(c1.send_local_state(s1));
    CHECK(c2.send_local_state(s2));
    PumpPair(host, c1, &c2, 4);

    // Arma 1 (M16A2) causa 28 de dano e exige intervalo mínimo de 3 ticks -> 4 tiros matam alvo de 100 HP
    for (int shot = 0; shot < 4; ++shot) {
        netcore::FireEvent fe{};
        fe.client_tick = host.current_tick();
        fe.weapon = 1;
        fe.origin = {10.0f, 1.5f, 10.0f};
        fe.direction = {1.0f, 0.0f, 0.0f};
        fe.claimed_target = c2.local_player_id();
        CHECK(c1.send_fire(fe));
        PumpPair(host, c1, &c2, 5);
    }

    bool saw_death = false;
    bool saw_respawn = false;
    for (int step = 0; step < 30; ++step) {
        PumpPair(host, c1, &c2, 1);
        for (const auto& d : c2.recent_deaths()) {
            if (d.victim == c2.local_player_id() && d.killer == c1.local_player_id()) saw_death = true;
        }
        for (const auto& r : c2.recent_respawns()) {
            if (r.player == c2.local_player_id()) saw_respawn = true;
        }
        c2.clear_events();
        if (saw_death && saw_respawn) break;
    }

    CHECK(saw_death);
    CHECK(saw_respawn);
    CHECK(host.slots()[c1.local_player_id()].kills == 1);
    CHECK(host.slots()[c2.local_player_id()].deaths == 1);

    // Desconexão explícita via datagrama de controle
    const uint8_t c2_id = c2.local_player_id();
    c2_tr->disconnect(c2_tr->host_peer_id());
    PumpPair(host, c1, nullptr, 4);
    CHECK(!host.slots()[c2_id].occupied);
    CHECK(host.player_count() == 2);

    std::printf("[PASS] TestUdpHandshakeCombatAndCleanup\n");
}

void TestUdpIdlePeerTimeout() {
    mp::UdpTransportConfig cfg{};
    cfg.retransmit_interval_ms = 20;
    cfg.keepalive_interval_ms = 20;
    cfg.peer_timeout_ms = 80;

    auto host = mp::UdpTransport::CreateHost(0, "127.0.0.1", cfg);
    auto client = mp::UdpTransport::CreateClient("127.0.0.1", host->local_port(), 0, cfg);
    CHECK(host->is_valid() && client->is_valid());

    const std::vector<uint8_t> ping = {'H', 'I'};
    CHECK(client->send(client->host_peer_id(), netcore::Channel::Unreliable, ping));

    bool got_packet = false;
    for (int i = 0; i < 20 && !got_packet; ++i) {
        SleepMs(5);
        for (const auto& ev : host->poll()) {
            if (ev.type == netcore::TransportEventType::Packet) got_packet = true;
        }
    }
    CHECK(got_packet);
    CHECK(host->active_peer_count() == 1);

    // Para de fazer poll() no cliente para silenciar keepalives
    SleepMs(120);
    bool timed_out = false;
    for (const auto& ev : host->poll()) {
        if (ev.type == netcore::TransportEventType::Disconnected) timed_out = true;
    }
    CHECK(timed_out);
    CHECK(host->active_peer_count() == 0);
    std::printf("[PASS] TestUdpIdlePeerTimeout\n");
}

void TestSyntheticBridgeControllerAndCli() {
    // Sem flags, InitBridgeFromArgs deve preservar estritamente OfflineCampaign
    const char* default_argv[] = {"BlackEnhancedOnline.exe"};
    mp::InitBridgeFromArgs(1, default_argv);
    CHECK(mp::CurrentRole() == mp::Role::OfflineCampaign);

    // Arena sintética de memória convidada para checar ausência de dupla desreferência em world + 0x520
    std::vector<uint8_t> guest_ram(4 * 1024 * 1024, 0);
    uint8_t* base = guest_ram.data();

    constexpr uint32_t kGameObj = 0x00100000u;
    constexpr uint32_t kWorldObj = 0x00140000u;
    constexpr uint32_t kPlayer = kWorldObj + black::kWorldPlayer;
    constexpr uint32_t kCtrl = kWorldObj + black::kWorldController;
    constexpr uint32_t kFakeVtable = 0x0028ABCDu;

    black::W32(base, black::kGame, kGameObj);
    black::W32(base, kGameObj + black::kGameState, kGameObj + black::kInLevelState);
    black::W32(base, kGameObj + black::kInLevelState + 4, 0x1Cu);
    black::W32(base, black::kWorld, kWorldObj);

    // PlayerController embutido em world + 0x520 com vtable em +0x00 e ângulos em +0x18 / +0x1C
    black::W32(base, kCtrl + 0x00, kFakeVtable);
    black::WF32(base, kCtrl + 0x18, 90.0f);
    black::WF32(base, kCtrl + 0x1C, -15.0f);

    // Jogador vivo aponta +0x324 para world + 0x520
    black::W32(base, kPlayer + black::kCharControl, kCtrl);
    black::W32(base, kPlayer + black::kCharState, 0u);
    black::WF32(base, kPlayer + black::kCharHealth, 100.0f);
    black::W32(base, kPlayer + black::kCharName + 0, 0x4B454C4Cu);
    black::W32(base, kPlayer + black::kCharName + 4, 0x31305241u);
    black::WF32(base, kPlayer + black::kCharPos + 0, 12.0f);
    black::WF32(base, kPlayer + black::kCharPos + 4, 1.0f);
    black::WF32(base, kPlayer + black::kCharPos + 8, -4.0f);

    // Simula StartHostUdp + CLI parser no cliente (usando porta fixa livre e localhost)
    CHECK(mp::StartHostUdp(0, "HostTest"));
    CHECK(mp::CurrentRole() == mp::Role::Host);
    mp::OnImageLoaded(base);

    // Simula morte do jogador: OnLocalPlayerDying captura player + kCharControl sem corromper world + 0x520
    mp::OnLocalPlayerDying(base);
    black::W32(base, kPlayer + black::kCharControl, kPlayer + black::kCharDeathControl);
    black::W32(base, kPlayer + black::kCharState, 2u);
    black::WF32(base, kPlayer + black::kCharHealth, 0.0f);
    black::W32(base, kPlayer + black::kCharName + 0, 0u);
    black::W32(base, kPlayer + black::kCharName + 4, 0u);
    CHECK(mp::ShouldSuppressMissionFailed(base));

    // Executa OnPreFrame / OnPostFrame e garante que respawn restaura player + 0x324 mantendo world + 0x520 intacto
    mp::OnPreFrame(base);
    mp::OnPostFrame(base);
    CHECK(black::R32(base, kCtrl + 0x00) == kFakeVtable);
    CHECK(black::R32(base, kPlayer + black::kCharControl) == kCtrl);
    CHECK(black::R32(base, kPlayer + black::kCharState) == 0u);
    CHECK(black::RF32(base, kPlayer + black::kCharHealth) == 100.0f);
    CHECK(black::R32(base, kPlayer + black::kCharName + 0) == 0x4B454C4Cu);
    CHECK(black::R32(base, kPlayer + black::kCharName + 4) == 0x31305241u);

    // Inicia cliente via CLI numa segunda ponte em processo separado não se aplica aqui pois mp_bridge é singleton,
    // mas validamos StopMultiplayer e InitBridgeFromArgs("--mp-join", ...) separadamente.
    mp::StopMultiplayer();
    CHECK(mp::CurrentRole() == mp::Role::OfflineCampaign);

    const char* join_argv[] = {"BlackEnhancedOnline.exe", "--mp-join", "127.0.0.1:45499", "--mp-name", "Beta"};
    mp::InitBridgeFromArgs(5, join_argv);
    CHECK(mp::CurrentRole() == mp::Role::Client);
    mp::StopMultiplayer();
    CHECK(mp::CurrentRole() == mp::Role::OfflineCampaign);

    std::printf("[PASS] TestSyntheticBridgeControllerAndCli [RAM SINTETICA, NAO XBE REAL]\n");
}

}  // namespace

int main() {
    TestUdpReliableOrderingUnderLoss();
    TestUdpHandshakeCombatAndCleanup();
    TestUdpIdlePeerTimeout();
    TestSyntheticBridgeControllerAndCli();
    std::printf("Todos os testes UDP reais e de layout sintético passaram!\n");
    return 0;
}
