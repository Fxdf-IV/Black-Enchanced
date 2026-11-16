#include "netcore/protocol.hpp"

#include <cmath>
#include <type_traits>

#include "netcore/buffer.hpp"

namespace netcore {

namespace {

template <class T>
struct Traits;
#define NETCORE_TRAIT(T)                                \
    template <>                                         \
    struct Traits<T> {                                  \
        static constexpr MessageType type = MessageType::T; \
    }
NETCORE_TRAIT(Hello);
NETCORE_TRAIT(Welcome);
NETCORE_TRAIT(Reject);
NETCORE_TRAIT(MatchStart);
NETCORE_TRAIT(MatchEnd);
NETCORE_TRAIT(PlayerState);
NETCORE_TRAIT(WorldSnapshot);
NETCORE_TRAIT(FireEvent);
NETCORE_TRAIT(DamageEvent);
NETCORE_TRAIT(DeathEvent);
NETCORE_TRAIT(RespawnEvent);
NETCORE_TRAIT(Ping);
NETCORE_TRAIT(Pong);
NETCORE_TRAIT(Leave);
NETCORE_TRAIT(Kick);
NETCORE_TRAIT(LoadoutRequest);
NETCORE_TRAIT(LoadoutState);
#undef NETCORE_TRAIT

// ---- validação -----------------------------------------------------------------------------

bool finite(float f) { return std::isfinite(f); }

bool valid_position(const Vec3& v) {
    return finite(v.x) && finite(v.y) && finite(v.z) && std::fabs(v.x) <= kWorldLimit &&
           std::fabs(v.y) <= kWorldLimit && std::fabs(v.z) <= kWorldLimit;
}

bool valid_velocity(const Vec3& v) {
    return finite(v.x) && finite(v.y) && finite(v.z) && std::fabs(v.x) <= kVelocityLimit &&
           std::fabs(v.y) <= kVelocityLimit && std::fabs(v.z) <= kVelocityLimit;
}

bool valid_direction(const Vec3& v) {
    if (!finite(v.x) || !finite(v.y) || !finite(v.z)) return false;
    const float len2 = v.x * v.x + v.y * v.y + v.z * v.z;
    return len2 >= 0.81f && len2 <= 1.21f;
}

bool valid_angle(float a) { return finite(a) && std::fabs(a) <= kAngleLimit; }
bool valid_player(uint8_t id) { return id < kMaxPlayers; }
bool valid_player_or_none(uint8_t id) { return id < kMaxPlayers || id == kNoPlayer; }
bool valid_weapon(uint8_t w) { return w < kMaxWeapons; }
bool valid_weapon_or_none(uint8_t w) { return w < kMaxWeapons || w == kNoWeapon; }
bool valid_tactical_flags(uint8_t f) { return (f & ~(kLoadoutArmor | kLoadoutTactical)) == 0; }

bool valid_name(const std::string& s) {
    if (s.empty() || s.size() > kMaxNameLength) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

bool valid(const Hello& m) { return valid_name(m.name); }
bool valid(const Welcome& m) {
    return valid_player(m.player_id) && m.max_players >= 1 && m.max_players <= kMaxPlayers && m.tick_rate_hz >= 1 &&
           m.tick_rate_hz <= 240 && m.snapshot_rate_hz >= 1 && m.snapshot_rate_hz <= 240;
}
bool valid(const Reject& m) {
    return m.reason >= RejectReason::ProtocolMismatch && m.reason <= RejectReason::NotAccepting;
}
bool valid(const MatchStart&) { return true; }
bool valid(const MatchEnd& m) { return valid_player_or_none(m.winner); }
bool valid(const PlayerState& m) {
    return valid_position(m.position) && valid_angle(m.yaw) && valid_angle(m.pitch) && valid_velocity(m.velocity) &&
           valid_weapon(m.weapon);
}
bool valid(const SnapshotPlayer& m) {
    return valid_player(m.player_id) && valid_position(m.position) && valid_angle(m.yaw) && valid_angle(m.pitch) &&
           valid_velocity(m.velocity) && valid_weapon(m.weapon);
}
bool valid(const WorldSnapshot& m) {
    if (m.phase > MatchPhase::Ended || m.players.size() > kMaxPlayers) return false;
    bool seen[kMaxPlayers] = {};
    for (const auto& p : m.players) {
        if (!valid(p) || seen[p.player_id]) return false;
        seen[p.player_id] = true;
    }
    return true;
}
bool valid(const FireEvent& m) {
    return valid_player(m.shooter) && valid_weapon(m.weapon) && valid_position(m.origin) &&
           valid_direction(m.direction) && valid_player_or_none(m.claimed_target) && m.claimed_target != m.shooter;
}
bool valid(const DamageEvent& m) {
    return valid_player(m.attacker) && valid_player(m.target) && m.attacker != m.target;
}
bool valid(const DeathEvent& m) { return valid_player(m.victim) && valid_player_or_none(m.killer) && valid_weapon(m.weapon); }
bool valid(const RespawnEvent& m) {
    return valid_player(m.player) && m.spawn_point < kMaxSpawnPoints && valid_position(m.position) &&
           valid_angle(m.yaw);
}
bool valid(const Ping&) { return true; }
bool valid(const Pong&) { return true; }
bool valid(const Leave&) { return true; }
bool valid(const Kick& m) { return m.reason >= KickReason::HostLeft && m.reason <= KickReason::HandshakeTimeout; }
bool valid(const LoadoutRequest& m) {
    return valid_weapon_or_none(m.primary_weapon) && valid_weapon_or_none(m.secondary_weapon) &&
           valid_tactical_flags(m.tactical_flags);
}
bool valid(const LoadoutState& m) {
    return valid_player(m.player_id) && valid_weapon_or_none(m.primary_weapon) &&
           valid_weapon_or_none(m.secondary_weapon) && valid_tactical_flags(m.tactical_flags) &&
           m.last_result <= static_cast<uint8_t>(LoadoutResult::NotAliveOrEnded);
}

// ---- escrita -------------------------------------------------------------------------------

void put(ByteWriter& w, const Vec3& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
}

void write(ByteWriter& w, const Hello& m) {
    w.u16(m.protocol_version);
    w.u32(m.rules_hash);
    w.u8(static_cast<uint8_t>(m.name.size()));
    w.bytes({reinterpret_cast<const uint8_t*>(m.name.data()), m.name.size()});
}
void write(ByteWriter& w, const Welcome& m) {
    w.u8(m.player_id);
    w.u8(m.max_players);
    w.u16(m.tick_rate_hz);
    w.u16(m.snapshot_rate_hz);
}
void write(ByteWriter& w, const Reject& m) { w.u8(static_cast<uint8_t>(m.reason)); }
void write(ByteWriter& w, const MatchStart& m) {
    w.u32(m.start_tick);
    w.u16(m.time_limit_s);
    w.u16(m.kill_limit);
    w.u8(m.arena_id);
}
void write(ByteWriter& w, const MatchEnd& m) { w.u8(m.winner); }
void write(ByteWriter& w, const PlayerState& m) {
    w.u32(m.sequence);
    w.u32(m.client_tick);
    put(w, m.position);
    w.f32(m.yaw);
    w.f32(m.pitch);
    put(w, m.velocity);
    w.u8(m.anim_state);
    w.u8(m.weapon);
    w.u8(m.flags);
}
void write(ByteWriter& w, const SnapshotPlayer& m) {
    w.u8(m.player_id);
    put(w, m.position);
    w.f32(m.yaw);
    w.f32(m.pitch);
    put(w, m.velocity);
    w.u8(m.anim_state);
    w.u8(m.weapon);
    w.u8(m.health);
    w.u8(m.flags);
    w.i16(m.kills);
    w.i16(m.deaths);
}
void write(ByteWriter& w, const WorldSnapshot& m) {
    w.u32(m.tick);
    w.u16(m.time_left_s);
    w.u8(static_cast<uint8_t>(m.phase));
    w.u8(static_cast<uint8_t>(m.players.size()));
    for (const auto& p : m.players) write(w, p);
}
void write(ByteWriter& w, const FireEvent& m) {
    w.u32(m.client_tick);
    w.u8(m.shooter);
    w.u8(m.weapon);
    put(w, m.origin);
    put(w, m.direction);
    w.u8(m.claimed_target);
}
void write(ByteWriter& w, const DamageEvent& m) {
    w.u8(m.attacker);
    w.u8(m.target);
    w.u16(m.amount);
    w.u8(m.health_after);
}
void write(ByteWriter& w, const DeathEvent& m) {
    w.u8(m.victim);
    w.u8(m.killer);
    w.u8(m.weapon);
}
void write(ByteWriter& w, const RespawnEvent& m) {
    w.u8(m.player);
    w.u8(m.spawn_point);
    put(w, m.position);
    w.f32(m.yaw);
}
void write(ByteWriter& w, const Ping& m) {
    w.u32(m.nonce);
    w.u32(m.sent_ms);
}
void write(ByteWriter& w, const Pong& m) {
    w.u32(m.nonce);
    w.u32(m.echo_ms);
    w.u32(m.host_tick);
}
void write(ByteWriter& w, const Leave& m) { w.u8(m.reason); }
void write(ByteWriter& w, const Kick& m) { w.u8(static_cast<uint8_t>(m.reason)); }
void write(ByteWriter& w, const LoadoutRequest& m) {
    w.u8(m.primary_weapon);
    w.u8(m.secondary_weapon);
    w.u8(m.tactical_flags);
}
void write(ByteWriter& w, const LoadoutState& m) {
    w.u8(m.player_id);
    w.u8(m.primary_weapon);
    w.u8(m.secondary_weapon);
    w.u8(m.tactical_flags);
    w.u8(m.last_result);
    w.u16(m.credits);
    w.u16(m.max_credits);
    w.u16(m.kit_cost);
    w.u16(m.buy_ticks_left);
}

// ---- leitura -------------------------------------------------------------------------------
// Cada read() devolve false para conteúdo estruturalmente impossível (contagem acima do teto etc.).
// Erros de tamanho ficam registrados no próprio ByteReader.

Vec3 get_vec3(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

bool read(ByteReader& r, Hello& m) {
    m.protocol_version = r.u16();
    m.rules_hash = r.u32();
    const uint8_t len = r.u8();
    if (len > kMaxNameLength) return false;
    auto s = r.bytes(len);
    m.name.assign(reinterpret_cast<const char*>(s.data()), s.size());
    return true;
}
bool read(ByteReader& r, Welcome& m) {
    m.player_id = r.u8();
    m.max_players = r.u8();
    m.tick_rate_hz = r.u16();
    m.snapshot_rate_hz = r.u16();
    return true;
}
bool read(ByteReader& r, Reject& m) {
    m.reason = static_cast<RejectReason>(r.u8());
    return true;
}
bool read(ByteReader& r, MatchStart& m) {
    m.start_tick = r.u32();
    m.time_limit_s = r.u16();
    m.kill_limit = r.u16();
    m.arena_id = r.u8();
    return true;
}
bool read(ByteReader& r, MatchEnd& m) {
    m.winner = r.u8();
    return true;
}
bool read(ByteReader& r, PlayerState& m) {
    m.sequence = r.u32();
    m.client_tick = r.u32();
    m.position = get_vec3(r);
    m.yaw = r.f32();
    m.pitch = r.f32();
    m.velocity = get_vec3(r);
    m.anim_state = r.u8();
    m.weapon = r.u8();
    m.flags = r.u8();
    return true;
}
bool read(ByteReader& r, SnapshotPlayer& m) {
    m.player_id = r.u8();
    m.position = get_vec3(r);
    m.yaw = r.f32();
    m.pitch = r.f32();
    m.velocity = get_vec3(r);
    m.anim_state = r.u8();
    m.weapon = r.u8();
    m.health = r.u8();
    m.flags = r.u8();
    m.kills = r.i16();
    m.deaths = r.i16();
    return true;
}
bool read(ByteReader& r, WorldSnapshot& m) {
    m.tick = r.u32();
    m.time_left_s = r.u16();
    m.phase = static_cast<MatchPhase>(r.u8());
    const uint8_t count = r.u8();
    if (count > kMaxPlayers) return false;
    m.players.resize(count);
    for (auto& p : m.players) read(r, p);
    return true;
}
bool read(ByteReader& r, FireEvent& m) {
    m.client_tick = r.u32();
    m.shooter = r.u8();
    m.weapon = r.u8();
    m.origin = get_vec3(r);
    m.direction = get_vec3(r);
    m.claimed_target = r.u8();
    return true;
}
bool read(ByteReader& r, DamageEvent& m) {
    m.attacker = r.u8();
    m.target = r.u8();
    m.amount = r.u16();
    m.health_after = r.u8();
    return true;
}
bool read(ByteReader& r, DeathEvent& m) {
    m.victim = r.u8();
    m.killer = r.u8();
    m.weapon = r.u8();
    return true;
}
bool read(ByteReader& r, RespawnEvent& m) {
    m.player = r.u8();
    m.spawn_point = r.u8();
    m.position = get_vec3(r);
    m.yaw = r.f32();
    return true;
}
bool read(ByteReader& r, Ping& m) {
    m.nonce = r.u32();
    m.sent_ms = r.u32();
    return true;
}
bool read(ByteReader& r, Pong& m) {
    m.nonce = r.u32();
    m.echo_ms = r.u32();
    m.host_tick = r.u32();
    return true;
}
bool read(ByteReader& r, Leave& m) {
    m.reason = r.u8();
    return true;
}
bool read(ByteReader& r, Kick& m) {
    m.reason = static_cast<KickReason>(r.u8());
    return true;
}
bool read(ByteReader& r, LoadoutRequest& m) {
    m.primary_weapon = r.u8();
    m.secondary_weapon = r.u8();
    m.tactical_flags = r.u8();
    return true;
}
bool read(ByteReader& r, LoadoutState& m) {
    m.player_id = r.u8();
    m.primary_weapon = r.u8();
    m.secondary_weapon = r.u8();
    m.tactical_flags = r.u8();
    m.last_result = r.u8();
    m.credits = r.u16();
    m.max_credits = r.u16();
    m.kit_cost = r.u16();
    m.buy_ticks_left = r.u16();
    return true;
}

template <class T>
DecodeResult finish(ByteReader& r) {
    T value{};
    const bool structural = read(r, value);
    if (!r.ok()) return {std::nullopt, DecodeError::Truncated};
    if (!structural) return {std::nullopt, DecodeError::InvalidValue};
    if (!r.at_end()) return {std::nullopt, DecodeError::TrailingBytes};
    if (!valid(value)) return {std::nullopt, DecodeError::InvalidValue};
    return {Message{std::move(value)}, DecodeError::None};
}

}  // namespace

MessageType type_of(const Message& m) {
    return std::visit([](const auto& v) { return Traits<std::decay_t<decltype(v)>>::type; }, m);
}

Channel channel_for(MessageType t) {
    switch (t) {
        case MessageType::PlayerState:
        case MessageType::WorldSnapshot:
        case MessageType::Ping:
        case MessageType::Pong:
            return Channel::Unreliable;
        default:
            return Channel::Reliable;
    }
}

bool is_valid(const Message& m) {
    return std::visit([](const auto& v) { return valid(v); }, m);
}

std::size_t encode(const Message& m, std::span<uint8_t> out) {
    if (!is_valid(m)) return 0;
    if (out.size() > kMaxPacketSize) out = out.first(kMaxPacketSize);
    ByteWriter w(out);
    w.u16(kPacketMagic);
    w.u8(static_cast<uint8_t>(type_of(m)));
    std::visit([&w](const auto& v) { write(w, v); }, m);
    return w.ok() ? w.size() : 0;
}

std::vector<uint8_t> encode(const Message& m) {
    std::vector<uint8_t> buf(kMaxPacketSize);
    buf.resize(encode(m, std::span<uint8_t>(buf)));
    return buf;
}

DecodeResult decode(std::span<const uint8_t> in) {
    if (in.size() > kMaxPacketSize) return {std::nullopt, DecodeError::TooLarge};
    if (in.size() < 3) return {std::nullopt, DecodeError::TooShort};
    ByteReader r(in);
    if (r.u16() != kPacketMagic) return {std::nullopt, DecodeError::BadMagic};
    switch (static_cast<MessageType>(r.u8())) {
        case MessageType::Hello: return finish<Hello>(r);
        case MessageType::Welcome: return finish<Welcome>(r);
        case MessageType::Reject: return finish<Reject>(r);
        case MessageType::MatchStart: return finish<MatchStart>(r);
        case MessageType::MatchEnd: return finish<MatchEnd>(r);
        case MessageType::PlayerState: return finish<PlayerState>(r);
        case MessageType::WorldSnapshot: return finish<WorldSnapshot>(r);
        case MessageType::FireEvent: return finish<FireEvent>(r);
        case MessageType::DamageEvent: return finish<DamageEvent>(r);
        case MessageType::DeathEvent: return finish<DeathEvent>(r);
        case MessageType::RespawnEvent: return finish<RespawnEvent>(r);
        case MessageType::Ping: return finish<Ping>(r);
        case MessageType::Pong: return finish<Pong>(r);
        case MessageType::Leave: return finish<Leave>(r);
        case MessageType::Kick: return finish<Kick>(r);
        case MessageType::LoadoutRequest: return finish<LoadoutRequest>(r);
        case MessageType::LoadoutState: return finish<LoadoutState>(r);
    }
    return {std::nullopt, DecodeError::UnknownType};
}

const char* to_string(DecodeError e) {
    switch (e) {
        case DecodeError::None: return "none";
        case DecodeError::TooShort: return "too short";
        case DecodeError::TooLarge: return "too large";
        case DecodeError::BadMagic: return "bad magic";
        case DecodeError::UnknownType: return "unknown type";
        case DecodeError::Truncated: return "truncated";
        case DecodeError::TrailingBytes: return "trailing bytes";
        case DecodeError::InvalidValue: return "invalid value";
    }
    return "?";
}

}  // namespace netcore
