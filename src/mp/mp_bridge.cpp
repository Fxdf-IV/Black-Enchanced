#include "mp/mp_bridge.h"

#include <memory>

#include "core/log.h"
#include "netcore/rules.hpp"
#include "netcore/session.hpp"
#include "netcore/transport.hpp"

namespace mp {

namespace {

Role g_role = Role::OfflineCampaign;
uint64_t g_frame_count = 0;
uint8_t* g_guest_base = nullptr;

// Transporte loopback de fallback quando em modo de teste local antes do backend ENet/WebRTC.
std::unique_ptr<netcore::LoopbackEndpoint> g_local_endpoint;
std::unique_ptr<netcore::HostSession> g_host_session;

}  // namespace

void InitBridge() {
    g_frame_count = 0;
    LOGI("mp_bridge: initialized (role=%u, protocol_v=%u)", static_cast<unsigned>(g_role),
         static_cast<unsigned>(netcore::kProtocolVersion));
}

void OnImageLoaded(uint8_t* guest_base) {
    g_guest_base = guest_base;
    LOGI("mp_bridge: guest image mapped at %p", static_cast<void*>(guest_base));
}

void OnPreFrame(uint8_t* /*guest_base*/) {
    if (g_role == Role::OfflineCampaign) return;
    if (g_role == Role::Host && g_host_session) {
        g_host_session->tick();
    }
}

void OnPostFrame(uint8_t* /*guest_base*/) {
    ++g_frame_count;
    if (g_frame_count == 1) {
        LOGI("mp_bridge: first guest frame hook fired (sub_0020F6A0 verified)");
    }
}

Role CurrentRole() {
    return g_role;
}

void SetRole(Role r) {
    g_role = r;
    if (g_role == Role::Host && !g_host_session) {
        g_local_endpoint = std::make_unique<netcore::LoopbackEndpoint>();
        g_host_session = std::make_unique<netcore::HostSession>(
            *g_local_endpoint, netcore::RulesConfig::default_deathmatch(), "Host");
        g_host_session->start_match();
    } else if (g_role == Role::OfflineCampaign) {
        g_host_session.reset();
        g_local_endpoint.reset();
    }
}

uint64_t FrameCount() {
    return g_frame_count;
}

}  // namespace mp
