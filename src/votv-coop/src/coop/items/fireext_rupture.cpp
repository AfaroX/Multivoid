// coop/items/fireext_rupture.cpp -- see coop/items/fireext_rupture.h.

#include "coop/items/fireext_rupture.h"

#include "coop/element/intent_authority.h"     // IntentTarget: the sender's reach to the extinguisher
#include "coop/element/registry.h"             // EidForActor, LivePropActor
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/local_streams.h"         // LastHeldActor: the release edge has gone out
#include "coop/player/players_registry.h"      // Local
#include "coop/props/active_drive.h"           // NowMs
#include "coop/props/prop_drive_host.h"        // Coast: the flight streams until it rests
#include "coop/props/prop_key_index.h"         // Find
#include "coop/props/prop_park.h"              // Unpark: a copy a hold still drives
#include "coop/props/remote_prop.h"            // IsActorUnderAnyDrive, EndAnyHoldOn

#include "ue_wrap/actors/prop.h"               // GetInteractableKeyString, GetStaticMesh
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // ReleaseMainPlayerGrabIfHolding
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace coop::fireext_rupture {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace EL = coop::element;

constexpr const wchar_t* kFireExtClass = L"prop_fireExt_C";
UE_FACT_FN(kRowShoot, "fireext_rupture", L"prop_fireExt_C", L"shoot", Declared);
constexpr int kTag = 0x46455854;  // 'FEXT'
// The knock that ruptures it is the player's own swing, with the extinguisher in the hand or just let go.
constexpr float kReachUU = 600.0f;
// How long a queued rupture waits for this client's release edge before it goes anyway.
constexpr uint64_t kReleaseWaitMs = 1000;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_saidLive = false;
uint64_t g_sent = 0, g_ruptured = 0, g_refused = 0, g_streamed = 0;

struct Pending {
    ue_wrap::CachedObjRef ref;
    coop::net::FireExtRupturePayload payload{};
    uint64_t queuedMs = 0;
};
std::vector<Pending> g_pending;  // game thread only

bool IsFireExt(void* actor) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == kFireExtClass) return true;
    return false;
}

// The extinguisher's own record of a rupture; a second shoot on a ruptured one is the burst's own loop.
bool IsActive(void* actor) {
    static void* sCls = nullptr;
    static int32_t sOff = -1;
    void* cls = R::ClassOf(actor);
    if (cls != sCls) { sCls = cls; sOff = R::FindPropertyOffset(cls, L"active"); }
    return sOff >= 0 && *(static_cast<const uint8_t*>(actor) + sOff) != 0;
}

coop::net::Session* Running() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running()) ? s : nullptr;
}

// CLIENT: the rupture is refused and queued for the host; the hand lets go so the host's physics can
// take the copy. HOST: its own body runs.
sg::Verdict OnShootPre(const sg::Call& c) {
    auto* s = Running();
    if (!s || s->role() != coop::net::Role::Client || !IsFireExt(c.object)) return sg::Verdict::Run;
    for (const Pending& p : g_pending)
        if (p.ref.Is(c.object)) return sg::Verdict::Cancel;  // already queued: the burst's own repeat
    Pending p;
    p.ref.Set(c.object);
    const std::wstring key = ue_wrap::prop::GetInteractableKeyString(c.object);
    if (!key.empty() && key != L"None") coop::net::WireKeyFromString(key, p.payload.key);
    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    p.payload.elementId = eid == EL::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    if (p.payload.key.len == 0 && p.payload.elementId == 0u) {
        UE_LOGW("fireext_rupture: this client's extinguisher %p has neither a key nor an eid -- nothing to name; "
                "its rupture is dropped", c.object);
        return sg::Verdict::Cancel;
    }
    p.queuedMs = coop::active_drive::NowMs();
    ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(coop::players::Registry::Get().Local(), c.object);
    g_pending.push_back(std::move(p));
    return sg::Verdict::Cancel;
}

// HOST: every rupture it runs streams. A prop still in the host's own hand is refused by the channel, and
// its release coasts it (coop/player/local_streams).
void OnShootPost(const sg::Call& c) {
    auto* s = Running();
    if (!s || s->role() != coop::net::Role::Host || !IsFireExt(c.object)) return;
    coop::prop_drive_host::Coast(c.object, "fire extinguisher rupture");
    ++g_streamed;
}

