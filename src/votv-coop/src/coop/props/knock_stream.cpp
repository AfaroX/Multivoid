// coop/props/knock_stream.cpp -- see coop/props/knock_stream.h.

#include "coop/props/knock_stream.h"

#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cmath>
#include <cstdint>

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
bool g_watched = false, g_saidLive = false;
uint64_t g_knocks = 0;

void OnHitPost(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host || !c.object) return;
    void* prop = R::OuterOf(c.object);  // the component's outer is the prop it rides on
    if (!prop || !ue_wrap::prop::IsDescendantOfProp(prop)) return;
    const ue_wrap::prop::VelocityState v = ue_wrap::prop::GetPhysicsVelocity(prop);
    if (!v.ok) return;
    const float sp = std::sqrt(v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y +
                               v.linearCmS.Z * v.linearCmS.Z);
    if (sp < kMovingCmS) return;
    // Refused there for a prop in a hand or with no wire identity; a prop already streaming restarts its
    // rest clock.
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
