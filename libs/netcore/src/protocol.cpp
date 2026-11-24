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
NETCORE_TRAIT(PlayerRoster);
NETCORE_TRAIT(BlastEvent);
NETCORE_TRAIT(TeamSelectRequest);
NETCORE_TRAIT(TerritorialState);
NETCORE_TRAIT(TerritorialEvent);
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
           m.tick_rate_hz <= 240 && m.snapshot_rate_hz >= 1 && m.snapshot_rate_hz <= 240 && m.game_mode <= 2u;
}
bool valid(const Reject& m) {
    return m.reason >= RejectReason::ProtocolMismatch && m.reason <= RejectReason::NotAccepting;
}
bool valid(const MatchStart& m) { return m.game_mode <= 2u; }
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
    return valid_player_or_none(m.attacker) && valid_player(m.target);
}
bool valid(const DeathEvent& m) { return valid_player(m.victim) && valid_player_or_none(m.killer) && valid_weapon_or_none(m.weapon); }
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
bool valid(const PlayerRoster& m) {
    if (m.players.size() > kMaxPlayers) return false;
    bool seen[kMaxPlayers] = {};
    for (const auto& entry : m.players) {
        if (!valid_player(entry.player_id) || !valid_name(entry.name) || seen[entry.player_id]) return false;
        seen[entry.player_id] = true;
    }
    return true;
}
bool valid(const BlastEvent& m) {
    return valid_player_or_none(m.attacker) && valid_player_or_none(m.target) && valid_weapon_or_none(m.weapon) &&
           (m.flags & ~uint8_t(kBlastFlagClaim | kBlastFlagVisualOnly)) == 0 && valid_position(m.origin) &&
           finite(m.damage) && m.damage >= 0.0f && m.damage <= 250.0f;
}
bool valid(const TeamSelectRequest& m) { return m.team <= 1u; }
bool valid(const TerritorialState& m) {
    if (m.match_state > 5u || m.attacker_team > 1u || m.objective_count < 1u ||
        m.objective_count > kMaxTerritorialObjectives || m.captured_count > m.objective_count ||
        m.current_objective > m.objective_count || m.current_obj_state > 5u || m.progress_permille > 1000u ||
        m.overtime > 1u || (m.winner_team > 1u && m.winner_team != kNoPlayer) ||
        m.r1_captured > m.objective_count || m.r2_captured > m.objective_count ||
        m.r1_progress_permille > 1000u || m.r2_progress_permille > 1000u ||
        !valid_position(m.active_zone_center) || !finite(m.active_zone_radius) ||
        m.active_zone_radius < 0.0f || m.active_zone_radius > 500.0f ||
        m.zone_attackers > kMaxPlayers || m.zone_defenders > kMaxPlayers) {
        return false;
    }
    for (uint8_t t : m.player_teams) {
        if (t > 1u && t != kNoPlayer) return false;
    }
    return true;
}
bool valid(const TerritorialEvent& m) {
    return m.event_type <= 4u && m.team <= 1u && m.index <= kMaxTerritorialObjectives;
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
    w.u8(m.game_mode);
    w.u8(m.arena_id);
    w.u32(m.available_weapon_mask);
}
void write(ByteWriter& w, const Reject& m) { w.u8(static_cast<uint8_t>(m.reason)); }
void write(ByteWriter& w, const MatchStart& m) {
    w.u32(m.start_tick);
    w.u16(m.time_limit_s);
    w.u16(m.kill_limit);
    w.u8(m.arena_id);
    w.u8(m.game_mode);
    w.u32(m.available_weapon_mask);
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
void write(ByteWriter& w, const PlayerRoster& m) {
    w.u8(static_cast<uint8_t>(m.players.size()));
    for (const auto& entry : m.players) {
        w.u8(entry.player_id);
        w.u8(static_cast<uint8_t>(entry.name.size()));
        w.bytes({reinterpret_cast<const uint8_t*>(entry.name.data()), entry.name.size()});
    }
}
void write(ByteWriter& w, const BlastEvent& m) {
    w.u32(m.client_tick);
    w.u8(m.attacker);
    w.u8(m.target);
    w.u8(m.weapon);
    w.u8(m.flags);
    put(w, m.origin);
    w.f32(m.damage);
}
void write(ByteWriter& w, const TeamSelectRequest& m) { w.u8(m.team); }
void write(ByteWriter& w, const TerritorialState& m) {
    w.u32(m.tick);
    w.u8(m.match_state);
    w.u8(m.attacker_team);
    w.u8(m.current_objective);
    w.u8(m.objective_count);
    w.u8(m.captured_count);
    w.u8(m.current_obj_state);
    w.u16(m.progress_permille);
    w.u16(m.time_left_s);
    w.u8(m.overtime);
    w.u16(m.attacker_waves);
    w.u16(m.defender_waves);
    w.u8(m.winner_team);
    for (uint8_t t : m.player_teams) w.u8(t);
    w.u8(m.r1_captured);
    w.u16(m.r1_progress_permille);
    w.u16(m.r1_time_s);
    w.u8(m.r2_captured);
    w.u16(m.r2_progress_permille);
    w.u16(m.r2_time_s);
    put(w, m.active_zone_center);
    w.f32(m.active_zone_radius);
    w.u8(m.atk_wave_eta_s);
    w.u8(m.def_wave_eta_s);
    w.u8(m.zone_attackers);
    w.u8(m.zone_defenders);
}
void write(ByteWriter& w, const TerritorialEvent& m) {
    w.u8(m.event_type);
    w.u8(m.team);
    w.u8(m.index);
    w.u16(m.count);
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
    m.game_mode = r.u8();
    m.arena_id = r.u8();
    m.available_weapon_mask = r.u32();
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
    m.game_mode = r.u8();
    m.available_weapon_mask = r.u32();
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
bool read(ByteReader& r, PlayerRoster& m) {
    const uint8_t count = r.u8();
    if (count > kMaxPlayers) return false;
    m.players.clear();
    m.players.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        PlayerRosterEntry entry;
        entry.player_id = r.u8();
        const uint8_t len = r.u8();
        if (len > kMaxNameLength) return false;
        auto s = r.bytes(len);
        entry.name.assign(reinterpret_cast<const char*>(s.data()), s.size());
        m.players.push_back(std::move(entry));
    }
    return true;
}
bool read(ByteReader& r, BlastEvent& m) {
    m.client_tick = r.u32();
    m.attacker = r.u8();
    m.target = r.u8();
    m.weapon = r.u8();
    m.flags = r.u8();
    m.origin = get_vec3(r);
    m.damage = r.f32();
    return true;
}
bool read(ByteReader& r, TeamSelectRequest& m) {
    m.team = r.u8();
    return true;
}
bool read(ByteReader& r, TerritorialState& m) {
    m.tick = r.u32();
    m.match_state = r.u8();
    m.attacker_team = r.u8();
    m.current_objective = r.u8();
    m.objective_count = r.u8();
    m.captured_count = r.u8();
    m.current_obj_state = r.u8();
    m.progress_permille = r.u16();
    m.time_left_s = r.u16();
    m.overtime = r.u8();
    m.attacker_waves = r.u16();
    m.defender_waves = r.u16();
    m.winner_team = r.u8();
    for (uint8_t& t : m.player_teams) t = r.u8();
    m.r1_captured = r.u8();
    m.r1_progress_permille = r.u16();
    m.r1_time_s = r.u16();
    m.r2_captured = r.u8();
    m.r2_progress_permille = r.u16();
    m.r2_time_s = r.u16();
    m.active_zone_center = get_vec3(r);
    m.active_zone_radius = r.f32();
    m.atk_wave_eta_s = r.u8();
    m.def_wave_eta_s = r.u8();
    m.zone_attackers = r.u8();
    m.zone_defenders = r.u8();
    return true;
}
bool read(ByteReader& r, TerritorialEvent& m) {
    m.event_type = r.u8();
    m.team = r.u8();
    m.index = r.u8();
    m.count = r.u16();
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
        case MessageType::TerritorialState:
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
        case MessageType::PlayerRoster: return finish<PlayerRoster>(r);
        case MessageType::BlastEvent: return finish<BlastEvent>(r);
        case MessageType::TeamSelectRequest: return finish<TeamSelectRequest>(r);
        case MessageType::TerritorialState: return finish<TerritorialState>(r);
        case MessageType::TerritorialEvent: return finish<TerritorialEvent>(r);
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
