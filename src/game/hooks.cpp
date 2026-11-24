// Ganchos de funções recompiladas do default.xbe (BLACK, NTSC-U, XDK 5849).
// Uma definição forte X86_FUNC(sub_XXXXXXXX) substitui a função gerada pelo recompilador;
// __imp__sub_XXXXXXXX chama a implementação original.
//
// Regra de ouro: quando mp::CurrentRole() == mp::Role::OfflineCampaign (padrão), todo gancho
// delega imediatamente para a função original sem alterar o estado da campanha.

#include <algorithm>
#include <atomic>
#include <cmath>

#include "common/cutscene_skip.h"
#include "common/pc_input.h"
#include "core/log.h"
#include "game/black_addrs.h"
#include "mp/mp_bridge.h"
#include "netcore/limits.hpp"
#include "netcore/protocol.hpp"
#include "xdk/hle.h"

namespace {
std::atomic<uint32_t> g_active_movie_ctrl{0};
std::atomic<bool> g_active_movie_is_intro{false};
}  // namespace

// 0. Atualização por quadro do player de filme XMV (0x000C4570; ESI = player). Os três donos do player
//    (sub_00209D40, sub_00209DC0 e sub_000D3620) encerram o vídeo quando [player+0x62] ("filme terminou",
//    definido em 0x000C4704) vira 1 e então liberam o decodificador (sub_000DB190). Com o pulo ativo (botão
//    "Pular Intro" do cliente web: clique = só o filme atual, Shift+clique = todos) marcamos esse byte depois da atualização normal, e o próprio jogo executa
//    o encerramento limpo.
X86_EXTERN_FUNC(__imp__sub_000C4570);
X86_FUNC(sub_000C4570) {
    const uint32_t player = ctx.esi;
    __imp__sub_000C4570(ctx, base);
    const uint32_t mode = common::g_skip_cutscenes.load(std::memory_order_relaxed);
    const bool mp_direct = mp::IsDirectMapLaunchPending();
    if ((mode != common::kSkipOff || mp_direct) && player >= 0x10000u && black::R8(base, player + 0x62u) == 0u) {
        static std::atomic<uint32_t> logged{0};
        if (logged.fetch_add(1) < 8u) {
            LOGI("skip_cutscenes: forcando fim do filme XMV (player=0x%08X, modo=%u, mp_direct=%d)",
                 player, mode, mp_direct ? 1 : 0);
        }
        black::W8(base, player + 0x62u, 1u);
        if (!mp_direct && mode == common::kSkipOnce) {
            uint32_t expected = common::kSkipOnce;
            common::g_skip_cutscenes.compare_exchange_strong(expected, common::kSkipOff);
        }
    }
}

// 1. Quadro principal (0x0020F6A0): drena pacotes e atualiza atores remotos antes da simulação;
//    lê e transmite o estado do jogador local depois da simulação.
//    BLACK foi projetado pela Criterion para 30 FPS cravados (33.333 ms por quadro).
//    Em ambientes com WebGPU assíncrono ou monitores de alta taxa de atualização sem VSync
//    bloqueante no D3D8, o loop em loc_000E1AB7 roda centenas de vezes por segundo,
//    acelerando física, locomoção e tiros em até 4x. Aqui garantimos a cadência exata de 30.0 Hz.
X86_EXTERN_FUNC(__imp__sub_0020F6A0);
X86_FUNC(sub_0020F6A0) {
    static auto s_target_time = std::chrono::steady_clock::now();
    constexpr auto kFrameDur = std::chrono::microseconds(33333);  // 30.0 Hz NTSC Criterion

    const auto now = std::chrono::steady_clock::now();
    if (now < s_target_time) {
        const auto remaining = s_target_time - now;
        if (remaining > std::chrono::milliseconds(2)) {
            std::this_thread::sleep_for(remaining - std::chrono::milliseconds(1));
        }
        while (std::chrono::steady_clock::now() < s_target_time) {
            std::this_thread::yield();
        }
    }

    const auto after_frame = std::chrono::steady_clock::now();
    if (after_frame > s_target_time + kFrameDur * 2) {
        s_target_time = after_frame + kFrameDur;
    } else {
        s_target_time += kFrameDur;
    }

    // Se houver vídeo/cutscene ativo e o jogador pressionar teclas de pular (Space/Enter/Esc/Clique),
    // ou se skip_intro estiver ativo: aguarda o streaming de background finalizar o carregamento dos assets
    // da fase/menu ([stream_mgr + 0x62] != 0) para pular com segurança absoluta e sem memory out of bounds.
    const uint32_t movie_ctrl = g_active_movie_ctrl.load(std::memory_order_acquire);
    if (movie_ctrl >= 0x10000u && black::R8(base, movie_ctrl + 0x1B8u) != 0) {
        const uint32_t stream_mgr = black::R32(base, movie_ctrl);
        const bool load_ready = (stream_mgr >= 0x10000u) ? (black::R8(base, stream_mgr + 0x62u) != 0) : true;
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        const bool user_skip = common::ShouldSkipCurrentMovie();
        const bool auto_skip =
            (cfg.skip_intro && g_active_movie_is_intro.load(std::memory_order_relaxed)) ||
            mp::IsDirectMapLaunchPending();

        if ((user_skip || auto_skip) && load_ready) {
            black::W8(base, movie_ctrl + 0x1B8u, 0);
            black::W8(base, movie_ctrl + 0x1B9u, 1);
            g_active_movie_is_intro.store(false, std::memory_order_relaxed);
            LOGR("sub_0020F6A0: Video/cutscene pulado com seguranca (load_ready=1, user=%d, auto=%d).",
                 user_skip ? 1 : 0, auto_skip ? 1 : 0);
        }
    }

    mp::OnPreFrame(base);
    __imp__sub_0020F6A0(ctx, base);
    mp::OnPostFrame(base);
}

