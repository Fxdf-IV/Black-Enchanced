#pragma once
// Interface comum de transporte (seção 9 do plano).
// A lógica de partida e sessão fala apenas com ITransport; nunca depende diretamente de ENet/UDP
// nem de WebRTC DataChannels.

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "netcore/protocol.hpp"

namespace netcore {

using PeerId = uint32_t;
inline constexpr PeerId kInvalidPeer = 0;

enum class TransportEventType : uint8_t {
    Connected = 1,
    Disconnected = 2,
    Packet = 3,
};

struct TransportEvent {
    TransportEventType type = TransportEventType::Connected;
    PeerId peer = kInvalidPeer;
    Channel channel = Channel::Reliable;
    std::vector<uint8_t> payload;
};

class ITransport {
public:
    virtual ~ITransport() = default;

    // Envia um pacote para um par conectado. Retorna false se o par não existe ou o pacote excede kMaxPacketSize.
    virtual bool send(PeerId peer, Channel channel, std::span<const uint8_t> data) = 0;

    // Envia para todos os pares conectados.
    virtual void broadcast(Channel channel, std::span<const uint8_t> data) = 0;

    // Encerra a conexão com um par.
    virtual void disconnect(PeerId peer) = 0;

    // Coleta eventos pendentes (conexões, desconexões e pacotes recebidos).
    virtual std::vector<TransportEvent> poll() = 0;
};

// Transporte em memória para testes determinísticos de múltiplos jogadores (1 host + N clientes)
// sem abrir portas de rede. Também permite simular perda/atraso e injeção de pacotes hostis.
class LoopbackEndpoint final : public ITransport {
public:
    LoopbackEndpoint() = default;

    bool send(PeerId peer, Channel channel, std::span<const uint8_t> data) override {
        if (data.size() == 0 || data.size() > kMaxPacketSize) return false;
        auto it = peers_.find(peer);
        if (it == peers_.end()) return false;
        auto* remote = it->second.remote;
        const PeerId remote_id_for_me = it->second.remote_peer_id;
        remote->inbox_.push_back(TransportEvent{
            .type = TransportEventType::Packet,
            .peer = remote_id_for_me,
            .channel = channel,
            .payload = std::vector<uint8_t>(data.begin(), data.end()),
        });
        return true;
    }

    // Permite que testes injetem pacotes brutos (inclusive acima de kMaxPacketSize ou truncados)
    // diretamente na fila de entrada do destino.
    void inject_raw_to(PeerId peer, Channel channel, std::span<const uint8_t> raw) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        auto* remote = it->second.remote;
        const PeerId remote_id_for_me = it->second.remote_peer_id;
        remote->inbox_.push_back(TransportEvent{
            .type = TransportEventType::Packet,
            .peer = remote_id_for_me,
            .channel = channel,
            .payload = std::vector<uint8_t>(raw.begin(), raw.end()),
        });
    }

    void broadcast(Channel channel, std::span<const uint8_t> data) override {
        for (const auto& [peer_id, _] : peers_) {
            send(peer_id, channel, data);
        }
    }

    void disconnect(PeerId peer) override {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        LoopbackEndpoint* remote = it->second.remote;
        const PeerId remote_peer_id = it->second.remote_peer_id;
        peers_.erase(it);
        inbox_.push_back(TransportEvent{
            .type = TransportEventType::Disconnected,
            .peer = peer,
            .channel = Channel::Reliable,
            .payload = {},
        });
        if (remote) {
            remote->peers_.erase(remote_peer_id);
            remote->inbox_.push_back(TransportEvent{
                .type = TransportEventType::Disconnected,
                .peer = remote_peer_id,
                .channel = Channel::Reliable,
                .payload = {},
            });
        }
    }

    std::vector<TransportEvent> poll() override {
        std::vector<TransportEvent> out(inbox_.begin(), inbox_.end());
        inbox_.clear();
        return out;
    }

    // Conecta dois endpoints em memória e devolve o PeerId usado por cada lado.
    static std::pair<PeerId, PeerId> link(LoopbackEndpoint& a, LoopbackEndpoint& b) {
        const PeerId id_on_a = a.next_peer_id_++;
        const PeerId id_on_b = b.next_peer_id_++;
        a.peers_[id_on_a] = Link{&b, id_on_b};
        b.peers_[id_on_b] = Link{&a, id_on_a};
        a.inbox_.push_back(
            TransportEvent{.type = TransportEventType::Connected, .peer = id_on_a, .channel = Channel::Reliable, .payload = {}});
        b.inbox_.push_back(
            TransportEvent{.type = TransportEventType::Connected, .peer = id_on_b, .channel = Channel::Reliable, .payload = {}});
        return {id_on_a, id_on_b};
    }

private:
    struct Link {
        LoopbackEndpoint* remote = nullptr;
        PeerId remote_peer_id = kInvalidPeer;
    };

    PeerId next_peer_id_ = 1;
    std::unordered_map<PeerId, Link> peers_;
    std::deque<TransportEvent> inbox_;
};

}  // namespace netcore
