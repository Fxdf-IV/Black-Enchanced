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

// 0. Atualização por quadro do player de filme XMV (0x000C4570; ESI = player). Os três donos do player
//    (sub_00209D40, sub_00209DC0 e sub_000D3620) encerram o vídeo quando [player+0x62] ("filme terminou",
//    definido em 0x000C4704) vira 1 e então liberam o decodificador (sub_000DB190). Com o pulo ativo (botão
//    "Pular Intro" do cliente web) marcamos esse byte depois da atualização normal, e o próprio jogo executa
//    o encerramento limpo.
X86_EXTERN_FUNC(__imp__sub_000C4570);
X86_FUNC(sub_000C4570) {
    const uint32_t player = ctx.esi;
    __imp__sub_000C4570(ctx, base);
    if (common::g_skip_cutscenes.load(std::memory_order_relaxed) && player >= 0x10000u) {
        static std::atomic<uint32_t> logged{0};
        if (black::R8(base, player + 0x62u) == 0u && logged.fetch_add(1) < 8u) {
            LOGI("skip_cutscenes: forcando fim do filme XMV (player=0x%08X)", player);
        }
        black::W8(base, player + 0x62u, 1u);
    }
}

// 1. Quadro principal (0x0020F6A0): drena pacotes e atualiza atores remotos antes da simulação;
//    lê e transmite o estado do jogador local depois da simulação.
X86_EXTERN_FUNC(__imp__sub_0020F6A0);
X86_FUNC(sub_0020F6A0) {
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
    } else if (common::GetEnhancedConfig().hitmarker &&
               attacker_va != 0 && attacker_va != ctx.ecx &&
               attacker_va == black::LocalPlayerAddr(base) &&
               std::isfinite(raw_damage) && raw_damage > 0.0f) {
        mp::NotifyHitmarker(netcore::kNoPlayer, false);
    }
    __imp__sub_001E74B0(ctx, base);
}

// 6. Dano de explosão / impacto genérico (0x001DF830 — ret 0x1C, ECX = vítima):
//    [esp+0x18] aqui é categoria de dano (1/2/3), não atacante; suprimimos sobre slots de jogadores
//    no multiplayer para evitar que explosões/scripts corrompam bonecos remotos ou dessincronizem vida.
X86_EXTERN_FUNC(__imp__sub_001DF830);
X86_FUNC(sub_001DF830) {
    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        const float raw_damage = black::RF32(base, ctx.esp + 0x0C);
        if (mp::ShouldSuppressBlastDamage(base, ctx.ecx, raw_damage)) {
            ::kernel::Return(ctx, 0, 7);
            return;
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
                LOGI("hooks: weapon apply ws=0x%08X w0=0x%08X w1=0x%08X active=0x%08X slotCnt=%u activeId=%u",
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
                LOGI("hooks: remote actor 0x%08X equipped eng_wpn=%u (wpn=0x%08X)",
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

// 11. Rotina unificada de disparo da arma (0x001DBD50 — [esp+4] = Weapon*, ret 0x04):
//     Captura o momento exato em que o jogador local dispara e envia FireEvent (com hit claim se houver).
X86_EXTERN_FUNC(__imp__sub_001DBD50);
X86_FUNC(sub_001DBD50) {
    uint32_t weapon_va = 0;
    bool track_local_shot = false;
    if (mp::CurrentRole() != mp::Role::OfflineCampaign) {
        weapon_va = black::R32(base, ctx.esp + 4);
        track_local_shot = mp::OnLocalWeaponFirePre(base, weapon_va);
    }
    __imp__sub_001DBD50(ctx, base);
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

    const float sy = black::RF32(base, ctrl_va + black::kControlSensYaw);
    const float sp = black::RF32(base, ctrl_va + black::kControlSensPitch);
    const float yaw_sign = (std::isfinite(sy) && sy < 0.0f) ? -1.0f : 1.0f;
    const float pitch_sign = (std::isfinite(sp) && sp < 0.0f) ? -1.0f : 1.0f;
    const bool guest_invert_y = (black::R8(base, ctrl_va + 0x101u) != 0);
    const float y_inv = (cfg.invert_y != guest_invert_y) ? -1.0f : 1.0f;

    float yaw = black::RF32(base, ctrl_va + black::kControlAngles + 8u);
    float pitch = black::RF32(base, ctrl_va + black::kControlAngles + 12u);
    if (!std::isfinite(yaw)) yaw = 0.0f;
    if (!std::isfinite(pitch)) pitch = 0.0f;

    yaw += yaw_sign * mdx * cfg.mouse_sens * ads_scale;
    pitch += pitch_sign * (-mdy) * cfg.mouse_sens * ads_scale * y_inv;

    while (yaw >= 360.0f) yaw -= 360.0f;
    while (yaw < -180.0f) yaw += 360.0f;
    pitch = std::clamp(pitch, -70.0f, 70.0f);

    black::WF32(base, ctrl_va + black::kControlAngles + 8u, yaw);
    black::WF32(base, ctrl_va + black::kControlAngles + 0u, yaw);
    black::WF32(base, ctrl_va + black::kControlAngles + 12u, pitch);
}
