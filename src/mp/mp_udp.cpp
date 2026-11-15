#include "mp/mp_udp.h"

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
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <unordered_map>

#include "netcore/limits.hpp"

namespace mp {

namespace {

constexpr uint16_t kWireMagic = 0x424D;  // "BM"
constexpr std::size_t kWireHeaderSize = 18;
constexpr std::size_t kMaxWireDatagram = kWireHeaderSize + netcore::kMaxPacketSize;

enum class WireKind : uint8_t {
    Reliable = 1,
    Unreliable = 2,
    Ack = 3,
    Disconnect = 4,
};

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void WriteU16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void WriteU32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t ReadU16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

uint32_t ReadU32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kBadSocket = INVALID_SOCKET;

class WsaRuntime {
public:
    WsaRuntime() {
        WSADATA wsa{};
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WsaRuntime() {
        if (ok_) WSACleanup();
    }
    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

void CloseNativeSocket(NativeSocket s) {
    if (s != kBadSocket) closesocket(s);
}

bool SetNonBlocking(NativeSocket s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool WouldBlockError() {
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAECONNRESET;
}
#else
using NativeSocket = int;
constexpr NativeSocket kBadSocket = -1;

class WsaRuntime {
public:
    bool ok() const { return true; }
};

void CloseNativeSocket(NativeSocket s) {
    if (s >= 0) close(s);
}

bool SetNonBlocking(NativeSocket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool WouldBlockError() {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == ECONNRESET;
}
#endif

struct Endpoint {
    uint32_t ipv4_be = 0;
    uint16_t port_be = 0;
    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

struct EndpointHash {
    std::size_t operator()(const Endpoint& e) const noexcept {
        return (static_cast<std::size_t>(e.ipv4_be) << 16) ^ static_cast<std::size_t>(e.port_be);
    }
};

sockaddr_in ToSockAddr(const Endpoint& ep) {
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ep.ipv4_be;
    sa.sin_port = ep.port_be;
    return sa;
}

bool ResolveIpv4(const std::string& host, uint16_t port, Endpoint& out) {
    if (host.empty()) return false;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return false;
    }
    bool found = false;
    for (addrinfo* cur = res; cur != nullptr; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET && cur->ai_addrlen >= sizeof(sockaddr_in)) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(cur->ai_addr);
            out.ipv4_be = sin->sin_addr.s_addr;
            out.port_be = sin->sin_port;
            found = true;
            break;
        }
    }
    freeaddrinfo(res);
    return found;
}

struct PendingReliable {
    uint32_t seq = 0;
    std::vector<uint8_t> payload;
    uint64_t last_sent_ms = 0;
};

struct PeerLink {
    netcore::PeerId id = netcore::kInvalidPeer;
    Endpoint endpoint{};
    uint64_t last_recv_ms = 0;
    uint64_t last_send_ms = 0;

    uint32_t next_send_seq = 1;
    std::vector<PendingReliable> pending_reliable;

    uint32_t recv_next_seq = 1;
    std::map<uint32_t, std::vector<uint8_t>> recv_reorder;
    bool needs_ack = false;
};

}  // namespace

struct UdpTransport::Impl {
    WsaRuntime wsa;
    NativeSocket sock = kBadSocket;
    bool is_host = false;
    uint16_t local_port = 0;
    UdpTransportConfig config{};

    netcore::PeerId host_peer_id = 1;
    netcore::PeerId next_peer_id = 1;
    std::unordered_map<netcore::PeerId, PeerLink> peers;
    std::unordered_map<Endpoint, netcore::PeerId, EndpointHash> endpoint_to_peer;

    uint32_t drop_next_outgoing = 0;
    uint64_t dropped_for_test = 0;
    uint64_t retransmissions = 0;

    ~Impl() { CloseNativeSocket(sock); }

    bool open_socket(const Endpoint& bind_ep) {
        if (!wsa.ok()) return false;
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kBadSocket) return false;
        if (!SetNonBlocking(sock)) {
            CloseNativeSocket(sock);
            sock = kBadSocket;
            return false;
        }

        sockaddr_in sa = ToSockAddr(bind_ep);
        if (bind(sock, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
            CloseNativeSocket(sock);
            sock = kBadSocket;
            return false;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int addr_len = sizeof(bound);
#else
        socklen_t addr_len = sizeof(bound);
#endif
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &addr_len) == 0) {
            local_port = ntohs(bound.sin_port);
        }
        return true;
    }

