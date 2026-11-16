#include "mp/mp_webrtc.h"

#include <algorithm>
#include <utility>

#include "netcore/limits.hpp"

namespace mp {

bool WebRtcTransport::send(netcore::PeerId peer, netcore::Channel channel, std::span<const uint8_t> data) {
    if (peer == netcore::kInvalidPeer || data.empty() || data.size() > netcore::kMaxPacketSize) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!peers_.count(peer)) {
        return false;
    }
    if (outbox_.size() >= 1024) {
        outbox_.pop_front();
    }
    outbox_.push_back(WebRtcOutgoingPacket{
        .peer = peer,
        .channel = channel,
        .payload = std::vector<uint8_t>(data.begin(), data.end()),
    });
    return true;
}

void WebRtcTransport::broadcast(netcore::Channel channel, std::span<const uint8_t> data) {
    if (data.empty() || data.size() > netcore::kMaxPacketSize) {
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (netcore::PeerId peer : peers_) {
        if (outbox_.size() >= 1024) {
            outbox_.pop_front();
        }
        outbox_.push_back(WebRtcOutgoingPacket{
            .peer = peer,
            .channel = channel,
            .payload = std::vector<uint8_t>(data.begin(), data.end()),
        });
    }
}

void WebRtcTransport::disconnect(netcore::PeerId peer) {
    if (peer == netcore::kInvalidPeer) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (peers_.erase(peer) > 0) {
        disconnects_.push_back(peer);
        inbox_.push_back(netcore::TransportEvent{
            .type = netcore::TransportEventType::Disconnected,
            .peer = peer,
            .channel = netcore::Channel::Reliable,
            .payload = {},
        });
    }
}

std::vector<netcore::TransportEvent> WebRtcTransport::poll() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<netcore::TransportEvent> events;
    events.reserve(inbox_.size());
    while (!inbox_.empty()) {
        events.push_back(std::move(inbox_.front()));
        inbox_.pop_front();
    }
    return events;
}

void WebRtcTransport::push_incoming_connected(netcore::PeerId peer) {
    if (peer == netcore::kInvalidPeer) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (peers_.insert(peer).second) {
        inbox_.push_back(netcore::TransportEvent{
            .type = netcore::TransportEventType::Connected,
            .peer = peer,
            .channel = netcore::Channel::Reliable,
            .payload = {},
        });
    }
}

void WebRtcTransport::push_incoming_disconnected(netcore::PeerId peer) {
    if (peer == netcore::kInvalidPeer) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (peers_.erase(peer) > 0) {
        inbox_.push_back(netcore::TransportEvent{
            .type = netcore::TransportEventType::Disconnected,
            .peer = peer,
            .channel = netcore::Channel::Reliable,
            .payload = {},
        });
    }
}

bool WebRtcTransport::push_incoming_packet(netcore::PeerId peer, netcore::Channel channel,
                                           std::span<const uint8_t> data) {
    if (peer == netcore::kInvalidPeer || data.empty() || data.size() > netcore::kMaxPacketSize) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    peers_.insert(peer);
    if (inbox_.size() >= 1024) {
        inbox_.pop_front();
    }
    inbox_.push_back(netcore::TransportEvent{
        .type = netcore::TransportEventType::Packet,
        .peer = peer,
        .channel = channel,
        .payload = std::vector<uint8_t>(data.begin(), data.end()),
    });
    return true;
}

bool WebRtcTransport::pop_outgoing(WebRtcOutgoingPacket& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (outbox_.empty()) return false;
    out = std::move(outbox_.front());
    outbox_.pop_front();
    return true;
}

bool WebRtcTransport::pop_disconnect(netcore::PeerId& out_peer) {
    std::lock_guard<std::mutex> lock(mu_);
    if (disconnects_.empty()) return false;
    out_peer = disconnects_.front();
    disconnects_.pop_front();
    return true;
}

std::size_t WebRtcTransport::peer_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peers_.size();
}

}  // namespace mp
