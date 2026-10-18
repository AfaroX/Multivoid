// coop/props/prop_tool_flags.cpp -- see coop/props/prop_tool_flags.h.

#include "coop/props/prop_tool_flags.h"

#include "coop/element/registry.h"     // EidForActor
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/part_index.h"     // WireKeyFor, ResolveWire: a prop or a parent's part

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"     // TryGetActorLocation, TryGetActorRotation, SetActorLocation, SetActorRotation
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>
#include <string>

namespace coop::prop_tool_flags {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

UE_FACT_FN(kRowSetPropProps, "prop_tool_flags", nullptr, L"setPropProps", AnyClass);
constexpr int kTag = 0x54464C47;  // 'TFLG'

constexpr uint8_t kStatic = 1, kFrozen = 2, kActive = 4, kSleeping = 8;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched = false, g_saidLive = false;
R::ParamOffset g_pStatic(L"static"), g_pFrozen(L"frozen"), g_pActive(L"active"), g_pSleeping(L"sleeping");

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

bool HasClassInChain(void* obj, const wchar_t* name) {
    if (!obj) return false;
    for (void* cls = R::ClassOf(obj); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == name) return true;
    return false;
}

bool ReadBool(const sg::Call& c, R::ParamOffset& p) {
    const int32_t off = p.Of(c.function);
    return off >= 0 && c.locals[off] != 0;
}

// The prop's own setter, on its own class so an override runs, with the tool's four arguments.
bool RunSetter(void* prop, uint8_t flags) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(prop), L"setPropProps");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<bool>(L"static", (flags & kStatic) != 0) && f.Set<bool>(L"frozen", (flags & kFrozen) != 0) &&
           f.Set<bool>(L"active", (flags & kActive) != 0) && f.Set<bool>(L"sleeping", (flags & kSleeping) != 0) &&
           ue_wrap::Call(prop, f);
}

void Apply(const coop::net::PropToolFlagsPayload& p, const char* why) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* prop = coop::part_index::ResolveWire(key, p.elementId);
    if (!prop) {
        UE_LOGI("prop_tool_flags: %s for '%ls' eid=%u -- no such prop here", why, key.c_str(), p.elementId);
        return;
    }
    // The pose first, so the setter's init() takes the body where the tool user's stood; then again after,
    // in case init() moved it.
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const ue_wrap::FRotator rot{p.pitch, p.yaw, p.roll};
    E::SetActorLocation(prop, at);
    E::SetActorRotation(prop, rot);
    if (!RunSetter(prop, p.flags)) {
        UE_LOGW("prop_tool_flags: %s for '%ls' -- the setter did not run here", why, key.c_str());
        return;
    }
    E::SetActorLocation(prop, at);
    E::SetActorRotation(prop, rot);
    UE_LOGI("prop_tool_flags: %s -- '%ls' static=%d frozen=%d sleeping=%d at (%.1f, %.1f, %.1f)", why, key.c_str(),
            (p.flags & kStatic) ? 1 : 0, (p.flags & kFrozen) ? 1 : 0, (p.flags & kSleeping) ? 1 : 0, p.x, p.y, p.z);
}

// The setter just ran on this machine: crossed only when the toolgun's prop-properties tool ran it.
void OnSetPropPropsPost(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || !c.locals) return;
    if (!HasClassInChain(c.callerObject, L"tool_propProps_C") || !HasClassInChain(c.object, L"prop_C")) return;
    coop::net::PropToolFlagsPayload p{};
    const std::wstring key = coop::part_index::WireKeyFor(c.object);
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(c.object);
    if (key.empty() && eid == coop::element::kInvalidId) {
        UE_LOGI("prop_tool_flags: the toolgun set %p's flags, which has no key or id -- not sent", c.object);
        return;
    }
    coop::net::WireKeyFromString(key, p.key);
    p.elementId = eid == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    p.flags = static_cast<uint8_t>((ReadBool(c, g_pStatic) ? kStatic : 0) | (ReadBool(c, g_pFrozen) ? kFrozen : 0) |
                                   (ReadBool(c, g_pActive) ? kActive : 0) | (ReadBool(c, g_pSleeping) ? kSleeping : 0));
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(c.object, at) || !E::TryGetActorRotation(c.object, rot)) return;
    p.x = at.X; p.y = at.Y; p.z = at.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    s->SendReliable(coop::net::ReliableKind::PropToolFlags, &p, sizeof(p));
    UE_LOGI("prop_tool_flags: sent the toolgun's flags 0x%02x for '%ls' eid=%u", static_cast<unsigned>(p.flags),
            key.c_str(), p.elementId);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!g_watched) g_watched = sg::WatchName(kRowSetPropProps, kTag, nullptr, &OnSetPropPropsPost);
    if (!g_watched || g_saidLive) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowSetPropProps, kTag)) return;
    g_saidLive = true;
    if (sg::NameWatchLive(kRowSetPropProps, kTag)) UE_LOGI("prop_tool_flags: the setPropProps watch is live");
    else UE_LOGE("prop_tool_flags: the setPropProps watch settled dead -- a toolgun freeze stays on its user's screen");
}

void OnToolFlags(coop::net::Session& session, const coop::net::PropToolFlagsPayload& payload, uint8_t senderSlot) {
    const bool host = session.role() == coop::net::Role::Host;
    if (!host && senderSlot != 0) return;
    Apply(payload, host ? "a client's toolgun" : "the host's");
    if (!host) return;
    // To every client but the sender: an echo would put back a freeze its user has since grabbed loose.
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (i != senderSlot)
            session.SendReliableToSlot(i, coop::net::ReliableKind::PropToolFlags, &payload, sizeof(payload));
}

}  // namespace coop::prop_tool_flags
