// coop/props/prop_hit_authority.cpp -- see coop/props/prop_hit_authority.h.

#include "coop/props/prop_hit_authority.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/intent_authority.h"     // IntentTarget: the sender's reach to the prop
#include "coop/element/registry.h"             // EidForActor, LivePropActor
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/local_streams.h"         // LastHeldActor: the holder's copy answers
#include "coop/player/players_registry.h"      // Local, Puppet
#include "coop/player/remote_player.h"
#include "coop/props/active_drive.h"           // NowMs
#include "coop/props/prop_key_index.h"         // Find
#include "coop/props/remote_prop.h"            // IsActorUnderAnyDrive: another peer's hand holds it

#include "ue_wrap/actors/prop.h"               // IsDescendantOfProp, GetInteractableKeyString, GetStaticMesh
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/ufunction_hook.h"       // FrameChainRunsOn
#include "ue_wrap/engine/engine.h"           // TryGetActorLocation
#include "ue_wrap/engine/hit_result.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

namespace coop::props::prop_hit_authority {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

// The swing's and an explosion's entry, and the physics contact's two: every prop's damage starts at one.
UE_FACT_FN(kRowAddDamage, "prop_hit_authority", nullptr, L"addDamage", AnyClass);
UE_FACT_FN(kRowPhysDamage, "prop_hit_authority", nullptr, L"receivedPhyiscsDamage", AnyClass);
UE_FACT_FN(kRowImpactCpp, "prop_hit_authority", nullptr, L"impactDamageCPP", AnyClass);
constexpr int kTagAdd = 0x50484941, kTagPhys = 0x50484950, kTagImpact = 0x50484943;  // 'PHIA' 'PHIP' 'PHIC'

constexpr float    kReachUU = 600.0f;        // a swing's reach, with a pose's lag on top
constexpr float    kMaxDamage = 100000.0f;   // a sane bound on a wire amount, far above any swing's
constexpr float    kMaxImpact = 1.0e7f;
constexpr int      kMaxPerSecond = 20;       // swings per second a client sends, or a host takes per slot

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[3] = {}, g_saidLive[3] = {};
uint64_t g_forwarded = 0, g_refusedHere = 0, g_ran = 0, g_refusedHost = 0;
uint64_t g_rateWindowMs = 0;
int      g_rateCount = 0;
uint64_t g_slotWindowMs[coop::net::kMaxPeers] = {};
int      g_slotCount[coop::net::kMaxPeers] = {};

R::ParamOffset g_offDamage(L"damage"), g_offHit(L"hit"), g_offImpact(L"impact"), g_offSkip(L"skipSetting");

bool Off() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::prop_hit_authority_off);
    return s;
}

coop::net::Session* Running() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

// FHitResult's impact point, by the engine struct's own reflection, resolved once.
int32_t ImpactPointOffset() {
    static bool tried = false;
    static int32_t off = -1;
    if (!tried) {
        tried = true;
        if (void* hr = R::FindObject(L"HitResult", L"ScriptStruct")) off = R::FindPropertyOffset(hr, L"ImpactPoint");
        if (off < 0) UE_LOGW("prop_hit_authority: FHitResult.ImpactPoint did not resolve -- a forwarded hit lands at the prop");
    }
    return off;
}

bool RateOk(uint64_t& windowMs, int& count) {
    const uint64_t now = coop::active_drive::NowMs();
    if (now - windowMs >= 1000) { windowMs = now; count = 0; }
    return ++count <= kMaxPerSecond;
}

bool Finite3(float x, float y, float z, float bound) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) <= bound &&
           std::fabs(y) <= bound && std::fabs(z) <= bound;
}

void Note(uint64_t& counter, const char* what, const sg::Call& c) {
    if (++counter <= 10 || counter % 200 == 0)
        UE_LOGI("prop_hit_authority: %ls.%ls on '%ls' %s (%llu)", R::ClassNameOf(c.object).c_str(),
                R::ToString(R::NameOf(c.function)).c_str(), ue_wrap::prop::GetInteractableKeyString(c.object).c_str(),
                what, static_cast<unsigned long long>(counter));
}

