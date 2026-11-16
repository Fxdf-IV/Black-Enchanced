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

uint8_t default_secondary_id(const RulesConfig& rules) {
    for (const auto& w : rules.weapons) {
        if (w.slot == WeaponSlot::Secondary && w.price == 0) return w.id;
    }
    return rules.find_weapon(0) ? 0 : kNoWeapon;
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
    init_slot_loadout(host_slot);
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
    recent_fires_.clear();
    for (std::size_t i = 0; i < kMaxPlayers; ++i) {
        auto& s = slots_[i];
        if (!s.occupied) continue;
        s.kills = 0;
        s.deaths = 0;
        init_slot_loadout(s);
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

void HostSession::set_spawn_points(std::vector<SpawnPoint> spawns) {
    if (spawns.empty() || spawns.size() > kMaxSpawnPoints) return;
    for (const auto& sp : spawns) {
        if (!std::isfinite(sp.position.x) || !std::isfinite(sp.position.y) ||
            !std::isfinite(sp.position.z) || !std::isfinite(sp.yaw)) {
            return;
        }
    }
    rules_.spawns = std::move(spawns);
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
        const bool allowed = !rules_.enforce_loadout || state.weapon == s.owned_primary ||
                             state.weapon == s.owned_secondary;
        if (allowed) {
            if (const auto* w = rules_.find_weapon(state.weapon)) {
                s.weapon = state.weapon;
                s.ammo_in_mag = w->magazine_size;
            }
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

bool HostSession::local_buy_loadout(const LoadoutRequest& req) {
    return apply_loadout(slots_[0], req);
}

LoadoutState HostSession::slot_loadout_state(uint8_t player_id) const {
    if (player_id >= kMaxPlayers || !slots_[player_id].occupied) return LoadoutState{};
    return build_loadout_state(slots_[player_id], static_cast<LoadoutResult>(slots_[player_id].last_loadout_result));
}

bool HostSession::apply_damage(uint8_t attacker_id, uint8_t victim_id, uint16_t amount, uint8_t weapon_id) {
    if (phase_ != MatchPhase::Running) return false;
    if (attacker_id >= kMaxPlayers || victim_id >= kMaxPlayers || attacker_id == victim_id) return false;
    if (amount == 0 || weapon_id >= kMaxWeapons) return false;

    HostPlayerSlot& shooter = slots_[attacker_id];
    HostPlayerSlot& target = slots_[victim_id];
    if (!shooter.occupied || !target.occupied || (target.flags & kFlagAlive) == 0) return false;

    if (amount >= target.health) {
        target.health = 0;
        target.flags &= ~uint8_t(kFlagAlive);
        target.respawn_at_tick = tick_ + rules_.respawn_delay_ticks;
        ++shooter.kills;
        ++target.deaths;

        if (!rules_.fixed_budget_mode) {
            shooter.credits = static_cast<uint16_t>(
                std::min<uint32_t>(rules_.max_credits, static_cast<uint32_t>(shooter.credits) + rules_.kill_reward_credits));
            target.credits = static_cast<uint16_t>(
                std::min<uint32_t>(rules_.max_credits, static_cast<uint32_t>(target.credits) + rules_.death_bonus_credits));
        }
        // Limpa equipamento comprado da vítima ao morrer (economia CS).
        target.owned_primary = kNoWeapon;
        target.owned_secondary = default_secondary_id(rules_);
        target.tactical_flags = 0;
        if (target.owned_secondary != kNoWeapon) {
            target.weapon = target.owned_secondary;
        }

        broadcast_msg(transport_, DamageEvent{
                                      .attacker = shooter.player_id,
                                      .target = target.player_id,
                                      .amount = amount,
                                      .health_after = 0,
                                  });
        broadcast_msg(transport_, DeathEvent{
                                      .victim = target.player_id,
                                      .killer = shooter.player_id,
                                      .weapon = weapon_id,
                                  });
        broadcast_msg(transport_, build_loadout_state(shooter, LoadoutResult::Ok));
        broadcast_msg(transport_, build_loadout_state(target, LoadoutResult::Ok));
        check_match_end();
    } else {
        target.health = static_cast<uint8_t>(target.health - amount);
        broadcast_msg(transport_, DamageEvent{
                                      .attacker = shooter.player_id,
                                      .target = target.player_id,
                                      .amount = amount,
                                      .health_after = target.health,
                                  });
    }
    return true;
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
    } else if (const auto* req = std::get_if<LoadoutRequest>(&msg)) {
        handle_loadout_request(*slot, *req);
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
    HostPlayerSlot* existing = find_slot_by_peer(peer);
    if (msg.protocol_version != kProtocolVersion) {
        send_msg(transport_, peer, Reject{RejectReason::ProtocolMismatch});
        if (existing) handle_disconnect(peer);
        transport_.disconnect(peer);
        return;
    }
    if (msg.rules_hash != rules_hash_) {
        send_msg(transport_, peer, Reject{RejectReason::RulesMismatch});
        if (existing) handle_disconnect(peer);
        transport_.disconnect(peer);
        return;
    }

    const std::size_t cap = std::min<std::size_t>(kMaxPlayers, rules_.max_players);
    if (existing) {
        send_msg(transport_, peer, Welcome{
                                       .player_id = existing->player_id,
                                       .max_players = static_cast<uint8_t>(cap),
                                       .tick_rate_hz = rules_.tick_rate_hz,
                                       .snapshot_rate_hz = rules_.snapshot_rate_hz,
                                   });
        if (phase_ == MatchPhase::Running) {
            send_msg(transport_, peer, MatchStart{
                                           .start_tick = match_start_tick_,
                                           .time_limit_s = rules_.time_limit_s,
                                           .kill_limit = rules_.kill_limit,
                                           .arena_id = rules_.arena_id,
                                       });
        }
        send_msg(transport_, peer, build_loadout_state(*existing, static_cast<LoadoutResult>(existing->last_loadout_result)));
        return;
    }

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
    init_slot_loadout(*free_slot);
    if (const auto* w = rules_.find_weapon(free_slot->weapon)) {
        free_slot->ammo_in_mag = w->magazine_size;
    }

    send_msg(transport_, peer, Welcome{
                                   .player_id = free_slot->player_id,
                                   .max_players = static_cast<uint8_t>(cap),
                                   .tick_rate_hz = rules_.tick_rate_hz,
                                   .snapshot_rate_hz = rules_.snapshot_rate_hz,
                               });

    for (std::size_t i = 0; i < cap; ++i) {
        if (i != free_slot->player_id && slots_[i].occupied) {
            send_msg(transport_, peer, build_loadout_state(slots_[i], static_cast<LoadoutResult>(slots_[i].last_loadout_result)));
        }
    }

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
        const bool allowed = !rules_.enforce_loadout || msg.weapon == slot.owned_primary ||
                             msg.weapon == slot.owned_secondary;
        if (allowed) {
            if (const auto* w = rules_.find_weapon(msg.weapon)) {
                slot.weapon = msg.weapon;
                slot.ammo_in_mag = w->magazine_size;
            }
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

void HostSession::handle_loadout_request(HostPlayerSlot& slot, const LoadoutRequest& msg) {
    (void)apply_loadout(slot, msg);
}

bool HostSession::apply_validated_fire(HostPlayerSlot& shooter, const FireEvent& msg) {
    if (phase_ != MatchPhase::Running) return false;
    if ((shooter.flags & kFlagAlive) == 0) return false;

    const WeaponSpec* weapon = rules_.find_weapon(msg.weapon);
    if (!weapon) {
        if (!shooter.is_local_host) penalize(shooter.peer, &shooter, KickReason::TooManyViolations);
        return false;
    }

    if (rules_.enforce_loadout) {
        const bool owns_weapon = (weapon->id == shooter.owned_primary) || (weapon->id == shooter.owned_secondary);
        if (!owns_weapon) {
            if (!shooter.is_local_host) penalize(shooter.peer, &shooter, KickReason::TooManyViolations);
            return false;
        }
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
    if (!shooter.is_local_host && recent_fires_.size() < 64) {
        recent_fires_.push_back(msg);
    }

    uint8_t target_id = msg.claimed_target;
    if (target_id == kNoPlayer) {
        const float d2 = msg.direction.x * msg.direction.x +
                         msg.direction.y * msg.direction.y +
                         msg.direction.z * msg.direction.z;
        if (d2 <= 1e-6f) return true;
        const float inv_len = 1.0f / std::sqrt(d2);
        const float dir_x = msg.direction.x * inv_len;
        const float dir_y = msg.direction.y * inv_len;
        const float dir_z = msg.direction.z * inv_len;

        float best_t = weapon->max_range + 1.0f;
        for (std::size_t i = 0; i < kMaxPlayers; ++i) {
            const HostPlayerSlot& cand = slots_[i];
            if (!cand.occupied || (cand.flags & kFlagAlive) == 0 || cand.player_id == shooter.player_id) {
                continue;
            }
            const float dx = cand.position.x - msg.origin.x;
            const float dy = cand.position.y - msg.origin.y;
            const float dz = cand.position.z - msg.origin.z;
            const float t = dx * dir_x + dy * dir_y + dz * dir_z;
            if (t <= 0.0f || t > weapon->max_range) continue;
            const float dist_sq = dx * dx + dy * dy + dz * dz;
            const float perp_sq = std::max(0.0f, dist_sq - t * t);
            if (perp_sq <= 1.44f && t < best_t) {
                best_t = t;
                target_id = static_cast<uint8_t>(i);
            }
        }
        if (target_id == kNoPlayer) return true;
    } else {
        if (target_id >= kMaxPlayers) return false;
        const HostPlayerSlot& target = slots_[target_id];
        if (!target.occupied || (target.flags & kFlagAlive) == 0 || target.player_id == shooter.player_id) {
            return true;
        }
        const float max_r = weapon->max_range;
        if (distance_sq(shooter.position, target.position) > max_r * max_r) {
            return true;
        }
    }

    (void)apply_damage(shooter.player_id, target_id, weapon->damage, weapon->id);
    return true;
}

void HostSession::init_slot_loadout(HostPlayerSlot& slot) {
    slot.credits = rules_.fixed_budget_mode ? rules_.max_credits
                                            : std::min(rules_.starting_credits, rules_.max_credits);
    slot.owned_primary = kNoWeapon;
    slot.owned_secondary = default_secondary_id(rules_);
    slot.tactical_flags = 0;
    slot.last_loadout_result = static_cast<uint8_t>(LoadoutResult::Ok);
    slot.spawn_tick = tick_;
    slot.weapon = (slot.owned_secondary != kNoWeapon) ? slot.owned_secondary : 0;
}

uint32_t HostSession::compute_kit_cost(uint8_t primary_weapon, uint8_t secondary_weapon, uint8_t tactical_flags) const {
    uint32_t cost = 0;
    if (primary_weapon != kNoWeapon) {
        if (const auto* p = rules_.find_weapon(primary_weapon)) cost += p->price;
    }
    if (secondary_weapon != kNoWeapon) {
        if (const auto* s = rules_.find_weapon(secondary_weapon)) cost += s->price;
    }
    if (tactical_flags & kLoadoutArmor) cost += rules_.armor_price;
    if (tactical_flags & kLoadoutTactical) cost += rules_.tactical_price;
    return cost;
}

LoadoutState HostSession::build_loadout_state(const HostPlayerSlot& slot, LoadoutResult result) const {
    uint16_t buy_left = 0xFFFFu;
    if (phase_ == MatchPhase::Ended || (slot.flags & kFlagAlive) == 0) {
        buy_left = 0;
    } else if (rules_.buy_window_ticks > 0 && phase_ == MatchPhase::Running) {
        const uint32_t elapsed = (tick_ >= slot.spawn_tick) ? (tick_ - slot.spawn_tick) : 0;
        buy_left = (elapsed <= rules_.buy_window_ticks)
                       ? static_cast<uint16_t>(rules_.buy_window_ticks - elapsed)
                       : 0;
    }
    const uint32_t cost = compute_kit_cost(slot.owned_primary, slot.owned_secondary, slot.tactical_flags);
    return LoadoutState{
        .player_id = slot.player_id,
        .primary_weapon = slot.owned_primary,
        .secondary_weapon = slot.owned_secondary,
        .tactical_flags = slot.tactical_flags,
        .last_result = static_cast<uint8_t>(result),
        .credits = slot.credits,
        .max_credits = rules_.max_credits,
        .kit_cost = static_cast<uint16_t>(std::min<uint32_t>(cost, 0xFFFFu)),
        .buy_ticks_left = buy_left,
    };
}

bool HostSession::apply_loadout(HostPlayerSlot& slot, const LoadoutRequest& req) {
    auto fail = [&](LoadoutResult res) -> bool {
        slot.last_loadout_result = static_cast<uint8_t>(res);
        if (!slot.is_local_host) {
            send_msg(transport_, slot.peer, build_loadout_state(slot, res));
        }
        return false;
    };

    if (phase_ == MatchPhase::Ended || (slot.flags & kFlagAlive) == 0) {
        return fail(LoadoutResult::NotAliveOrEnded);
    }
    if (rules_.buy_window_ticks > 0 && phase_ == MatchPhase::Running) {
        const uint32_t elapsed = (tick_ >= slot.spawn_tick) ? (tick_ - slot.spawn_tick) : 0;
        if (elapsed > rules_.buy_window_ticks) {
            return fail(LoadoutResult::BuyWindowClosed);
        }
    }
    if ((req.tactical_flags & ~(kLoadoutArmor | kLoadoutTactical)) != 0) {
        return fail(LoadoutResult::InvalidItem);
    }

    const WeaponSpec* prim = nullptr;
    if (req.primary_weapon != kNoWeapon) {
        prim = rules_.find_weapon(req.primary_weapon);
        if (!prim || (prim->slot != WeaponSlot::Primary && prim->slot != WeaponSlot::Tactical)) {
            return fail(LoadoutResult::InvalidItem);
        }
    }
    const WeaponSpec* sec = nullptr;
    if (req.secondary_weapon != kNoWeapon) {
        sec = rules_.find_weapon(req.secondary_weapon);
        if (!sec || sec->slot != WeaponSlot::Secondary) {
            return fail(LoadoutResult::InvalidItem);
        }
    }

    const uint32_t total_cost = compute_kit_cost(req.primary_weapon, req.secondary_weapon, req.tactical_flags);
    if (total_cost > rules_.max_credits) {
        return fail(LoadoutResult::ExceedsBudgetCap);
    }

    if (rules_.fixed_budget_mode) {
        slot.credits = rules_.max_credits;
    } else {
        uint32_t delta_cost = 0;
        if (prim && req.primary_weapon != slot.owned_primary) delta_cost += prim->price;
        if (sec && req.secondary_weapon != slot.owned_secondary) delta_cost += sec->price;
        const uint8_t added_flags = static_cast<uint8_t>(req.tactical_flags & ~slot.tactical_flags);
        if (added_flags & kLoadoutArmor) delta_cost += rules_.armor_price;
        if (added_flags & kLoadoutTactical) delta_cost += rules_.tactical_price;
        if (delta_cost > slot.credits) {
            return fail(LoadoutResult::InsufficientCredits);
        }
        slot.credits = static_cast<uint16_t>(slot.credits - delta_cost);
    }

    const uint8_t prev_primary = slot.owned_primary;
    const uint8_t prev_secondary = slot.owned_secondary;
    const uint8_t prev_flags = slot.tactical_flags;

    slot.owned_primary = req.primary_weapon;
    slot.owned_secondary = req.secondary_weapon;
    slot.tactical_flags = req.tactical_flags;
    slot.last_loadout_result = static_cast<uint8_t>(LoadoutResult::Ok);

    if ((slot.tactical_flags & kLoadoutArmor) && !(prev_flags & kLoadoutArmor)) {
        const uint16_t max_armor_hp =
            std::min<uint16_t>(255u, static_cast<uint16_t>(rules_.max_health) + rules_.armor_bonus_health);
        slot.health = static_cast<uint8_t>(
            std::min<uint16_t>(max_armor_hp, static_cast<uint16_t>(slot.health) + rules_.armor_bonus_health));
    } else if (!(slot.tactical_flags & kLoadoutArmor) && (prev_flags & kLoadoutArmor)) {
        slot.health = std::min(slot.health, rules_.max_health);
    }

    if (slot.owned_primary != kNoWeapon && slot.owned_primary != prev_primary) {
        slot.weapon = slot.owned_primary;
    } else if (slot.owned_secondary != kNoWeapon && slot.owned_secondary != prev_secondary) {
        slot.weapon = slot.owned_secondary;
    } else if (slot.weapon != slot.owned_primary && slot.weapon != slot.owned_secondary) {
        slot.weapon = (slot.owned_primary != kNoWeapon)
                          ? slot.owned_primary
                          : (slot.owned_secondary != kNoWeapon ? slot.owned_secondary : 0);
    }
    if (const auto* active_w = rules_.find_weapon(slot.weapon)) {
        slot.ammo_in_mag = active_w->magazine_size;
    }

    broadcast_msg(transport_, build_loadout_state(slot, LoadoutResult::Ok));
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
    const uint16_t max_hp = (slot.tactical_flags & kLoadoutArmor)
                                ? std::min<uint16_t>(255u, static_cast<uint16_t>(rules_.max_health) + rules_.armor_bonus_health)
                                : rules_.max_health;
    slot.health = static_cast<uint8_t>(max_hp);
    slot.flags = kFlagAlive;
    slot.respawn_at_tick = 0;
    slot.last_state_tick = 0;
    slot.spawn_tick = tick_;
    if (rules_.fixed_budget_mode) {
        slot.credits = rules_.max_credits;
    }
    if (const auto* w = rules_.find_weapon(slot.weapon)) {
        slot.ammo_in_mag = w->magazine_size;
    }

    broadcast_msg(transport_, RespawnEvent{
                                  .player = slot.player_id,
                                  .spawn_point = spawn_idx,
                                  .position = slot.position,
                                  .yaw = slot.yaw,
                              });
    broadcast_msg(transport_, build_loadout_state(slot, static_cast<LoadoutResult>(slot.last_loadout_result)));
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

bool ClientSession::send_loadout_request(const LoadoutRequest& req) {
    if (!joined_) return false;
    auto bytes = encode(req);
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
    } else if (const auto* fire = std::get_if<FireEvent>(&msg)) {
        if (recent_fires_.size() < 64) {
            recent_fires_.push_back(*fire);
        }
    } else if (const auto* dmg = std::get_if<DamageEvent>(&msg)) {
        recent_damages_.push_back(*dmg);
    } else if (const auto* death = std::get_if<DeathEvent>(&msg)) {
        recent_deaths_.push_back(*death);
    } else if (const auto* resp = std::get_if<RespawnEvent>(&msg)) {
        recent_respawns_.push_back(*resp);
    } else if (const auto* ls = std::get_if<LoadoutState>(&msg)) {
        if (ls->player_id < kMaxPlayers) {
            loadouts_[ls->player_id] = *ls;
        }
        if (recent_loadouts_.size() < 64) {
            recent_loadouts_.push_back(*ls);
        }
    } else if (const auto* pong = std::get_if<Pong>(&msg)) {
        last_pong_ = *pong;
    }
}

}  // namespace netcore
