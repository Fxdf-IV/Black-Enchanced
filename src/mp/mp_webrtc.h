#pragma once
// Transporte WebRTC (DataChannels reliable/unreliable) implementando netcore::ITransport.
// Permite conectar HostSession / ClientSession C++ diretamente a RTCPeerConnection/RTCDataChannel
// no navegador sem dependências de Win32 ou Winsock.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <unordered_set>
#include <vector>

#include "netcore/transport.hpp"

namespace mp {

struct WebRtcOutgoingPacket {
    netcore::PeerId peer = netcore::kInvalidPeer;
    netcore::Channel channel = netcore::Channel::Reliable;
    std::vector<uint8_t> payload;
};

class WebRtcTransport final : public netcore::ITransport {
public:
    WebRtcTransport() = default;
    ~WebRtcTransport() override = default;

    WebRtcTransport(const WebRtcTransport&) = delete;
    WebRtcTransport& operator=(const WebRtcTransport&) = delete;

    // ITransport
    bool send(netcore::PeerId peer, netcore::Channel channel, std::span<const uint8_t> data) override;
    void broadcast(netcore::Channel channel, std::span<const uint8_t> data) override;
    void disconnect(netcore::PeerId peer) override;
    std::vector<netcore::TransportEvent> poll() override;

    // Chamados pelo host JS / WebRTC quando o canal abre, fecha ou entrega pacote binário
    void push_incoming_connected(netcore::PeerId peer);
    void push_incoming_disconnected(netcore::PeerId peer);
    bool push_incoming_packet(netcore::PeerId peer, netcore::Channel channel, std::span<const uint8_t> data);

    // Drenados pela bridge JS -> RTCDataChannel.send()
    bool pop_outgoing(WebRtcOutgoingPacket& out);
    bool pop_disconnect(netcore::PeerId& out_peer);
    std::size_t peer_count() const;

private:
    mutable std::mutex mu_;
    std::unordered_set<netcore::PeerId> peers_;
    std::deque<netcore::TransportEvent> inbox_;
    std::deque<WebRtcOutgoingPacket> outbox_;
    std::deque<netcore::PeerId> disconnects_;
};

}  // namespace mp