// CLIENT: this player's swing at a prop it does not hold, named to the host with the swing's own arguments.
void Forward(coop::net::Session* s, const sg::Call& c) {
    if (!RateOk(g_rateWindowMs, g_rateCount)) return;
    coop::net::PropHitIntentPayload p{};
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, p.key);
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    p.elementId = eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    if (p.key.len == 0 && p.elementId == 0u) return;   // nothing the host could name it by
    const int32_t od = g_offDamage.Of(c.function), oh = g_offHit.Of(c.function);
    const int32_t oi = g_offImpact.Of(c.function), os = g_offSkip.Of(c.function);
    if (od < 0 || !c.locals) return;
    std::memcpy(&p.damage, c.locals + od, sizeof(float));
    ue_wrap::FVector at{};
    if (ue_wrap::engine::TryGetActorLocation(c.object, at)) { p.pointX = at.X; p.pointY = at.Y; p.pointZ = at.Z; }
    const int32_t ip = ImpactPointOffset();
    if (oh >= 0 && ip >= 0 && R::FindParamSize(c.function, L"hit") >= ip + 12) {
        float pt[3];
        std::memcpy(pt, c.locals + oh + ip, sizeof(pt));
        if (Finite3(pt[0], pt[1], pt[2], 1.0e6f) && (pt[0] != 0.f || pt[1] != 0.f || pt[2] != 0.f)) {
            p.pointX = pt[0]; p.pointY = pt[1]; p.pointZ = pt[2];
        }
    }
    if (oi >= 0) {
        float v[3];
        std::memcpy(v, c.locals + oi, sizeof(v));
        p.impactX = v[0]; p.impactY = v[1]; p.impactZ = v[2];
    }
    if (os >= 0) p.skipSetting = *(c.locals + os) != 0 ? 1 : 0;
    if (s->SendReliableToSlot(0, coop::net::ReliableKind::PropHitIntent, &p, sizeof(p))) {
        if (++g_forwarded <= 20 || g_forwarded % 100 == 0)
            UE_LOGI("prop_hit_authority: this player hit %ls '%ls' (damage %.1f) -- sent to the host (%llu sent)",
                    R::ClassNameOf(c.object).c_str(), key.c_str(), p.damage, static_cast<unsigned long long>(g_forwarded));
    }
}

sg::Verdict OnDamagePre(const sg::Call& c) {
    auto* s = Running();
    if (!s || c.fromOurCode || !c.object || Off() || !ue_wrap::prop::IsDescendantOfProp(c.object))
        return sg::Verdict::Run;
    void* local = coop::players::Registry::Get().Local();
    const bool byThisPlayer = local && ue_wrap::ufunction_hook::FrameChainRunsOn(c.stack, local);
    if (s->role() == coop::net::Role::Host) {
        // The host answers every prop no other hand holds, and its own player's swing at one that is held.
        if (!coop::remote_prop::IsActorUnderAnyDrive(c.object) || byThisPlayer) return sg::Verdict::Run;
        Note(g_refusedHere, "refused: a client holds it, and its copy answers", c);
        return sg::Verdict::Cancel;
    }
    if (c.object == coop::local_streams::LastHeldActor()) return sg::Verdict::Run;   // the holder's copy
    if (c.tag == kTagAdd && byThisPlayer) {
        Forward(s, c);
        return sg::Verdict::Cancel;
    }
    Note(g_refusedHere, "refused: the host's copy answers it", c);
    return sg::Verdict::Cancel;
}

void Refuse(uint8_t slot, const std::wstring& key, uint32_t eid, const char* why) {
    ++g_refusedHost;
    if (g_refusedHost <= 20 || g_refusedHost % 50 == 0)
        UE_LOGI("prop_hit_authority: slot %u's hit on key='%ls' eid=%u not run (%s)", slot, key.c_str(), eid, why);
}

void* PawnOf(uint8_t slot) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(slot);
    return (rp && rp->valid()) ? rp->GetActor() : nullptr;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[3] = {&kRowAddDamage, &kRowPhysDamage, &kRowImpactCpp};
    const int tags[3] = {kTagAdd, kTagPhys, kTagImpact};
    bool pending = false;
    for (int i = 0; i < 3; ++i) {
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], tags[i], &OnDamagePre, nullptr);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (!pending) return;
    sg::ResolvePendingNames();
    for (int i = 0; i < 3; ++i) {
        if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], tags[i])) continue;
        g_saidLive[i] = true;
        if (sg::NameWatchLive(*rows[i], tags[i])) UE_LOGI("prop_hit_authority: the %ls watch is live", rows[i]->Name());
        else UE_LOGE("prop_hit_authority: the %ls watch settled dead -- every copy answers that entry", rows[i]->Name());
    }
}