// 2. Cérebro de IA (0x001E5F70 — vf0 do controlador de IA):
//    Teste #2 da Matriz da Fase 2 ("Desligar o cérebro de IA").
//    Se o controlador em ECX pertence a um ator controlado por jogador remoto, retorna sem executar a IA.
X86_EXTERN_FUNC(__imp__sub_001E5F70);
X86_FUNC(sub_001E5F70) {
    if (mp::ShouldSkipAiThink(ctx.ecx, base)) {
        // Plain ret (desempilha o endereço de retorno de 32 bits da pilha convidada)
        ::kernel::Return(ctx, ctx.eax, 0);
        return;
    }
    __imp__sub_001E5F70(ctx, base);
}

// 3. Morte do jogador local (0x001D54C0 — evento 5 do controlador do jogador):
//    Teste #7 e #8 da Matriz da Fase 2. Salva o controlador e o nome do jogador antes que 0x001D54C0
//    os substitua pelo controlador de morte (+0x630), permitindo respawn limpo sem recarregar a fase.
X86_EXTERN_FUNC(__imp__sub_001D54C0);
X86_FUNC(sub_001D54C0) {
    mp::OnLocalPlayerDying(base);
    __imp__sub_001D54C0(ctx, base);
}

// 4. Verificação por quadro do estado de morte do jogador (0x001BD360 — vf1 do controlador):
//    Quando o jogador está com state == 2 (morto), 0x001BD360 pede "Missão Fracassada" (0x001F45C0).
//    Em partida multiplayer ativa, suprimimos essa chamada para aguardar o RespawnEvent.
X86_EXTERN_FUNC(__imp__sub_001BD360);
X86_FUNC(sub_001BD360) {
    if (mp::ShouldSuppressMissionFailed(base)) {
        const uint32_t player = black::LocalPlayerAddr(base);
        if (player && black::R32(base, player + black::kCharState) == 2u) {
            ::kernel::Return(ctx, ctx.eax, 0);
            return;
        }
    }
    __imp__sub_001BD360(ctx, base);
}

// 5. Dano de projétil / impacto direcionado (0x001E74B0 — ret 0x1C, ECX = vítima):
//    Em partida multiplayer ativa, roteia a atribuição de atacante/vítima para o HostSession autoritativo
//    e suprime o handler single-player local sobre bonecos de jogadores.
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    const float raw_damage = black::RF32(base, ctx.esp + 0x0C);
    const uint32_t attacker_va = black::R32(base, ctx.esp + 0x18);
    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        if (mp::OnCharacterTakeBulletDamage(base, ctx.ecx, attacker_va, raw_damage)) {
            ::kernel::Return(ctx, 0, 7);
            return;
        }
    } else {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        const uint32_t local_player = black::LocalPlayerAddr(base);
        if (cfg.hitmarker &&
            attacker_va != 0 && attacker_va != ctx.ecx &&
            attacker_va == local_player &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            mp::NotifyHitmarker(netcore::kNoPlayer, false);
        }
        if (cfg.overhaul_enabled && local_player != 0 && ctx.ecx >= 0x10000u &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            if (attacker_va == local_player && ctx.ecx != local_player) {
                black::WF32(base, ctx.esp + 0x0Cu,
                            raw_damage * std::clamp(cfg.player_damage_mult, 0.5f, 3.0f));
            } else if (ctx.ecx == local_player && attacker_va != local_player) {
                black::WF32(base, ctx.esp + 0x0Cu,
                            raw_damage * std::clamp(cfg.enemy_damage_mult, 0.25f, 2.0f));
            }
        }
    }
    __imp__sub_001E74B0(ctx, base);
}

