#include "netcore/session.hpp"

#include <algorithm>
#include <cmath>

namespace netcore {

namespace {

float distance_sq(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

void send_msg(ITransport& t, PeerId peer, const Message& m) {
    auto bytes = encode(m);
    if (!bytes.empty()) {
        t.send(peer, channel_for(type_of(m)), bytes);
    }
}

void broadcast_msg(ITransport& t, const Message& m) {
    auto bytes = encode(m);
    if (!bytes.empty()) {
        t.broadcast(channel_for(type_of(m)), bytes);
    }
}

}  // namespace

// ---- HostSession ---------------------------------------------------------------------------

HostSession::HostSession(ITransport& transport, RulesConfig rules, std::string host_name)
    : transport_(transport), rules_(std::move(rules)), rules_hash_(rules_.compute_hash()) {
    for (std::size_t i = 0; i < kMaxPlayers; ++i) {
        slots_[i].player_id = static_cast<uint8_t>(i);
    }
    // Slot 0 pertence ao jogador local do host (listen-server)
    auto& host_slot = slots_[0];
    host_slot.occupied = true;
    host_slot.is_local_host = true;
    host_slot.name = host_name.empty() ? "Host" : std::move(host_name);
    if (host_slot.name.size() > kMaxNameLength) host_slot.name.resize(kMaxNameLength);
    host_slot.health = rules_.max_health;
    host_slot.flags = kFlagAlive;
    if (!rules_.spawns.empty()) {
        host_slot.position = rules_.spawns[0].position;
        host_slot.yaw = rules_.spawns[0].yaw;
        next_spawn_cursor_ = 1;
    }
}

void HostSession::start_match() {
    phase_ = MatchPhase::Running;
    match_start_tick_ = tick_;
    winner_ = kNoPlayer;
    for (std::size_t i = 0; i < kMaxPlayers; ++i) {
        auto& s = slots_[i];
        if (!s.occupied) continue;
        s.kills = 0;
        s.deaths = 0;
        const uint8_t sp = rules_.spawns.empty() ? 0 : static_cast<uint8_t>(next_spawn_cursor_++ % rules_.spawns.size());
        spawn_player(s, sp);
    }
    broadcast_msg(transport_, MatchStart{
                                  .start_tick = match_start_tick_,
                                  .time_limit_s = rules_.time_limit_s,
                                  .kill_limit = rules_.kill_limit,
                                  .arena_id = rules_.arena_id,
                              });
}

void HostSession::set_local_player_state(const PlayerState& state) {
    auto& s = slots_[0];
    if ((s.flags & kFlagAlive) == 0) return;
    if (!is_valid(Message{state})) return;
    s.position = state.position;
    s.yaw = state.yaw;
    s.pitch = state.pitch;
    s.velocity = state.velocity;
    s.anim_state = state.anim_state;
    if (s.weapon != state.weapon) {
        s.weapon = state.weapon;
        if (const auto* w = rules_.find_weapon(s.weapon)) {
            s.ammo_in_mag = w->magazine_size;
        }
    }
    s.flags = (state.flags & ~uint8_t(kFlagAlive)) | kFlagAlive;
}

bool HostSession::local_fire(const FireEvent& ev) {
    FireEvent copy = ev;
    copy.shooter = 0;
    if (!is_valid(Message{copy})) return false;
    return apply_validated_fire(slots_[0], copy);
}

void HostSession::tick() {
    ++tick_;

    // Reset da janela de rate-limit a cada 1 segundo de simulação
    const uint32_t window_ticks = std::max<uint16_t>(1, rules_.tick_rate_hz);
    if (tick_ % window_ticks == 0) {
        for (auto& s : slots_) s.packets_in_window = 0;
        unauthenticated_packets_.clear();
    }

    // 1. Eventos de transporte
    for (const auto& ev : transport_.poll()) {
        if (ev.type == TransportEventType::Disconnected) {
            handle_disconnect(ev.peer);
        } else if (ev.type == TransportEventType::Packet) {
            handle_packet(ev.peer, ev.payload);
        }
    }

    // 2. Respawns pendentes
    if (phase_ == MatchPhase::Running) {
        for (auto& s : slots_) {
            if (!s.occupied) continue;
            if ((s.flags & kFlagAlive) == 0 && s.respawn_at_tick != 0 && tick_ >= s.respawn_at_tick) {
                const uint8_t sp =
                    rules_.spawns.empty() ? 0 : static_cast<uint8_t>(next_spawn_cursor_++ % rules_.spawns.size());
                spawn_player(s, sp);
            }
        }
        check_match_end();
    }

    // 3. Snapshots periódicos
    const uint32_t snap_interval =
        std::max<uint32_t>(1, rules_.tick_rate_hz / std::max<uint16_t>(1, rules_.snapshot_rate_hz));
    if (tick_ % snap_interval == 0) {
        broadcast_snapshot();
    }
}

std::size_t HostSession::player_count() const {
    std::size_t n = 0;
    for (const auto& s : slots_) {
        if (s.occupied) ++n;
    }
    return n;
}

void HostSession::handle_packet(PeerId peer, const std::vector<uint8_t>& payload) {
    HostPlayerSlot* slot = find_slot_by_peer(peer);

    // Rate limit por cliente (mitigação do risco de flood descrito na seção 9)
    if (slot) {
        if (++slot->packets_in_window > rules_.max_packets_per_window) {
            kick_peer(peer, slot, KickReason::RateLimited);
            return;
        }
    } else {
        if (++unauthenticated_packets_[peer] > rules_.max_packets_per_window) {
            send_msg(transport_, peer, Kick{KickReason::RateLimited});
            transport_.disconnect(peer);
            return;
        }
    }

    const auto decoded = decode(payload);
    if (!decoded.message.has_value()) {
        penalize(peer, slot, KickReason::Malformed);
        return;
    }

    const Message& msg = *decoded.message;
    if (const auto* hello = std::get_if<Hello>(&msg)) {
        handle_hello(peer, *hello);
        return;
    }

    if (!slot) {
        // Mensagem de jogo antes do handshake
        penalize(peer, nullptr, KickReason::Malformed);
        return;
    }

    if (const auto* ps = std::get_if<PlayerState>(&msg)) {
        handle_player_state(*slot, *ps);
    } else if (const auto* fire = std::get_if<FireEvent>(&msg)) {
        handle_fire(*slot, *fire);
    } else if (const auto* ping = std::get_if<Ping>(&msg)) {
        send_msg(transport_, peer, Pong{.nonce = ping->nonce, .echo_ms = ping->sent_ms, .host_tick = tick_});
    } else if (std::holds_alternative<Leave>(msg)) {
        handle_disconnect(peer);
        transport_.disconnect(peer);
    } else {
        // Cliente tentando enviar mensagem exclusiva do host (Welcome, WorldSnapshot, DamageEvent etc.)
        penalize(peer, slot, KickReason::Malformed);
    }
}

void HostSession::handle_disconnect(PeerId peer) {
    if (auto* slot = find_slot_by_peer(peer)) {
        const uint8_t id = slot->player_id;
        *slot = HostPlayerSlot{};
        slot->player_id = id;
    }
    unauthenticated_packets_.erase(peer);
    unauthenticated_violations_.erase(peer);
}

void HostSession::handle_hello(PeerId peer, const Hello& msg) {
    if (find_slot_by_peer(peer) != nullptr) return;

    if (msg.protocol_version != kProtocolVersion) {
        send_msg(transport_, peer, Reject{RejectReason::ProtocolMismatch});
        transport_.disconnect(peer);
        return;
    }
    if (msg.rules_hash != rules_hash_) {
        send_msg(transport_, peer, Reject{RejectReason::RulesMismatch});
        transport_.disconnect(peer);
        return;
    }

    const std::size_t cap = std::min<std::size_t>(kMaxPlayers, rules_.max_players);
    HostPlayerSlot* free_slot = nullptr;
    for (std::size_t i = 1; i < cap; ++i) {
        if (!slots_[i].occupied) {
            free_slot = &slots_[i];
            break;
        }
    }
    if (!free_slot) {
        send_msg(transport_, peer, Reject{RejectReason::RoomFull});
        transport_.disconnect(peer);
        return;
    }

    free_slot->occupied = true;
    free_slot->is_local_host = false;
    free_slot->peer = peer;
    free_slot->name = msg.name;
    free_slot->kills = 0;
    free_slot->deaths = 0;
    if (const auto* w = rules_.find_weapon(0)) {
        free_slot->ammo_in_mag = w->magazine_size;
    }

    send_msg(transport_, peer, Welcome{
                                   .player_id = free_slot->player_id,
                                   .max_players = static_cast<uint8_t>(cap),
                                   .tick_rate_hz = rules_.tick_rate_hz,
                                   .snapshot_rate_hz = rules_.snapshot_rate_hz,
                               });

    const uint8_t sp = rules_.spawns.empty() ? 0 : static_cast<uint8_t>(next_spawn_cursor_++ % rules_.spawns.size());
    spawn_player(*free_slot, sp);

    if (phase_ == MatchPhase::Running) {
        send_msg(transport_, peer, MatchStart{
                                       .start_tick = match_start_tick_,
                                       .time_limit_s = rules_.time_limit_s,
                                       .kill_limit = rules_.kill_limit,
                                       .arena_id = rules_.arena_id,
                                   });
    }
}

void HostSession::handle_player_state(HostPlayerSlot& slot, const PlayerState& msg) {
    // Descarta pacotes fora de ordem ou repetidos
    if (msg.sequence <= slot.last_sequence) return;

    // Se o jogador está morto no host, ignora atualizações de movimento até o respawn
    if ((slot.flags & kFlagAlive) == 0) {
        slot.last_sequence = msg.sequence;
        return;
    }

    // Verificação de velocidade plausível no host (seção 9 do plano)
    const float speed_sq =
        msg.velocity.x * msg.velocity.x + msg.velocity.y * msg.velocity.y + msg.velocity.z * msg.velocity.z;
    const float max_v = rules_.max_speed_units_per_sec * 1.5f;  // tolerância para pulo/queda
    if (speed_sq > max_v * max_v) {
        penalize(slot.peer, &slot, KickReason::TooManyViolations);
        return;
    }

    if (slot.last_state_tick > 0 && msg.client_tick > slot.last_state_tick) {
        const uint32_t dt_ticks = msg.client_tick - slot.last_state_tick;
        const float dt_sec = static_cast<float>(dt_ticks) / static_cast<float>(std::max<uint16_t>(1, rules_.tick_rate_hz));
        const float max_disp = std::max(6.0f, max_v * dt_sec * 2.0f);
        if (distance_sq(slot.position, msg.position) > max_disp * max_disp) {
            penalize(slot.peer, &slot, KickReason::TooManyViolations);
            return;
        }
    }

    slot.last_sequence = msg.sequence;
    slot.last_state_tick = msg.client_tick;
    slot.position = msg.position;
    slot.yaw = msg.yaw;
    slot.pitch = msg.pitch;
    slot.velocity = msg.velocity;
    slot.anim_state = msg.anim_state;
    if (slot.weapon != msg.weapon) {
        if (const auto* w = rules_.find_weapon(msg.weapon)) {
            slot.weapon = msg.weapon;
            slot.ammo_in_mag = w->magazine_size;
        }
    }
    slot.flags = (msg.flags & ~uint8_t(kFlagAlive)) | kFlagAlive;
}

void HostSession::handle_fire(HostPlayerSlot& shooter, const FireEvent& msg) {
    if (msg.shooter != shooter.player_id) {
        penalize(shooter.peer, &shooter, KickReason::TooManyViolations);
        return;
    }
    apply_validated_fire(shooter, msg);
}

bool HostSession::apply_validated_fire(HostPlayerSlot& shooter, const FireEvent& msg) {
    if (phase_ != MatchPhase::Running) return false;
    if ((shooter.flags & kFlagAlive) == 0) return false;

    const WeaponSpec* weapon = rules_.find_weapon(msg.weapon);
    if (!weapon) {
        if (!shooter.is_local_host) penalize(shooter.peer, &shooter, KickReason::TooManyViolations);
        return false;
    }

    // Validação de cadência
    if (shooter.last_fire_tick != 0 && tick_ - shooter.last_fire_tick < weapon->min_fire_interval_ticks) {
        return false;
    }

    // Origem do disparo precisa ser plausível em relação à posição do atirador
    if (distance_sq(shooter.position, msg.origin) > 16.0f) {
        if (!shooter.is_local_host) penalize(shooter.peer, &shooter, KickReason::TooManyViolations);
        return false;
    }

    shooter.weapon = weapon->id;
    shooter.last_fire_tick = tick_;
    if (shooter.ammo_in_mag == 0) {
        shooter.ammo_in_mag = weapon->magazine_size;
    }
    --shooter.ammo_in_mag;

    // Retransmite o evento de disparo para todos os clientes (efeitos, som e traçante)
    broadcast_msg(transport_, msg);

    if (msg.claimed_target == kNoPlayer) return true;
    if (msg.claimed_target >= kMaxPlayers) return false;

    HostPlayerSlot& target = slots_[msg.claimed_target];
    if (!target.occupied || (target.flags & kFlagAlive) == 0 || target.player_id == shooter.player_id) {
        return true;
    }

    // Valida distância máxima da arma
    const float max_r = weapon->max_range;
    if (distance_sq(shooter.position, target.position) > max_r * max_r) {
        return true;
    }

    const uint16_t dmg = weapon->damage;
    if (dmg >= target.health) {
        target.health = 0;
        target.flags &= ~uint8_t(kFlagAlive);
        target.respawn_at_tick = tick_ + rules_.respawn_delay_ticks;
        ++shooter.kills;
        ++target.deaths;

        broadcast_msg(transport_, DamageEvent{
                                      .attacker = shooter.player_id,
                                      .target = target.player_id,
                                      .amount = dmg,
                                      .health_after = 0,
                                  });
        broadcast_msg(transport_, DeathEvent{
                                      .victim = target.player_id,
                                      .killer = shooter.player_id,
                                      .weapon = weapon->id,
                                  });
        check_match_end();
    } else {
        target.health = static_cast<uint8_t>(target.health - dmg);
        broadcast_msg(transport_, DamageEvent{
                                      .attacker = shooter.player_id,
                                      .target = target.player_id,
                                      .amount = dmg,
                                      .health_after = target.health,
                                  });
    }
    return true;
}

void HostSession::spawn_player(HostPlayerSlot& slot, uint8_t spawn_idx) {
    SpawnPoint sp{};
    if (!rules_.spawns.empty()) {
        sp = rules_.spawns[spawn_idx % rules_.spawns.size()];
    }
    slot.position = sp.position;
    slot.yaw = sp.yaw;
    slot.pitch = 0.0f;
    slot.velocity = Vec3{0.0f, 0.0f, 0.0f};
    slot.health = rules_.max_health;
    slot.flags = kFlagAlive;
    slot.respawn_at_tick = 0;
    if (const auto* w = rules_.find_weapon(slot.weapon)) {
        slot.ammo_in_mag = w->magazine_size;
    }

    broadcast_msg(transport_, RespawnEvent{
                                  .player = slot.player_id,
                                  .spawn_point = spawn_idx,
                                  .position = slot.position,
                                  .yaw = slot.yaw,
                              });
}

void HostSession::check_match_end() {
    if (phase_ != MatchPhase::Running) return;

    // 1. Limite de eliminações
    if (rules_.kill_limit > 0) {
        for (const auto& s : slots_) {
            if (s.occupied && s.kills >= static_cast<int16_t>(rules_.kill_limit)) {
                phase_ = MatchPhase::Ended;
                winner_ = s.player_id;
                broadcast_msg(transport_, MatchEnd{.winner = winner_});
                return;
            }
        }
    }

    // 2. Limite de tempo
    if (rules_.time_limit_s > 0) {
        const uint32_t elapsed_ticks = tick_ - match_start_tick_;
        const uint32_t limit_ticks =
            static_cast<uint32_t>(rules_.time_limit_s) * std::max<uint16_t>(1, rules_.tick_rate_hz);
        if (elapsed_ticks >= limit_ticks) {
            phase_ = MatchPhase::Ended;
            int16_t best_kills = -32768;
            uint8_t best_player = kNoPlayer;
            bool tie = false;
            for (const auto& s : slots_) {
                if (!s.occupied) continue;
                if (s.kills > best_kills) {
                    best_kills = s.kills;
                    best_player = s.player_id;
                    tie = false;
                } else if (s.kills == best_kills) {
                    tie = true;
                }
            }
            winner_ = tie ? kNoPlayer : best_player;
            broadcast_msg(transport_, MatchEnd{.winner = winner_});
        }
    }
}

void HostSession::broadcast_snapshot() {
    WorldSnapshot snap;
    snap.tick = tick_;
    snap.phase = phase_;
    if (phase_ == MatchPhase::Running && rules_.time_limit_s > 0) {
        const uint32_t elapsed_s = (tick_ - match_start_tick_) / std::max<uint16_t>(1, rules_.tick_rate_hz);
        snap.time_left_s =
            elapsed_s >= rules_.time_limit_s ? 0 : static_cast<uint16_t>(rules_.time_limit_s - elapsed_s);
    } else {
        snap.time_left_s = rules_.time_limit_s;
    }

    for (const auto& s : slots_) {
        if (!s.occupied) continue;
        snap.players.push_back(SnapshotPlayer{
            .player_id = s.player_id,
            .position = s.position,
            .yaw = s.yaw,
            .pitch = s.pitch,
            .velocity = s.velocity,
            .anim_state = s.anim_state,
            .weapon = s.weapon,
            .health = s.health,
            .flags = s.flags,
            .kills = s.kills,
            .deaths = s.deaths,
        });
    }
    broadcast_msg(transport_, snap);
}

void HostSession::penalize(PeerId peer, HostPlayerSlot* slot, KickReason reason_if_kicked) {
    if (slot) {
        if (++slot->violations >= rules_.max_violations_before_kick) {
            kick_peer(peer, slot, reason_if_kicked);
        }
    } else {
        if (++unauthenticated_violations_[peer] >= rules_.max_violations_before_kick) {
            send_msg(transport_, peer, Kick{reason_if_kicked});
            transport_.disconnect(peer);
            unauthenticated_packets_.erase(peer);
            unauthenticated_violations_.erase(peer);
        }
    }
}

void HostSession::kick_peer(PeerId peer, HostPlayerSlot* slot, KickReason reason) {
    send_msg(transport_, peer, Kick{reason});
    if (slot) {
        const uint8_t id = slot->player_id;
        *slot = HostPlayerSlot{};
        slot->player_id = id;
    }
    transport_.disconnect(peer);
}

HostPlayerSlot* HostSession::find_slot_by_peer(PeerId peer) {
    for (auto& s : slots_) {
        if (s.occupied && !s.is_local_host && s.peer == peer) return &s;
    }
    return nullptr;
}

// ---- ClientSession -------------------------------------------------------------------------

ClientSession::ClientSession(ITransport& transport, PeerId host_peer, RulesConfig rules, std::string player_name)
    : transport_(transport), host_peer_(host_peer), rules_(std::move(rules)), name_(std::move(player_name)) {}

bool ClientSession::send_hello() {
    Hello h{
        .protocol_version = kProtocolVersion,
        .rules_hash = rules_.compute_hash(),
        .name = name_,
    };
    auto bytes = encode(h);
    if (bytes.empty()) return false;
    return transport_.send(host_peer_, Channel::Reliable, bytes);
}

bool ClientSession::send_local_state(PlayerState state) {
    if (!joined_) return false;
    state.sequence = next_sequence_++;
    auto bytes = encode(state);
    if (bytes.empty()) return false;
    return transport_.send(host_peer_, Channel::Unreliable, bytes);
}

bool ClientSession::send_fire(FireEvent ev) {
    if (!joined_) return false;
    ev.shooter = player_id_;
    auto bytes = encode(ev);
    if (bytes.empty()) return false;
    return transport_.send(host_peer_, Channel::Reliable, bytes);
}

bool ClientSession::send_ping(uint32_t nonce, uint32_t now_ms) {
    auto bytes = encode(Ping{.nonce = nonce, .sent_ms = now_ms});
    if (bytes.empty()) return false;
    return transport_.send(host_peer_, Channel::Unreliable, bytes);
}

void ClientSession::poll() {
    for (const auto& ev : transport_.poll()) {
        if (ev.type != TransportEventType::Packet) continue;
        const auto decoded = decode(ev.payload);
        if (!decoded.message.has_value()) continue;
        handle_message(*decoded.message);
    }
}

void ClientSession::handle_message(const Message& msg) {
    if (const auto* w = std::get_if<Welcome>(&msg)) {
        joined_ = true;
        player_id_ = w->player_id;
    } else if (const auto* r = std::get_if<Reject>(&msg)) {
        joined_ = false;
        rejected_ = r->reason;
    } else if (const auto* k = std::get_if<Kick>(&msg)) {
        joined_ = false;
        kicked_ = k->reason;
    } else if (std::holds_alternative<MatchStart>(msg)) {
        latest_snapshot_.phase = MatchPhase::Running;
        winner_ = kNoPlayer;
    } else if (const auto* me = std::get_if<MatchEnd>(&msg)) {
        latest_snapshot_.phase = MatchPhase::Ended;
        winner_ = me->winner;
    } else if (const auto* snap = std::get_if<WorldSnapshot>(&msg)) {
        if (snap->tick >= latest_snapshot_.tick) {
            latest_snapshot_ = *snap;
        }
    } else if (const auto* dmg = std::get_if<DamageEvent>(&msg)) {
        recent_damages_.push_back(*dmg);
    } else if (const auto* death = std::get_if<DeathEvent>(&msg)) {
        recent_deaths_.push_back(*death);
    } else if (const auto* resp = std::get_if<RespawnEvent>(&msg)) {
        recent_respawns_.push_back(*resp);
    } else if (const auto* pong = std::get_if<Pong>(&msg)) {
        last_pong_ = *pong;
    }
}

}  // namespace netcore