    uint32_t compute_ack_bits(const PeerLink& p, uint32_t ack) const {
        uint32_t bits = 0;
        for (const auto& kv : p.recv_reorder) {
            if (kv.first > ack) {
                const uint32_t diff = kv.first - ack - 1;
                if (diff < 32) bits |= (1u << diff);
            }
        }
        return bits;
    }

    bool send_raw(PeerLink& p, WireKind kind, uint32_t seq, std::span<const uint8_t> payload, uint64_t now_ms) {
        if (sock == kBadSocket || payload.size() > netcore::kMaxPacketSize) return false;

        if (drop_next_outgoing > 0) {
            --drop_next_outgoing;
            ++dropped_for_test;
            p.last_send_ms = now_ms;
            p.needs_ack = false;
            return true;
        }

        const uint32_t ack = p.recv_next_seq > 1 ? (p.recv_next_seq - 1) : 0u;
        const uint32_t ack_bits = compute_ack_bits(p, ack);

        uint8_t buf[kMaxWireDatagram]{};
        WriteU16(buf + 0, kWireMagic);
        buf[2] = static_cast<uint8_t>(kind);
        buf[3] = 0;
        WriteU32(buf + 4, seq);
        WriteU32(buf + 8, ack);
        WriteU32(buf + 12, ack_bits);
        WriteU16(buf + 16, static_cast<uint16_t>(payload.size()));
        if (!payload.empty()) {
            std::memcpy(buf + kWireHeaderSize, payload.data(), payload.size());
        }

        const sockaddr_in sa = ToSockAddr(p.endpoint);
        const int total_len = static_cast<int>(kWireHeaderSize + payload.size());
#ifdef _WIN32
        const int sent = sendto(sock, reinterpret_cast<const char*>(buf), total_len, 0,
                                reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#else
        const ssize_t sent = sendto(sock, buf, static_cast<size_t>(total_len), 0,
                                    reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));
#endif
        p.last_send_ms = now_ms;
        p.needs_ack = false;
        return sent == total_len;
    }

    void process_acks(PeerLink& p, uint32_t ack, uint32_t ack_bits) {
        auto& q = p.pending_reliable;
        q.erase(std::remove_if(q.begin(), q.end(),
                               [&](const PendingReliable& item) {
                                   if (item.seq <= ack) return true;
                                   const uint32_t diff = item.seq - ack - 1;
                                   return diff < 32 && ((ack_bits >> diff) & 1u) != 0;
                               }),
                q.end());
    }