// 6. Dano de explosão / impacto genérico (0x001DF830 — ret 0x1C, ECX = vítima):
//    [esp+0x18] aqui é categoria de dano (1/2/3), não atacante; suprimimos sobre slots de jogadores
//    no multiplayer para evitar que explosões/scripts corrompam bonecos remotos ou dessincronizem vida.
X86_EXTERN_FUNC(__imp__sub_001DF830);
X86_FUNC(sub_001DF830) {
    const float raw_damage = black::RF32(base, ctx.esp + 0x0C);
    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        const uint32_t blast_info_va = black::R32(base, ctx.esp + 0x10);
        if (mp::OnCharacterTakeBlastDamage(base, ctx.ecx, raw_damage, blast_info_va)) {
            ::kernel::Return(ctx, 0, 7);
            return;
        }
    } else {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        const uint32_t local_player = black::LocalPlayerAddr(base);
        if (cfg.overhaul_enabled && local_player != 0 && ctx.ecx >= 0x10000u &&
            std::isfinite(raw_damage) && raw_damage > 0.0f) {
            const float mult = (ctx.ecx == local_player)
                                   ? std::clamp(cfg.enemy_damage_mult, 0.25f, 2.0f)
                                   : std::clamp(cfg.player_damage_mult, 0.5f, 3.0f);
            black::WF32(base, ctx.esp + 0x0Cu, raw_damage * mult);
        }
    }
    __imp__sub_001DF830(ctx, base);
}

