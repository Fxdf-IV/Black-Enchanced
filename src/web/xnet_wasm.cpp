#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <random>

#include "xdk/hle.h"

using kernel::HostPtr;
using kernel::W32;

namespace xnet {

void CloseSockets() {
    // WebRTC gerencia conexões reais via src/mp/mp_webrtc.cpp; sockets XNet HLE permanecem offline.
}

}  // namespace xnet

namespace {

thread_local uint32_t t_error = 0;
constexpr uint32_t kSocketError = 0xFFFFFFFFu;
constexpr uint32_t kWsaEWouldBlock = 10035u;
constexpr uint32_t kWsaENotSock = 10038u;
constexpr uint32_t kWsaEConnReset = 10054u;

uint32_t Fail(uint32_t err) {
    t_error = err;
    return kSocketError;
}

}  // namespace

HLE_STDCALL(XNetStartup, 1) { return 0; }
HLE_STDCALL(WSAStartup, 2) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 0x190);
    return 0;
}
HLE_STDCALL(XNetGetEthernetLinkStatus, 0) { return 0; }  // sem link LAN legado (multiplayer via mp_bridge/WebRTC)
HLE_STDCALL(XOnlineStartup, 1) { return 0; }
HLE_STDCALL(XOnlineCleanup, 0) { return 0; }
HLE_STDCALL(WSAGetLastError, 0) { return t_error; }

HLE_STDCALL(XNetRandom, 2) {
    static std::mutex m;
    static std::mt19937 rng{0x424C4143u};
    std::lock_guard<std::mutex> lock(m);
    if (a[0] && a[1]) {
        auto* p = static_cast<uint8_t*>(HostPtr(a[0]));
        for (uint32_t i = 0; i < a[1]; ++i) p[i] = static_cast<uint8_t>(rng());
    }
    return 0;
}

HLE_STDCALL(XNetCreateKey, 2) {
    if (a[0]) std::memset(HostPtr(a[0]), 0x11, 8);
    if (a[1]) std::memset(HostPtr(a[1]), 0x22, 16);
    return 0;
}
HLE_STDCALL(XNetRegisterKey, 2) { return 0; }
HLE_STDCALL(XNetUnregisterKey, 1) { return 0; }

HLE_STDCALL(XNetGetTitleXnAddr, 1) {
    if (a[0]) {
        auto* x = static_cast<uint8_t*>(HostPtr(a[0]));
        std::memset(x, 0, 36);
        x[0] = 127;
        x[1] = 0;
        x[2] = 0;
        x[3] = 1;
    }
    return 4;  // XNET_GET_XNADDR_STATIC
}

HLE_STDCALL(XNetXnAddrToInAddr, 3) {
    if (a[0] && a[2]) {
        uint32_t ip = 0;
        std::memcpy(&ip, HostPtr(a[0]), 4);
        W32(a[2], ip ? ip : 0x0100007Fu);
    }
    return 0;
}

HLE_STDCALL(XNetInAddrToXnAddr, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 36);
    if (a[2]) std::memset(HostPtr(a[2]), 0, 8);
    return 0;
}

HLE_STDCALL(XNetDnsLookup, 3) {
    (void)a;
    return 1;
}
HLE_STDCALL(XNetDnsRelease, 1) { return 0; }

HLE_STDCALL(socket, 3) { return Fail(kWsaEConnReset); }
HLE_STDCALL(closesocket, 1) { return Fail(kWsaENotSock); }
HLE_STDCALL(shutdown, 2) { return Fail(kWsaENotSock); }
HLE_STDCALL(ioctlsocket, 3) { return Fail(kWsaENotSock); }
HLE_STDCALL(setsockopt, 5) { return Fail(kWsaENotSock); }
HLE_STDCALL(getsockopt, 5) { return Fail(kWsaENotSock); }
HLE_STDCALL(getsockname, 3) { return Fail(kWsaENotSock); }
HLE_STDCALL(bind, 3) { return Fail(kWsaENotSock); }
HLE_STDCALL(connect, 3) { return Fail(kWsaENotSock); }
HLE_STDCALL(listen, 2) { return Fail(kWsaENotSock); }
HLE_STDCALL(accept, 3) { return Fail(kWsaENotSock); }
HLE_STDCALL(select, 5) { return 0; }
HLE_STDCALL(recvfrom, 6) { return Fail(kWsaEWouldBlock); }
HLE_STDCALL(recv, 4) { return Fail(kWsaEWouldBlock); }
HLE_STDCALL(send, 4) { return Fail(kWsaENotSock); }
HLE_STDCALL(sendto, 6) { return Fail(kWsaENotSock); }
HLE_STDCALL(WSARecvFrom, 9) { return Fail(kWsaEWouldBlock); }
HLE_STDCALL(WSARecv, 7) { return Fail(kWsaEWouldBlock); }
HLE_STDCALL(WSASendTo, 9) { return Fail(kWsaENotSock); }
HLE_STDCALL(WSASend, 7) { return Fail(kWsaENotSock); }
HLE_STDCALL(WSAGetOverlappedResult, 5) { return 0; }
HLE_STDCALL(WSACancelOverlappedIO, 1) { return 0; }