    void erase_peer(netcore::PeerId peer_id) {
        const auto it = peers.find(peer_id);
        if (it == peers.end()) return;
        endpoint_to_peer.erase(it->second.endpoint);
        peers.erase(it);
    }
};

UdpTransport::UdpTransport(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
UdpTransport::~UdpTransport() = default;

std::unique_ptr<UdpTransport> UdpTransport::CreateHost(
    uint16_t bind_port,
    const std::string& bind_addr,
    UdpTransportConfig config) {
    auto impl = std::make_unique<Impl>();
    impl->is_host = true;
    impl->config = config;

    Endpoint ep{};
    if (!ResolveIpv4(bind_addr.empty() ? "0.0.0.0" : bind_addr, bind_port, ep)) {
        return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
    }
    impl->open_socket(ep);
    return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
}

std::unique_ptr<UdpTransport> UdpTransport::CreateClient(
    const std::string& remote_host,
    uint16_t remote_port,
    uint16_t local_bind_port,
    UdpTransportConfig config) {
    auto impl = std::make_unique<Impl>();
    impl->is_host = false;
    impl->config = config;

    Endpoint remote_ep{};
    if (!ResolveIpv4(remote_host, remote_port, remote_ep)) {
        return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
    }
    Endpoint bind_ep{};
    if (!ResolveIpv4("0.0.0.0", local_bind_port, bind_ep)) {
        return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
    }
    if (!impl->open_socket(bind_ep)) {
        return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
    }

    const uint64_t now = NowMs();
    PeerLink link{};
    link.id = impl->host_peer_id;
    link.endpoint = remote_ep;
    link.last_recv_ms = now;
    link.last_send_ms = now;
    impl->endpoint_to_peer[remote_ep] = link.id;
    impl->peers.emplace(link.id, std::move(link));

    return std::unique_ptr<UdpTransport>(new UdpTransport(std::move(impl)));
}

bool UdpTransport::is_valid() const {
    return impl_ && impl_->sock != kBadSocket;
}

bool UdpTransport::is_host() const {
    return impl_ && impl_->is_host;
}

uint16_t UdpTransport::local_port() const {
    return impl_ ? impl_->local_port : 0;
}

netcore::PeerId UdpTransport::host_peer_id() const {
    return impl_ ? impl_->host_peer_id : netcore::kInvalidPeer;
}

std::size_t UdpTransport::active_peer_count() const {
    return impl_ ? impl_->peers.size() : 0;
}

bool UdpTransport::send(netcore::PeerId peer, netcore::Channel channel, std::span<const uint8_t> data) {
    if (!is_valid() || data.size() > netcore::kMaxPacketSize) return false;
    const auto it = impl_->peers.find(peer);
    if (it == impl_->peers.end()) return false;

    const uint64_t now = NowMs();
    PeerLink& p = it->second;
    if (channel == netcore::Channel::Reliable) {
        const uint32_t seq = p.next_send_seq++;
        const bool ok = impl_->send_raw(p, WireKind::Reliable, seq, data, now);
        p.pending_reliable.push_back(PendingReliable{
            .seq = seq,
            .payload = std::vector<uint8_t>(data.begin(), data.end()),
            .last_sent_ms = now,
        });
        return ok;
    }
    return impl_->send_raw(p, WireKind::Unreliable, 0, data, now);
}

void UdpTransport::broadcast(netcore::Channel channel, std::span<const uint8_t> data) {
    if (!is_valid() || data.size() > netcore::kMaxPacketSize) return;
    std::vector<netcore::PeerId> ids;
    ids.reserve(impl_->peers.size());
    for (const auto& kv : impl_->peers) ids.push_back(kv.first);
    for (netcore::PeerId id : ids) {
        send(id, channel, data);
    }
}

void UdpTransport::disconnect(netcore::PeerId peer) {
    if (!is_valid()) return;
    const auto it = impl_->peers.find(peer);
    if (it == impl_->peers.end()) return;
    impl_->send_raw(it->second, WireKind::Disconnect, 0, {}, NowMs());
    impl_->erase_peer(peer);
}

std::vector<netcore::TransportEvent> UdpTransport::poll() {
    std::vector<netcore::TransportEvent> events;
    if (!is_valid()) return events;

    const uint64_t now = NowMs();
    uint8_t buf[kMaxWireDatagram + 64]{};

    while (true) {
        sockaddr_in from{};
#ifdef _WIN32
        int from_len = sizeof(from);
        const int n = recvfrom(impl_->sock, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &from_len);
#else
        socklen_t from_len = sizeof(from);
        const ssize_t n = recvfrom(impl_->sock, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &from_len);
#endif
        if (n < 0) {
            if (WouldBlockError()) break;
            break;
        }

        const std::size_t bytes = static_cast<std::size_t>(n);
        if (bytes < kWireHeaderSize || bytes > kMaxWireDatagram) continue;
        if (ReadU16(buf + 0) != kWireMagic) continue;

        const uint8_t kind_raw = buf[2];
        const uint32_t seq = ReadU32(buf + 4);
        const uint32_t ack = ReadU32(buf + 8);
        const uint32_t ack_bits = ReadU32(buf + 12);
        const uint16_t payload_len = ReadU16(buf + 16);
        if (kWireHeaderSize + static_cast<std::size_t>(payload_len) != bytes) continue;
        if (payload_len > netcore::kMaxPacketSize) continue;

        const Endpoint ep{from.sin_addr.s_addr, from.sin_port};
        PeerLink* peer = nullptr;

        const auto ep_it = impl_->endpoint_to_peer.find(ep);
        if (ep_it != impl_->endpoint_to_peer.end()) {
            const auto p_it = impl_->peers.find(ep_it->second);
            if (p_it != impl_->peers.end()) peer = &p_it->second;
        } else if (impl_->is_host) {
            if (kind_raw == static_cast<uint8_t>(WireKind::Disconnect)) continue;
            if (impl_->peers.size() >= impl_->config.max_peers) continue;

            const netcore::PeerId new_id = impl_->next_peer_id++;
            PeerLink link{};
            link.id = new_id;
            link.endpoint = ep;
            link.last_recv_ms = now;
            link.last_send_ms = now;
            impl_->endpoint_to_peer[ep] = new_id;
            peer = &impl_->peers.emplace(new_id, std::move(link)).first->second;
            events.push_back(netcore::TransportEvent{
                .type = netcore::TransportEventType::Connected,
                .peer = new_id,
                .channel = netcore::Channel::Reliable,
                .payload = {},
            });
        } else {
            // Cliente descarta datagramas que não vieram do endpoint do host
            continue;
        }

        if (!peer) continue;
        peer->last_recv_ms = now;
        impl_->process_acks(*peer, ack, ack_bits);

        const uint8_t* payload_ptr = buf + kWireHeaderSize;
        if (kind_raw == static_cast<uint8_t>(WireKind::Disconnect)) {
            const netcore::PeerId disconnected_id = peer->id;
            impl_->erase_peer(disconnected_id);
            events.push_back(netcore::TransportEvent{
                .type = netcore::TransportEventType::Disconnected,
                .peer = disconnected_id,
                .channel = netcore::Channel::Reliable,
                .payload = {},
            });
            continue;
        }

        if (kind_raw == static_cast<uint8_t>(WireKind::Ack)) {
            continue;
        }

        if (kind_raw == static_cast<uint8_t>(WireKind::Unreliable)) {
            if (payload_len > 0) {
                events.push_back(netcore::TransportEvent{
                    .type = netcore::TransportEventType::Packet,
                    .peer = peer->id,
                    .channel = netcore::Channel::Unreliable,
                    .payload = std::vector<uint8_t>(payload_ptr, payload_ptr + payload_len),
                });
            }
            continue;
        }

        if (kind_raw == static_cast<uint8_t>(WireKind::Reliable)) {
            if (payload_len == 0 || seq == 0) continue;
            if (seq > peer->recv_next_seq + static_cast<uint32_t>(impl_->config.max_reorder_buffer)) continue;

            peer->needs_ack = true;
            if (seq < peer->recv_next_seq) {
                continue;  // duplicado já entregue
            }
            if (seq == peer->recv_next_seq) {
                events.push_back(netcore::TransportEvent{
                    .type = netcore::TransportEventType::Packet,
                    .peer = peer->id,
                    .channel = netcore::Channel::Reliable,
                    .payload = std::vector<uint8_t>(payload_ptr, payload_ptr + payload_len),
                });
                ++peer->recv_next_seq;
                while (true) {
                    const auto next_it = peer->recv_reorder.find(peer->recv_next_seq);
                    if (next_it == peer->recv_reorder.end()) break;
                    events.push_back(netcore::TransportEvent{
                        .type = netcore::TransportEventType::Packet,
                        .peer = peer->id,
                        .channel = netcore::Channel::Reliable,
                        .payload = std::move(next_it->second),
                    });
                    peer->recv_reorder.erase(next_it);
                    ++peer->recv_next_seq;
                }
            } else if (peer->recv_reorder.size() < impl_->config.max_reorder_buffer) {
                peer->recv_reorder.try_emplace(
                    seq, std::vector<uint8_t>(payload_ptr, payload_ptr + payload_len));
            }
        }
    }

    // Retransmissões confiáveis, ACKs, keepalives e detecção de timeout
    std::vector<netcore::PeerId> timed_out;
    for (auto& kv : impl_->peers) {
        PeerLink& p = kv.second;
        if (now - p.last_recv_ms > impl_->config.peer_timeout_ms) {
            timed_out.push_back(kv.first);
            continue;
        }
        for (auto& pend : p.pending_reliable) {
            if (now - pend.last_sent_ms >= impl_->config.retransmit_interval_ms) {
                impl_->send_raw(p, WireKind::Reliable, pend.seq, pend.payload, now);
                pend.last_sent_ms = now;
                ++impl_->retransmissions;
            }
        }
        if (p.needs_ack || (now - p.last_send_ms >= impl_->config.keepalive_interval_ms)) {
            impl_->send_raw(p, WireKind::Ack, 0, {}, now);
        }
    }

    for (netcore::PeerId dead_id : timed_out) {
        impl_->erase_peer(dead_id);
        events.push_back(netcore::TransportEvent{
            .type = netcore::TransportEventType::Disconnected,
            .peer = dead_id,
            .channel = netcore::Channel::Reliable,
            .payload = {},
        });
    }

    return events;
}

void UdpTransport::drop_next_outgoing_for_test(uint32_t count) {
    if (impl_) impl_->drop_next_outgoing = count;
}

uint64_t UdpTransport::packets_dropped_for_test() const {
    return impl_ ? impl_->dropped_for_test : 0;
}

uint64_t UdpTransport::retransmissions_for_test() const {
    return impl_ ? impl_->retransmissions : 0;
}

}  // namespace mp