// 7. Atualização do pool de personagens (0x001CCBB0 — ESI = pool, ret 0x04):
//    Antes que o motor processe os atores do pool, sincroniza posição, bbox e partição espacial
//    (world+0x4960) de cada boneco remoto via kFnCharSetPos (0x001CE660) usando vetor alinhado,
//    e aplica pedidos pendentes de compra/troca de loadout em Character+0x278.
X86_EXTERN_FUNC(__imp__sub_001CCBB0);
X86_FUNC(sub_001CCBB0) {
    {
        static constexpr ::hle::Map kSpawnActorMap = ::hle::Make({
            ::hle::Loc::ecx, ::hle::Loc::edi,
            ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack,
            ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack,
            ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack,
        });
        uint8_t sp_slot = 0;
        uint32_t sp_name_va = 0, sp_seq_va = 0, sp_arch = 0, sp_wh_lo = 0, sp_wh_hi = 0, sp_spawner_va = 0;
        while (mp::PrepareRemoteActorSpawn(base, sp_slot, sp_name_va, sp_seq_va, sp_arch,
                                           sp_wh_lo, sp_wh_hi, sp_spawner_va)) {
            const uint32_t ai_ctrl = ::hle::Call(
                ctx, black::kFnSpawnAllyFromPool, kSpawnActorMap,
                {sp_name_va, sp_seq_va, sp_arch, 1u, sp_wh_lo, sp_wh_hi, sp_spawner_va,
                 sp_wh_lo, sp_wh_hi, 0u, 0u, 0u, 0xFFFFFFFFu, 0u});
            mp::CompleteRemoteActorSpawn(base, sp_slot, ai_ctrl);
        }

        static constexpr ::hle::Map kFreeMap = ::hle::Make({::hle::Loc::stack});
        static constexpr ::hle::Map kSpawnMap = ::hle::Make(
            {::hle::Loc::edi, ::hle::Loc::stack, ::hle::Loc::stack, ::hle::Loc::stack});
        static constexpr ::hle::Map kEquipMap = ::hle::Make({::hle::Loc::ecx, ::hle::Loc::edx});

        uint8_t eng_prim = 0xFFu;
        uint8_t eng_sec = 0xFFu;
        uint8_t equip_slot = 0u;
        uint8_t tact_flags = 0u;
        if (mp::ConsumeWeaponApplyRequest(base, eng_prim, eng_sec, equip_slot, tact_flags)) {
            const uint32_t ws = black::R32(base, black::kWeaponSystem);
            const uint32_t player = black::LocalPlayerAddr(base);
            const uint32_t slot_mgr = player + black::kCharSlotMgr;
            const uint32_t slots_arr = black::R32(base, player + black::kCharSlotsPtr);
            const uint8_t slot_count = std::max<uint8_t>(1u, black::R8(base, player + black::kCharSlotCount));
            const uint32_t res_ptr = black::R32(base, player + black::kCharReserveAmmoPtr);

            if (ws != 0) {
                ::hle::Call(ctx, black::kFnWeaponFreeAll, kFreeMap, {slot_mgr});

                uint32_t w0 = 0;
                uint32_t w1 = 0;
                if (eng_prim != 0xFFu) {
                    w0 = ::hle::Call(ctx, black::kFnWeaponSpawn, kSpawnMap,
                                     {ws, player, static_cast<uint32_t>(eng_prim), 0u});
                    if (res_ptr >= 0x10000u) black::W8(base, res_ptr + eng_prim, 6u);
                    if (w0 && (tact_flags & netcore::kLoadoutTactical)) black::W8(base, w0 + 0x108u, 1u);
                }
                if (eng_sec != 0xFFu && (w0 == 0 || slot_count >= 2u)) {
                    w1 = ::hle::Call(ctx, black::kFnWeaponSpawn, kSpawnMap,
                                     {ws, player, static_cast<uint32_t>(eng_sec), 0u});
                    if (res_ptr >= 0x10000u) black::W8(base, res_ptr + eng_sec, 6u);
                    if (w1 && (tact_flags & netcore::kLoadoutTactical)) black::W8(base, w1 + 0x108u, 1u);
                }

                uint32_t active_w = 0;
                if (w0 && w1 && slot_count >= 2u) {
                    black::W32(base, slots_arr + 0u, w0);
                    black::W32(base, slots_arr + 4u, w1);
                    active_w = (equip_slot == 1u) ? w1 : w0;
                    ::hle::Call(ctx, black::kFnWeaponEquip, kEquipMap, {slot_mgr, active_w});
                } else {
                    active_w = w0 ? w0 : w1;
                    black::W32(base, slots_arr + 0u, active_w);
                    if (slot_count >= 2u) black::W32(base, slots_arr + 4u, 0u);
                    if (active_w) {
                        ::hle::Call(ctx, black::kFnWeaponEquip, kEquipMap, {slot_mgr, active_w});
                    }
                }
                LOGR("hooks: weapon apply ws=0x%08X w0=0x%08X w1=0x%08X active=0x%08X slotCnt=%u activeId=%u",
                     ws, w0, w1, active_w, static_cast<unsigned>(slot_count),
                     static_cast<unsigned>(black::R8(base, player + black::kCharActiveWeaponId)));
            }
        }

        uint8_t switch_slot = 0u;
        if (mp::ConsumeSlotSwitchRequest(base, switch_slot)) {
            const uint32_t player = black::LocalPlayerAddr(base);
            const uint32_t slots_arr = black::R32(base, player + black::kCharSlotsPtr);
            const uint8_t slot_count = black::R8(base, player + black::kCharSlotCount);
            if (slots_arr && switch_slot < slot_count) {
                const uint32_t target_w = black::R32(base, slots_arr + static_cast<uint32_t>(switch_slot) * 4u);
                if (target_w) {
                    ::hle::Call(ctx, black::kFnWeaponEquip, kEquipMap, {player + black::kCharSlotMgr, target_w});
                }
            }
        }

        uint32_t rem_actor = 0;
        uint8_t rem_eng_wpn = 0xFFu;
        while (mp::ConsumeRemoteWeaponApplyRequest(base, rem_actor, rem_eng_wpn)) {
            const uint32_t ws = black::R32(base, black::kWeaponSystem);
            const uint32_t slot_mgr = rem_actor + black::kCharSlotMgr;
            const uint32_t slots_arr = black::R32(base, rem_actor + black::kCharSlotsPtr);
            if (ws != 0 && slots_arr != 0) {
                ::hle::Call(ctx, black::kFnWeaponFreeAll, kFreeMap, {slot_mgr});
                const uint32_t rw = ::hle::Call(ctx, black::kFnWeaponSpawn, kSpawnMap,
                                                {ws, rem_actor, static_cast<uint32_t>(rem_eng_wpn), 0u});
                black::W32(base, slots_arr + 0u, rw);
                if (rw) {
                    ::hle::Call(ctx, black::kFnWeaponEquip, kEquipMap, {slot_mgr, rw});
                }
                LOGR("hooks: remote actor 0x%08X equipped eng_wpn=%u (wpn=0x%08X)",
                     rem_actor, static_cast<unsigned>(rem_eng_wpn), rw);
            }
        }
    }
    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        static constexpr ::hle::Map kSetPosMap = ::hle::Make({::hle::Loc::ecx, ::hle::Loc::eax});
        for (uint8_t slot = 0; slot < netcore::kMaxPlayers; ++slot) {
            uint32_t actor_va = 0;
            uint32_t pos_vec_va = 0;
            if (mp::PrepareRemoteActorTransform(base, slot, actor_va, pos_vec_va)) {
                ::hle::Call(ctx, black::kFnCharSetPos, kSetPosMap, {actor_va, pos_vec_va});
            }
        }
        static constexpr ::hle::Map kTracerMap =
            ::hle::Make({::hle::Loc::edx, ::hle::Loc::ebx, ::hle::Loc::stack, ::hle::Loc::stack});
        static constexpr ::hle::Map kSfxMap = ::hle::Make({::hle::Loc::eax});
        uint32_t tracer_mgr_va = 0;
        uint32_t start_vec_va = 0;
        uint32_t end_vec_va = 0;
        uint32_t weapon_va = 0;
        while (mp::PrepareRemoteFireRender(base, tracer_mgr_va, start_vec_va, end_vec_va, weapon_va)) {
            ::hle::Call(ctx, black::kFnTracerEmit, kTracerMap,
                        {tracer_mgr_va, 0u, start_vec_va, end_vec_va});
            if (weapon_va != 0) {
                ::hle::Call(ctx, black::kFnWeaponFireSfx, kSfxMap, {weapon_va});
            }
        }
    }
    __imp__sub_001CCBB0(ctx, base);
}

