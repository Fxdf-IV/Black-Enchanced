#pragma once
// Mensagens do protocolo (seção 9 do plano). O modelo replicado é nosso, não a memória do motor.
//
// Formato de todo pacote: u16 kPacketMagic | u8 MessageType | corpo.
// O corpo de Hello é CONGELADO (versão u16 primeiro) para que versões diferentes ainda consigam
// se recusar com uma mensagem clara.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "netcore/limits.hpp"

namespace netcore {

enum class MessageType : uint8_t {
    Hello = 1,
    Welcome = 2,
    Reject = 3,
    MatchStart = 4,
    MatchEnd = 5,
    PlayerState = 6,
    WorldSnapshot = 7,
    FireEvent = 8,
    DamageEvent = 9,
    DeathEvent = 10,
    RespawnEvent = 11,
    Ping = 12,
    Pong = 13,
    Leave = 14,
    Kick = 15,
    LoadoutRequest = 16,
    LoadoutState = 17,
    PlayerRoster = 18,
    BlastEvent = 19,
    TeamSelectRequest = 20,
    TerritorialState = 21,
    TerritorialEvent = 22,
};

// Os dois canais que todo transporte precisa oferecer (ENet: canais; WebRTC: dois data channels).
enum class Channel : uint8_t {
    Reliable = 0,    // confiável e ordenado
    Unreliable = 1,  // pode perder e desordenar; quem lê descarta o que for velho
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    friend bool operator==(const Vec3&, const Vec3&) = default;
};

enum class RejectReason : uint8_t {
    ProtocolMismatch = 1,
    RulesMismatch = 2,
    RoomFull = 3,
    Malformed = 4,
    NotAccepting = 5,
};

enum class KickReason : uint8_t {
    HostLeft = 1,
    RateLimited = 2,
    Malformed = 3,
    TooManyViolations = 4,
    HandshakeTimeout = 5,
};

enum class MatchPhase : uint8_t {
    Warmup = 0,
    Running = 1,
    Ended = 2,
};

// Bits de PlayerState::flags / SnapshotPlayer::flags.
enum PlayerFlags : uint8_t {
    kFlagAlive = 1 << 0,
    kFlagCrouched = 1 << 1,
    kFlagAiming = 1 << 2,
    kFlagReloading = 1 << 3,
    kFlagPaused = 1 << 4,
};

// cliente -> host, primeira mensagem. Layout congelado.
struct Hello {
    uint16_t protocol_version = kProtocolVersion;
    uint32_t rules_hash = 0;
    std::string name;
    friend bool operator==(const Hello&, const Hello&) = default;
};

// host -> cliente
struct Welcome {
    uint8_t player_id = 0;
    uint8_t max_players = kMaxPlayers;
    uint16_t tick_rate_hz = 30;
    uint16_t snapshot_rate_hz = 20;
    uint8_t game_mode = 0;  // 0=PvP, 1=Coop
    uint8_t arena_id = 0;
    uint32_t available_weapon_mask = 0xFFFFFFFFu;
    friend bool operator==(const Welcome&, const Welcome&) = default;
};

struct Reject {
    RejectReason reason = RejectReason::Malformed;
    friend bool operator==(const Reject&, const Reject&) = default;
};

struct MatchStart {
    uint32_t start_tick = 0;
    uint16_t time_limit_s = 0;
    uint16_t kill_limit = 0;
    uint8_t arena_id = 0;
    uint8_t game_mode = 0;
    uint32_t available_weapon_mask = 0xFFFFFFFFu;
    friend bool operator==(const MatchStart&, const MatchStart&) = default;
};

struct MatchEnd {
    uint8_t winner = kNoPlayer;  // kNoPlayer = empate
    friend bool operator==(const MatchEnd&, const MatchEnd&) = default;
};

// cliente -> host, não confiável. O cliente tem autoridade sobre a própria posição (MVP).
struct PlayerState {
    uint32_t sequence = 0;
    uint32_t client_tick = 0;
    Vec3 position;
    float yaw = 0, pitch = 0;
    Vec3 velocity;
    uint8_t anim_state = 0;
    uint8_t weapon = 0;
    uint8_t flags = 0;
    friend bool operator==(const PlayerState&, const PlayerState&) = default;
};

struct SnapshotPlayer {
    uint8_t player_id = 0;
    Vec3 position;
    float yaw = 0, pitch = 0;
    Vec3 velocity;
    uint8_t anim_state = 0;
    uint8_t weapon = 0;
    uint8_t health = 0;
    uint8_t flags = 0;
    int16_t kills = 0;
    int16_t deaths = 0;
    friend bool operator==(const SnapshotPlayer&, const SnapshotPlayer&) = default;
};

// host -> clientes, não confiável.
struct WorldSnapshot {
    uint32_t tick = 0;
    uint16_t time_left_s = 0;
    MatchPhase phase = MatchPhase::Warmup;
    std::vector<SnapshotPlayer> players;  // no máximo kMaxPlayers, ids únicos
    friend bool operator==(const WorldSnapshot&, const WorldSnapshot&) = default;
};

// cliente -> host (e depois host -> demais clientes), confiável. O cliente informa o alvo atingido.
struct FireEvent {
    uint32_t client_tick = 0;
    uint8_t shooter = 0;
    uint8_t weapon = 0;
    Vec3 origin;
    Vec3 direction;  // unitário
    uint8_t claimed_target = kNoPlayer;
    friend bool operator==(const FireEvent&, const FireEvent&) = default;
};

struct DamageEvent {
    uint8_t attacker = 0;
    uint8_t target = 0;
    uint16_t amount = 0;
    uint8_t health_after = 0;
    friend bool operator==(const DamageEvent&, const DamageEvent&) = default;
};

struct DeathEvent {
    uint8_t victim = 0;
    uint8_t killer = kNoPlayer;
    uint8_t weapon = 0;
    friend bool operator==(const DeathEvent&, const DeathEvent&) = default;
};

struct RespawnEvent {
    uint8_t player = 0;
    uint8_t spawn_point = 0;
    Vec3 position;
    float yaw = 0;
    friend bool operator==(const RespawnEvent&, const RespawnEvent&) = default;
};

struct Ping {
    uint32_t nonce = 0;
    uint32_t sent_ms = 0;
    friend bool operator==(const Ping&, const Ping&) = default;
};

struct Pong {
    uint32_t nonce = 0;
    uint32_t echo_ms = 0;
    uint32_t host_tick = 0;
    friend bool operator==(const Pong&, const Pong&) = default;
};

struct Leave {
    uint8_t reason = 0;
    friend bool operator==(const Leave&, const Leave&) = default;
};

struct Kick {
    KickReason reason = KickReason::Malformed;
    friend bool operator==(const Kick&, const Kick&) = default;
};

// Bits de compra tática no sistema de Loadout estilo CS.
enum LoadoutBuyFlags : uint8_t {
    kLoadoutArmor = 1 << 0,     // Colete balístico (+armor_bonus_health ao spawnar)
    kLoadoutTactical = 1 << 1,  // Pacote tático / granada / silenciador
};

enum class LoadoutResult : uint8_t {
    Ok = 0,
    InsufficientCredits = 1,
    ExceedsBudgetCap = 2,
    InvalidItem = 3,
    BuyWindowClosed = 4,
    NotAliveOrEnded = 5,
};

struct LoadoutRequest {
    uint8_t primary_weapon = kNoWeapon;
    uint8_t secondary_weapon = 0;
    uint8_t tactical_flags = 0;
    friend bool operator==(const LoadoutRequest&, const LoadoutRequest&) = default;
};

struct LoadoutState {
    uint8_t player_id = 0;
    uint8_t primary_weapon = kNoWeapon;
    uint8_t secondary_weapon = 0;
    uint8_t tactical_flags = 0;
    uint8_t last_result = 0;
    uint16_t credits = 0;
    uint16_t max_credits = 0;
    uint16_t kit_cost = 0;
    uint16_t buy_ticks_left = 0;
    friend bool operator==(const LoadoutState&, const LoadoutState&) = default;
};

struct PlayerRosterEntry {
    uint8_t player_id = 0;
    std::string name;
    friend bool operator==(const PlayerRosterEntry&, const PlayerRosterEntry&) = default;
};

struct PlayerRoster {
    std::vector<PlayerRosterEntry> players;
    friend bool operator==(const PlayerRoster&, const PlayerRoster&) = default;
};

enum BlastFlags : uint8_t {
    kBlastFlagClaim = 1 << 0,
    kBlastFlagVisualOnly = 1 << 1,
};

struct BlastEvent {
    uint32_t client_tick = 0;
    uint8_t attacker = kNoPlayer;
    uint8_t target = kNoPlayer;   // kNoPlayer = sem vítima direta / apenas explosão
    uint8_t weapon = kNoWeapon;   // 14=M203, 15=RPG-7, kNoWeapon=granada/barril
    uint8_t flags = 0;
    Vec3 origin;
    float damage = 0.0f;
    friend bool operator==(const BlastEvent&, const BlastEvent&) = default;
};

// cliente -> host (confiável): solicita escolha de equipe no Lobby do Territorial Assault (0=A, 1=B).
struct TeamSelectRequest {
    uint8_t team = 0;
    friend bool operator==(const TeamSelectRequest&, const TeamSelectRequest&) = default;
};

// host -> clientes (não confiável): estado autoritativo da partida Territorial Assault para o HUD.
struct TerritorialState {
    uint32_t tick = 0;
    uint8_t match_state = 0;        // ta::MatchState (0..5)
    uint8_t attacker_team = 0;      // 0=A, 1=B
    uint8_t current_objective = 0;  // 0..objective_count
    uint8_t objective_count = 1;    // 1..kMaxTerritorialObjectives
    uint8_t captured_count = 0;     // 0..objective_count
    uint8_t current_obj_state = 0;  // ta::ObjectiveState (0..5)
    uint16_t progress_permille = 0; // 0..1000
    uint16_t time_left_s = 0;
    uint8_t overtime = 0;           // 0 ou 1
    uint16_t attacker_waves = 0;
    uint16_t defender_waves = 0;
    uint8_t winner_team = kNoPlayer;  // 0=A, 1=B, kNoPlayer=nenhum/empate
    std::array<uint8_t, kMaxPlayers> player_teams = {kNoPlayer, kNoPlayer, kNoPlayer, kNoPlayer};
    uint8_t r1_captured = 0;
    uint16_t r1_progress_permille = 0;
    uint16_t r1_time_s = 0;
    uint8_t r2_captured = 0;
    uint16_t r2_progress_permille = 0;
    uint16_t r2_time_s = 0;
    friend bool operator==(const TerritorialState&, const TerritorialState&) = default;
};

// host -> clientes (confiável): evento discreto de Territorial Assault (captura, onda, overtime, fim de rodada).
struct TerritorialEvent {
    uint8_t event_type = 0;  // ta::EventType (0..4)
    uint8_t team = 0;        // 0=A, 1=B
    uint8_t index = 0;       // índice de objetivo, novo estado ou RoundEndReason
    uint16_t count = 0;      // jogadores revividos ou objetivos capturados
    friend bool operator==(const TerritorialEvent&, const TerritorialEvent&) = default;
};

using Message = std::variant<Hello, Welcome, Reject, MatchStart, MatchEnd, PlayerState, WorldSnapshot, FireEvent,
                             DamageEvent, DeathEvent, RespawnEvent, Ping, Pong, Leave, Kick, LoadoutRequest,
                             LoadoutState, PlayerRoster, BlastEvent, TeamSelectRequest, TerritorialState,
                             TerritorialEvent>;

MessageType type_of(const Message& m);

// Canal em que cada tipo de mensagem deve trafegar (tabela "Mensagens do protocolo" do plano).
Channel channel_for(MessageType t);

// Escreve a mensagem em out. Devolve o tamanho escrito, ou 0 se a mensagem é inválida ou não cabe.
std::size_t encode(const Message& m, std::span<uint8_t> out);

// Atalho: codifica num vetor novo (vazio em caso de falha).
std::vector<uint8_t> encode(const Message& m);

enum class DecodeError : uint8_t {
    None = 0,
    TooShort,
    TooLarge,
    BadMagic,
    UnknownType,
    Truncated,
    TrailingBytes,
    InvalidValue,
};

struct DecodeResult {
    std::optional<Message> message;
    DecodeError error = DecodeError::None;
};

// Decodifica e valida tamanho, tipo e intervalo de cada campo antes de qualquer uso.
// Nenhuma entrada, por mais hostil, provoca leitura fora do buffer.
DecodeResult decode(std::span<const uint8_t> in);

// Validação por campo, usada por encode e decode (exposta para testes e para a ponte com o jogo).
bool is_valid(const Message& m);

const char* to_string(DecodeError e);

}  // namespace netcore
