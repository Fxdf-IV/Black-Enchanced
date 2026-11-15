#pragma once
// Transporte UDP real da Fase 4 implementando netcore::ITransport (Win32 Winsock2 / POSIX).
// Sem subpastas e sem inclusão de cabeçalhos de plataforma neste .h para preservar portabilidade.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "netcore/transport.hpp"

namespace mp {

inline constexpr uint16_t kDefaultUdpPort = 45410;

struct UdpTransportConfig {
    uint32_t retransmit_interval_ms = 80;
    uint32_t keepalive_interval_ms = 500;
    uint32_t peer_timeout_ms = 5000;
    std::size_t max_peers = 16;
    std::size_t max_reorder_buffer = 64;
};

// Implementa netcore::ITransport sobre um socket UDP não bloqueante com entrega confiável
// ordenada (sequência + ACK + bits seletivos + retransmissão) e datagramas não confiáveis.
class UdpTransport final : public netcore::ITransport {
public:
    static std::unique_ptr<UdpTransport> CreateHost(
        uint16_t bind_port = kDefaultUdpPort,
        const std::string& bind_addr = "0.0.0.0",
        UdpTransportConfig config = {});

    static std::unique_ptr<UdpTransport> CreateClient(
        const std::string& remote_host,
        uint16_t remote_port = kDefaultUdpPort,
        uint16_t local_bind_port = 0,
        UdpTransportConfig config = {});

    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    bool is_valid() const;
    bool is_host() const;
    uint16_t local_port() const;
    netcore::PeerId host_peer_id() const;
    std::size_t active_peer_count() const;

    bool send(netcore::PeerId peer, netcore::Channel channel, std::span<const uint8_t> data) override;
    void broadcast(netcore::Channel channel, std::span<const uint8_t> data) override;
    void disconnect(netcore::PeerId peer) override;
    std::vector<netcore::TransportEvent> poll() override;

    // Utilitário determinístico para testes unitários/locais em loopback: descarta os próximos N
    // datagramas de saída no socket real do SO para exercitar retransmissão e reordenação.
    void drop_next_outgoing_for_test(uint32_t count);
    uint64_t packets_dropped_for_test() const;
    uint64_t retransmissions_for_test() const;

private:
    struct Impl;
    explicit UdpTransport(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace mp