// 8. Seleção de animação de locomoção (0x0017E280 — ESI = char+0x328 [anim_va], plain ret):
//    Injeta temporariamente velocidade horizontal e marcha S_MOV do snapshot durante a escolha de
//    animação de atores remotos, restaurando os campos em seguida para não desviar a física.
X86_EXTERN_FUNC(__imp__sub_0017E280);
X86_FUNC(sub_0017E280) {
    float saved_speed = 0.0f;
    uint8_t saved_move = 0;
    float saved_ctrl_speed = 0.0f;
    const uint32_t anim_va = ctx.esi;
    const bool patched =
        (mp::CurrentRole() != mp::Role::OfflineCampaign) &&
        mp::OnAnimSelectPre(base, anim_va, saved_speed, saved_move, saved_ctrl_speed);
    __imp__sub_0017E280(ctx, base);
    if (patched) {
        mp::OnAnimSelectPost(base, anim_va, saved_speed, saved_move, saved_ctrl_speed);
    }
}

// 9. Colisão fatal por corpo em movimento (0x001DFC30 — ECX = vítima, ret 0x10):
//    Protege slots de jogadores no multiplayer contra morte/impacto espúrio em sobreposição.
X86_EXTERN_FUNC(__imp__sub_001DFC30);
X86_FUNC(sub_001DFC30) {
    if (mp::CurrentRole() != mp::Role::OfflineCampaign &&
        mp::ShouldProtectActorFromCrush(base, ctx.ecx)) {
        ::kernel::Return(ctx, 0, 4);
        return;
    }
    __imp__sub_001DFC30(ctx, base);
}

// 10. Morte por esmagamento físico (0x001D6000 — EDX = vítima, ret 0x04):
//     Protege slots de jogadores no multiplayer contra esmagamento durante teleporte/respawn.
X86_EXTERN_FUNC(__imp__sub_001D6000);
X86_FUNC(sub_001D6000) {
    if (mp::CurrentRole() != mp::Role::OfflineCampaign &&
        mp::ShouldProtectActorFromCrush(base, ctx.edx)) {
        ::kernel::Return(ctx, 0, 1);
        return;
    }
    __imp__sub_001D6000(ctx, base);
}

// 11. Rotina unificada de disparo da arma (0x001DBD50 — [esp+4] = FireContext*, ret 0x04):
//     Captura o momento exato em que o jogador local dispara e envia FireEvent (com hit claim se houver);
//     em Campanha Overhaul, ajusta temporariamente WeaponDef+0x14 (spread) restaurando imediatamente após.
X86_EXTERN_FUNC(__imp__sub_001DBD50);
X86_FUNC(sub_001DBD50) {
    uint32_t weapon_va = 0;
    bool track_local_shot = false;
    thread_local bool in_spread_override = false;
    uint32_t spread_wdef_va = 0;
    float saved_spread = 0.0f;
    bool restore_spread = false;

    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        weapon_va = black::R32(base, ctx.esp + 4);
        track_local_shot = mp::OnLocalWeaponFirePre(base, weapon_va);
    } else if (!in_spread_override) {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        if (cfg.overhaul_enabled && std::fabs(cfg.recoil_mult - 1.0f) > 1e-3f) {
            const uint32_t fire_ctx = black::R32(base, ctx.esp + 4);
            const uint32_t handler_va = (fire_ctx >= 0x10000u) ? black::R32(base, fire_ctx + 4u) : 0u;
            const uint32_t wdef_va = (fire_ctx >= 0x10000u) ? black::R32(base, fire_ctx + 0x0Cu) : 0u;
            const uint32_t owner_va = (handler_va >= 0x10000u) ? black::R32(base, handler_va + 0xF0u) : 0u;
            const uint32_t local_player = black::LocalPlayerAddr(base);
            if (local_player != 0 && owner_va == local_player && wdef_va >= 0x10000u) {
                const float orig = black::RF32(base, wdef_va + 0x14u);
                if (std::isfinite(orig) && orig > 0.0f) {
                    spread_wdef_va = wdef_va;
                    saved_spread = orig;
                    restore_spread = true;
                    in_spread_override = true;
                    black::WF32(base, wdef_va + 0x14u, orig * std::clamp(cfg.recoil_mult, 0.1f, 1.5f));
                }
            }
        }
    }

    __imp__sub_001DBD50(ctx, base);

    if (restore_spread) {
        black::WF32(base, spread_wdef_va + 0x14u, saved_spread);
        in_spread_override = false;
    }
    if (track_local_shot) {
        mp::OnLocalWeaponFirePost(base, weapon_va);
    }
}

