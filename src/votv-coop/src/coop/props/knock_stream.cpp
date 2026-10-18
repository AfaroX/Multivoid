// coop/props/knock_stream.cpp -- see coop/props/knock_stream.h.

#include "coop/props/knock_stream.h"

#include "coop/net/session.h"
#include "coop/element/registry.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"   // IsParked: another peer streams it
#include "coop/props/remote_prop.h"         // IsActorUnderAnyDrive: another peer holds or coasts it
#include "coop/player/local_streams.h"      // LocalPlayer, LastHeldActor: this player and its hand
#include "coop/player/players_registry.h"   // IsPuppet: another player's body

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace coop::props::knock_stream {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The component's contact report, Blueprint-internal from its hit delegate, so the seam is the body loop.
UE_FACT_FN(kRowHitCpp, "knock_stream", L"comp_physicsImpact_C", L"hitCpp", Declared);
constexpr int kTag = 0x4B4E4F43;  // 'KNOC'

// A contact that leaves the prop slower than this is a touch, not a knock: it would open a stream only to
// close it half a second later with the pose every peer already has.
constexpr float kMovingCmS = 5.0f;

std::atomic<coop::net::Session*> g_session{nullptr};
R::ParamOffset g_pOther(L"OtherActor");

// Another peer moves it here: its stream, its hand or its coast.
bool OthersMove(void* actor) {
    return actor && (coop::prop_drive_stream::IsParked(actor) || coop::remote_prop::IsActorUnderAnyDrive(actor));
}

// This peer moved `other`: this player, the prop in its hand, or a prop it streams.
bool Mine(void* other) {
    return other && (other == coop::local_streams::LocalPlayer() || other == coop::local_streams::LastHeldActor() ||
                     coop::prop_drive_host::OwnsActor(other));
}
bool g_watched = false, g_saidLive = false;
uint64_t g_knocks = 0;

void OnHitPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || !c.object) return;
    void* prop = R::OuterOf(c.object);  // the component's outer is the prop it rides on
    if (!prop || !ue_wrap::prop::IsDescendantOfProp(prop) || OthersMove(prop)) return;
    void* other = nullptr;
    const int32_t oo = c.locals ? g_pOther.Of(c.function) : -1;
    if (oo >= 0) std::memcpy(&other, c.locals + oo, sizeof(other));
    // Who set it moving: this peer when the other actor is its own; the host for a world knock nobody's
    // player or stream caused. Another player's body or another peer's prop is that peer's knock.
    const bool host = s->role() == coop::net::Role::Host;
    const bool theirs = other && (OthersMove(other) || coop::players::Registry::Get().IsPuppet(other));
    if (!Mine(other) && !(host && !theirs) && !coop::prop_drive_host::OwnsActor(prop)) return;
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(prop);
    if (!v.ok) return;
    const float sp = std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                               v.linearCmS.Z * v.linearCmS.Z);
    if (sp < kMovingCmS) return;
    // A prop with no element id has no copy a peer could move -- a client's drop the host spawned this
    // very tick, before its id is minted, is the usual one -- so its knock is no news.
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(prop);
    if (eid == coop::element::kInvalidId || eid == 0u) return;   // the channel names it by its wire id
    // Refused there for a prop in a hand; a prop already streaming restarts its rest clock.
    coop::prop_drive_host::Coast(prop, "knock");
    ++g_knocks;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!g_watched) g_watched = sg::WatchName(kRowHitCpp, kTag, nullptr, &OnHitPost);
    if (!g_watched || g_saidLive) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowHitCpp, kTag)) return;
    g_saidLive = true;
    if (sg::NameWatchLive(kRowHitCpp, kTag)) UE_LOGI("knock_stream: the contact watch is live");
    else UE_LOGE("knock_stream: the contact watch settled dead -- a knocked prop rests where each peer puts it");
}

void OnDisconnect() {
    if (g_knocks) UE_LOGI("knock_stream: session end -- %llu knocks streamed", static_cast<unsigned long long>(g_knocks));
    g_knocks = 0;
}

}  // namespace coop::props::knock_stream