void OnHitIntent(coop::net::Session& session, const coop::net::PropHitIntentPayload& p, uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_hit_authority::OnHitIntent");
    if (session.role() != coop::net::Role::Host || senderSlot >= coop::net::kMaxPeers) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (!RateOk(g_slotWindowMs[senderSlot], g_slotCount[senderSlot])) {
        Refuse(senderSlot, key, p.elementId, "over the rate");
        return;
    }
    if (!std::isfinite(p.damage) || std::fabs(p.damage) > kMaxDamage || !Finite3(p.pointX, p.pointY, p.pointZ, 1.0e6f) ||
        !Finite3(p.impactX, p.impactY, p.impactZ, kMaxImpact)) {
        Refuse(senderSlot, key, p.elementId, "amounts out of bounds");
        return;
    }
    if (p.elementId != 0u && !EL::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
        !EL::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
        Refuse(senderSlot, key, p.elementId, "eid in neither band");
        return;
    }
    // Key first; the eid only names a keyless prop, since a named key that does not resolve is that prop
    // gone and its eid may name another by now.
    void* prop = nullptr;
    if (!key.empty()) prop = coop::prop_key_index::Find(key);
    else if (p.elementId != 0u) prop = EL::LivePropActor(static_cast<EL::ElementId>(p.elementId));
    if (!prop) { Refuse(senderSlot, key, p.elementId, "no such prop here"); return; }
    if (!ue_wrap::prop::IsDescendantOfProp(prop)) { Refuse(senderSlot, key, p.elementId, "not a prop"); return; }
    if (coop::remote_prop::IsActorUnderAnyDrive(prop)) {
        Refuse(senderSlot, key, p.elementId, "a hand holds it, and its holder's copy answers");
        return;
    }
    const auto tok = EL::IntentTarget::ForClientIntent(session, senderSlot, kReachUU);
    if (const auto sub = tok.Authorize(prop); !sub) {
        Refuse(senderSlot, key, p.elementId, EL::OutcomeName(sub.outcome));
        return;
    }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), kRowAddDamage.Name());
    if (!fn) { Refuse(senderSlot, key, p.elementId, "addDamage did not resolve"); return; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) { Refuse(senderSlot, key, p.elementId, "addDamage's frame did not build"); return; }
    const ue_wrap::FVector point{p.pointX, p.pointY, p.pointZ};
    const ue_wrap::FVector impact{p.impactX, p.impactY, p.impactZ};
    // The hit names the host's copy and its mesh, as the swing's own trace named the client's; a hit that
    // does not build leaves the zeroed one, which a prop reading only the amount does not miss.
    if (!ue_wrap::hit_result::Write(f, L"hit", prop, ue_wrap::prop::GetStaticMesh(prop), point) && g_ran == 0)
        UE_LOGW("prop_hit_authority: the hit did not build for '%ls' -- addDamage runs with an empty one", key.c_str());
    f.Set<void*>(L"actor", PawnOf(senderSlot));
    f.Set<float>(L"damage", p.damage);
    f.Set<ue_wrap::FVector>(L"impact", impact);
    f.Set<uint8_t>(L"skipSetting", p.skipSetting ? 1 : 0);
    if (!ue_wrap::Call(prop, f)) { Refuse(senderSlot, key, p.elementId, "addDamage did not dispatch"); return; }
    if (++g_ran <= 20 || g_ran % 100 == 0)
        UE_LOGI("prop_hit_authority: HOST ran slot %u's hit on %ls '%ls' (damage %.1f, %llu run)", senderSlot,
                R::ClassNameOf(prop).c_str(), key.c_str(), p.damage, static_cast<unsigned long long>(g_ran));
}

void OnDisconnect() {
    if (g_forwarded || g_refusedHere || g_ran || g_refusedHost)
        UE_LOGI("prop_hit_authority: session end -- forwarded=%llu refused-here=%llu host-ran=%llu host-refused=%llu",
                static_cast<unsigned long long>(g_forwarded), static_cast<unsigned long long>(g_refusedHere),
                static_cast<unsigned long long>(g_ran), static_cast<unsigned long long>(g_refusedHost));
    g_forwarded = g_refusedHere = g_ran = g_refusedHost = 0;
    g_rateWindowMs = 0;
    g_rateCount = 0;
    for (int i = 0; i < coop::net::kMaxPeers; ++i) { g_slotWindowMs[i] = 0; g_slotCount[i] = 0; }
}

}  // namespace coop::props::prop_hit_authority