void Refuse(uint8_t slot, const std::wstring& key, uint32_t eid, const char* why) {
    ++g_refused;
    if (g_refused <= 20 || g_refused % 50 == 0)
        UE_LOGI("fireext_rupture: slot %u's extinguisher key='%ls' eid=%u not ruptured (%s)", slot, key.c_str(), eid, why);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!g_watched) g_watched = sg::WatchName(kRowShoot, kTag, &OnShootPre, &OnShootPost);
    if (g_watched && !g_saidLive) {
        sg::ResolvePendingNames();
        if (sg::NameWatchSettled(kRowShoot, kTag)) {
            g_saidLive = true;
            if (sg::NameWatchLive(kRowShoot, kTag)) UE_LOGI("fireext_rupture: the shoot watch is live");
            else UE_LOGE("fireext_rupture: the shoot watch settled dead -- a rupture stays on the screen it happened on");
        }
    }
    if (g_pending.empty()) return;
    auto* s = Running();
    if (!s || s->role() != coop::net::Role::Client) { g_pending.clear(); return; }
    // The release goes out at the hold's edge on the pump; the rupture follows it on the same FIFO, so the
    // host lets go of its driven copy before it ruptures it.
    const uint64_t now = coop::active_drive::NowMs();
    std::vector<Pending> keep;
    for (Pending& p : g_pending) {
        void* a = p.ref.Get();
        if (a && a == coop::local_streams::LastHeldActor() && now - p.queuedMs < kReleaseWaitMs) {
            keep.push_back(std::move(p));
            continue;
        }
        if (s->SendReliableToSlot(0, coop::net::ReliableKind::FireExtRupture, &p.payload, sizeof(p.payload))) {
            ++g_sent;
            UE_LOGI("fireext_rupture: this client's extinguisher eid=%u ruptured -- sent to the host (%llu sent)",
                    p.payload.elementId, static_cast<unsigned long long>(g_sent));
        }
    }
    g_pending.swap(keep);
}

void OnRupture(coop::net::Session& session, const coop::net::FireExtRupturePayload& p, uint8_t senderSlot) {
    UE_ASSERT_GAME_THREAD("fireext_rupture::OnRupture");
    if (session.role() != coop::net::Role::Host) return;
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (p.elementId != 0u && !EL::Registry::IsAllowedHostAllocatedEid(p.elementId) &&
        !EL::Registry::IsAllowedPeerAllocatedEid(p.elementId)) {
        Refuse(senderSlot, key, p.elementId, "eid in neither band");
        return;
    }
    // Key first; the eid only names a keyless one, since a named key that does not resolve is that
    // extinguisher gone and its eid may name another prop by now.
    void* ext = nullptr;
    if (!key.empty()) ext = coop::prop_key_index::Find(key);
    else if (p.elementId != 0u) ext = EL::LivePropActor(static_cast<EL::ElementId>(p.elementId));
    if (!ext) { Refuse(senderSlot, key, p.elementId, "no such extinguisher here"); return; }
    if (!IsFireExt(ext)) { Refuse(senderSlot, key, p.elementId, "not a fire extinguisher"); return; }
    if (IsActive(ext)) { Refuse(senderSlot, key, p.elementId, "already ruptured here"); return; }
    const auto tok = EL::IntentTarget::ForClientIntent(session, senderSlot, kReachUU);
    if (const auto sub = tok.Authorize(ext); !sub) {
        Refuse(senderSlot, key, p.elementId, EL::OutcomeName(sub.outcome));
        return;
    }
    // A hold the release has not yet closed still parks the copy kinematic, where the burst's forces do
    // nothing: it is ended and the body handed back first.
    if (coop::remote_prop::IsActorUnderAnyDrive(ext)) {
        coop::remote_prop::EndAnyHoldOn(ext);
        coop::prop_park::Unpark(ext, ue_wrap::prop::GetStaticMesh(ext), /*rootSimulates=*/true);
    }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(ext), kRowShoot.Name());
    if (!fn) { Refuse(senderSlot, key, p.elementId, "shoot did not resolve"); return; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(ext, f)) { Refuse(senderSlot, key, p.elementId, "shoot did not dispatch"); return; }
    ++g_ruptured;
    UE_LOGI("fireext_rupture: HOST ruptured slot %u's extinguisher key='%ls' eid=%u (%llu ruptured)", senderSlot,
            key.c_str(), p.elementId, static_cast<unsigned long long>(g_ruptured));
}

void OnDisconnect() {
    if (g_sent || g_ruptured || g_refused || g_streamed)
        UE_LOGI("fireext_rupture: session end -- sent=%llu ruptured=%llu refused=%llu streamed=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_ruptured),
                static_cast<unsigned long long>(g_refused), static_cast<unsigned long long>(g_streamed));
    g_pending.clear();
    g_sent = g_ruptured = g_refused = g_streamed = 0;
}

}  // namespace coop::fireext_rupture