// 12. Atualização de FOV / zoom de câmera (0x001BB110):
//     Mantém 70° no vanilla quando fov_deg == 70, ou ajusta *kCamera + 0x750 preservando zoom ADS.
X86_EXTERN_FUNC(__imp__sub_001BB110);
X86_FUNC(sub_001BB110) {
    __imp__sub_001BB110(ctx, base);
    const common::EnhancedConfig cfg = common::GetEnhancedConfig();
    if (std::fabs(cfg.fov_deg - 70.0f) <= 0.25f || !black::IsInLevel(base)) return;

    const uint32_t cam = black::R32(base, black::kCamera);
    const uint32_t player = black::LocalPlayerAddr(base);
    if (!cam || !player || black::R8(base, cam + black::kCameraOverrideFov) == 0) return;

    const float raw_zoom = black::RF32(base, player + black::kCharZoomFactor);
    const float zoom = (std::isfinite(raw_zoom) && raw_zoom >= 0.5f && raw_zoom <= 12.0f) ? raw_zoom : 1.0f;
    black::WF32(base, cam + black::kCameraFovDeg, std::clamp(cfg.fov_deg, 55.0f, 115.0f) / zoom);
}

// 13. Atualização de mira em 1ª pessoa (0x001BD3F0 — ESI = PlayerController, ret 0x08):
//     Quando EnhancedConfig::mouse_direct estiver ativo (opt-in), aplica delta linear 1:1 do mouse
//     em graus diretamente em [ctrl+0x18] (yaw) e [ctrl+0x1C] (pitch), sem deadzone nem curva analógica.
X86_EXTERN_FUNC(__imp__sub_001BD3F0);
X86_FUNC(sub_001BD3F0) {
    const uint32_t ctrl_va = ctx.esi;
    __imp__sub_001BD3F0(ctx, base);

    const common::EnhancedConfig cfg = common::GetEnhancedConfig();
    if (!cfg.mouse_direct || !black::IsInLevel(base)) return;

    const uint32_t player = black::LocalPlayerAddr(base);
    if (!player || black::R32(base, player + black::kCharState) != 0) {
        common::ClearDirectMouseLook();
        return;
    }
    if (black::R32(base, ctrl_va + black::kControlChar) != player) return;

    float mdx = 0.0f;
    float mdy = 0.0f;
    if (!common::ConsumeDirectMouseLook(mdx, mdy)) return;

    const float raw_zoom = black::RF32(base, player + black::kCharZoomFactor);
    const float zoom = (std::isfinite(raw_zoom) && raw_zoom >= 1.0f && raw_zoom <= 12.0f) ? raw_zoom : 1.0f;
    const float ads_scale = (zoom > 1.05f) ? (cfg.ads_sens_mult / zoom) : 1.0f;

    // Sinais fixos e independentes do perfil salvo do jogo (sensibilidade negativa / "inverter Y" do
    // menu original variam entre saves): mouse para a direita vira a câmera para a direita e mouse
    // para baixo olha para baixo; só a opção "Inverter Eixo Y" da página inverte o pitch.
    const float y_inv = cfg.invert_y ? -1.0f : 1.0f;

    float yaw = black::RF32(base, ctrl_va + black::kControlAngles + 8u);
    float pitch = black::RF32(base, ctrl_va + black::kControlAngles + 12u);
    if (!std::isfinite(yaw)) yaw = 0.0f;
    if (!std::isfinite(pitch)) pitch = 0.0f;

    // Convenção do motor: yaw positivo vira para a esquerda; pitch positivo olha para baixo.
    yaw -= mdx * cfg.mouse_sens * ads_scale;
    pitch += mdy * cfg.mouse_sens * ads_scale * y_inv;

    while (yaw >= 360.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;
    pitch = std::clamp(pitch, -70.0f, 70.0f);

    black::WF32(base, ctrl_va + black::kControlAngles + 8u, yaw);
    black::WF32(base, ctrl_va + black::kControlAngles + 0u, yaw);
    black::WF32(base, ctrl_va + black::kControlAngles + 12u, pitch);
}

namespace {
thread_local uint32_t g_campaign_reload_reserve_va = 0;
}  // namespace

// 14. Atualização / disparo / recarga da arma (0x001EEFD0 — stdcall [esp+4] = WeaponHandler*, ret 0x20):
//     No modo Campanha Overhaul, captura o contexto da reserva de munição (+0xFC) durante recargas
//     do jogador local e escala o impulso de recuo gravado em Character+0x4C4 por recoil_mult.
X86_EXTERN_FUNC(__imp__sub_001EEFD0);
X86_FUNC(sub_001EEFD0) {
    if (mp::CurrentRole() == mp::Role::OfflineCampaign) {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        if (cfg.overhaul_enabled) {
            const uint32_t handler_va = black::R32(base, ctx.esp + 4u);
            const uint32_t owner_va = (handler_va >= 0x10000u) ? black::R32(base, handler_va + 0xF0u) : 0u;
            const uint32_t local_player = black::LocalPlayerAddr(base);
            if (local_player != 0 && owner_va == local_player) {
                const uint32_t prev_reserve = g_campaign_reload_reserve_va;
                g_campaign_reload_reserve_va = cfg.ammo_bonus_enabled ? black::R32(base, handler_va + 0xFCu) : 0u;
                const float pre_kick = black::RF32(base, owner_va + 0x4C4u);

                __imp__sub_001EEFD0(ctx, base);

                g_campaign_reload_reserve_va = prev_reserve;
                const float post_kick = black::RF32(base, owner_va + 0x4C4u);
                if (std::isfinite(pre_kick) && std::isfinite(post_kick) && post_kick != pre_kick) {
                    const float mult = std::clamp(cfg.recoil_mult, 0.1f, 1.5f);
                    const float scaled = pre_kick + (post_kick - pre_kick) * mult;
                    if (std::isfinite(scaled)) {
                        black::WF32(base, owner_va + 0x4C4u, scaled);
                    }
                }
                return;
            }
        }
    }
    __imp__sub_001EEFD0(ctx, base);
}

// 15. Impulso secundário / flinch de câmera (0x001B6690 — EAX = Character*, ret 0x08):
//     Escala choques visuais aplicados diretamente em Character+0x4C4 na Campanha Overhaul.
X86_EXTERN_FUNC(__imp__sub_001B6690);
X86_FUNC(sub_001B6690) {
    if (mp::CurrentRole() == mp::Role::OfflineCampaign) {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        if (cfg.overhaul_enabled) {
            const uint32_t char_va = ctx.eax;
            const uint32_t local_player = black::LocalPlayerAddr(base);
            if (local_player != 0 && char_va == local_player) {
                const float pre_kick = black::RF32(base, char_va + 0x4C4u);
                __imp__sub_001B6690(ctx, base);
                const float post_kick = black::RF32(base, char_va + 0x4C4u);
                if (std::isfinite(pre_kick) && std::isfinite(post_kick) && post_kick != pre_kick) {
                    const float mult = std::clamp(cfg.recoil_mult, 0.1f, 1.5f);
                    const float scaled = pre_kick + (post_kick - pre_kick) * mult;
                    if (std::isfinite(scaled)) {
                        black::WF32(base, char_va + 0x4C4u, scaled);
                    }
                }
                return;
            }
        }
    }
    __imp__sub_001B6690(ctx, base);
}

// 16. Consumo de munição da reserva na recarga (0x001B57A0 — ESI = uint16*, EDX = ammo_type, CL = req):
//     Economia Reloaded (+50% na reserva ao recarregar): reembolsa metade dos projéteis consumidos
//     quando chamado no contexto real de recarga do jogador local em Campanha Overhaul.
X86_EXTERN_FUNC(__imp__sub_001B57A0);
X86_FUNC(sub_001B57A0) {
    const uint32_t reserve_va = ctx.esi;
    const uint32_t ammo_type = ctx.edx;
    const uint16_t req = static_cast<uint16_t>(ctx.ecx & 0xFFu);
    const bool eligible = (mp::CurrentRole() == mp::Role::OfflineCampaign &&
                           g_campaign_reload_reserve_va != 0u &&
                           reserve_va == g_campaign_reload_reserve_va &&
                           ammo_type < 64u);

    __imp__sub_001B57A0(ctx, base);

    if (eligible && (ctx.eax & 0xFFu) != 0u && req > 0u) {
        const common::EnhancedConfig cfg = common::GetEnhancedConfig();
        if (cfg.overhaul_enabled && cfg.ammo_bonus_enabled) {
            thread_local uint32_t odd_reload_toggle = 0u;
            uint16_t refund = static_cast<uint16_t>(req / 2u);
            if ((req & 1u) != 0u && ((odd_reload_toggle++ & 1u) == 0u)) {
                ++refund;
            }
            if (refund > 0u) {
                const uint32_t cell_va = reserve_va + ammo_type * 2u;
                const uint16_t cur = black::R16(base, cell_va);
                const uint32_t restored = std::min<uint32_t>(65535u, static_cast<uint32_t>(cur) + refund);
                black::W16(base, cell_va, static_cast<uint16_t>(restored));
            }
        }
    }
}

namespace {
void LogStreamErrorContext(const uint8_t* base, const X86Context& ctx, const char* tag) {
    const uint32_t ret_va = (ctx.esp >= 0x10000u) ? black::R32(base, ctx.esp) : 0u;
    const uint32_t mgr = (base) ? black::R32(base, 0x002D45D8u) : 0u;
    const uint32_t slot = (mgr >= 0x10000u) ? black::R32(base, mgr + 0xDE4u) : 0xFFFFFFFFu;
    char path[64]{};
    if (mgr >= 0x10000u && slot < 12u) {
        const uint32_t s_va = mgr + slot * 0x13Cu + 0x10u;
        for (int i = 0; i < 63; ++i) {
            const uint8_t ch = black::R8(base, s_va + static_cast<uint32_t>(i));
            path[i] = (ch >= 0x20u && ch < 0x7Fu) ? static_cast<char>(ch) : '\0';
            if (ch == 0u) break;
        }
    }
    LOGR("DISC-DIAG(%s): ret=0x%08X mgr=0x%08X slot=%u path='%s'",
         tag, ret_va, mgr, slot, path);
    if (mgr >= 0x10000u) {
        for (uint32_t s = 0; s < 10u; ++s) {
            const uint32_t sva = mgr + s * 0x13Cu;
            const uint32_t st = black::R32(base, sva + 0x08u);
            const uint8_t act = black::R8(base, sva + 0x135u);
            const uint8_t ch0 = black::R8(base, sva + 0x10u);
            if (st != 0 || act != 0 || ch0 != 0) {
                char sp[64]{};
                for (int i = 0; i < 63; ++i) {
                    const uint8_t ch = black::R8(base, sva + 0x10u + static_cast<uint32_t>(i));
                    sp[i] = (ch >= 0x20u && ch < 0x7Fu) ? static_cast<char>(ch) : '\0';
                    if (ch == 0u) break;
                }
                LOGR("  stream[%u]: st=%u act=%u f='%s'", s, st, static_cast<unsigned>(act), sp);
            }
        }
    }
}
}  // namespace

X86_EXTERN_FUNC(__imp__sub_0020A100);
X86_FUNC(sub_0020A100) {
    LogStreamErrorContext(base, ctx, "20A100");
    __imp__sub_0020A100(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0020B6D0);
X86_FUNC(sub_0020B6D0) {
    LogStreamErrorContext(base, ctx, "20B6D0");
    __imp__sub_0020B6D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0020B3B0);
X86_FUNC(sub_0020B3B0) {
    LogStreamErrorContext(base, ctx, "20B3B0");
    __imp__sub_0020B3B0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00147FD0);
X86_FUNC(sub_00147FD0) {
    const uint32_t lo = black::R32(base, ctx.esp + 8u);
    const uint32_t hi = black::R32(base, ctx.esp + 12u);
    LOGR("FPGUN-LOAD(147FD0): fl=%u slot=%u lo=0x%08X hi=0x%08X",
         ctx.eax & 0xFFu, black::R32(base, ctx.esp + 4u), lo, hi);
    __imp__sub_00147FD0(ctx, base);
}

// 15. Controle de Reproducao de Videos / Cutscenes (0x001F3630):
//     Interceta a inicializacao de videos XMV (LO_N_US / Logos).
//     Com skip_intro ativo, apos a chamada da implementacao original que configura
//     o controlador de video em ESI, marca os logos como concluidos ([ctrl+0x1B8]=0, [ctrl+0x1B9]=1),
//     fazendo o motor transicionar imediatamente para FEMain.bin em < 1 segundo.
X86_EXTERN_FUNC(__imp__sub_001F3630);
X86_FUNC(sub_001F3630) {
    const uint32_t ctrl_va = ctx.esi;
    const uint32_t name_va = ctx.edi;
    char movie_name[64]{};
    if (name_va >= 0x10000u) {
        for (int i = 0; i < 63; ++i) {
            const uint8_t ch = black::R8(base, name_va + static_cast<uint32_t>(i));
            movie_name[i] = (ch >= 0x20u && ch < 0x7Fu) ? static_cast<char>(ch) : '\0';
            if (ch == 0u) break;
        }
    }
    const bool is_intro = (std::strstr(movie_name, "LO_") != nullptr ||
                           std::strstr(movie_name, "lo_") != nullptr ||
                           std::strstr(movie_name, "Logos") != nullptr ||
                           std::strstr(movie_name, "logos") != nullptr);

    g_active_movie_is_intro.store(is_intro, std::memory_order_relaxed);
    g_active_movie_ctrl.store(ctrl_va, std::memory_order_release);
    LOGR("sub_001F3630: Inicializando video XMV '%s' (ctrl=0x%08X, intro=%d).",
         movie_name, ctrl_va, is_intro ? 1 : 0);

    __imp__sub_001F3630(ctx, base);
}
