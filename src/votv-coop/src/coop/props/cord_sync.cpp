// coop/props/cord_sync.cpp -- see coop/props/cord_sync.h.

#include "coop/props/cord_sync.h"

#include "coop/element/portable_identity.h"  // PortableWireKey: the outlet's name on every peer
#include "coop/element/registry.h"           // EidForActor
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_key_index.h"       // Find, DebugCollect: corded items by their key
#include "coop/props/remote_prop.h"          // ResolveLiveActorByEid

#include "ue_wrap/actors/prop.h"             // GetInteractableKeyString
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"           // TryGetActorLocation
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::cord_sync {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;

UE_FACT_FN(kRowConnect, "cord_sync", L"cord_C", L"connect", Declared);
UE_FACT_FN(kRowDisconnect, "cord_sync", L"cord_C", L"disconnect", Declared);
UE_FACT_FN(kRowTick, "cord_sync", L"cord_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowDestroyed, "cord_sync", L"cord_C", L"ReceiveDestroyed", Declared);
constexpr int kTag = 0x434F5244;  // 'CORD'

constexpr float    kNearCm     = 60.f;    // an outlet named otherwise is the one this close to the sender's
constexpr uint64_t kSocketsMs  = 3000;    // the outlet list is walked again after this
constexpr uint64_t kAssertMs   = 20000;   // a joiner holds what the host sent this long while its load settles
constexpr uint64_t kAssertStep = 1000;
constexpr uint64_t kDragMs     = 66;      // a dragged head's pose, at most this often

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[4] = {}, g_saidLive[4] = {};
void* g_inTick = nullptr;       // the cord whose tick runs now
void* g_destroying = nullptr;   // the cord being destroyed now
uint64_t g_sent = 0, g_applied = 0;

R::InstanceOffset g_cordSocket(L"socket");    // cord_C.socket: the outlet it is in, or null
R::InstanceOffset g_cordParent(L"parent");    // cord_C.parent: the item it powers
R::InstanceOffset g_itemCord(L"obj_cord");    // prop_corded_C.obj_cord: the item's cord
R::InstanceOffset g_cordHead(L"head");        // cord_C.head: the plug's component
R::InstanceOffset g_cordDrag(L"drag");        // cord_C.drag: true while this machine's player drags it

// The drag: this machine's player's cords streaming their head, and the cords another player drags here.
struct DragSent { uint64_t at = 0; ue_wrap::FVector loc{}; };
std::map<void*, DragSent> g_localDrag;
std::set<void*> g_remoteDrag;
R::ParamOffset g_pSocket(L"socket"), g_pLoad(L"load");

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

bool IsHost(const coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

bool HasClassInChain(void* obj, const wchar_t* name) {
    if (!obj) return false;
    for (void* cls = R::ClassOf(obj); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == name) return true;
    return false;
}

void* ReadObj(void* obj, R::InstanceOffset& field) {
    if (!obj) return nullptr;
    const int32_t off = field.Of(obj);
    if (off < 0) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, static_cast<const uint8_t*>(obj) + off, sizeof(p));
    return (p && R::IsLive(p)) ? p : nullptr;
}

void* CordOf(void* item) { return HasClassInChain(item, L"prop_corded_C") ? ReadObj(item, g_itemCord) : nullptr; }

std::wstring KeyOf(void* actor) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(actor);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

// ---- outlets ----

// Every live outlet: few, and walked only when a plug arrives, at most once per few seconds.
struct Outlet { void* actor; int32_t idx; std::wstring key; ue_wrap::FVector at; };
std::vector<Outlet> g_outlets;
uint64_t g_outletsAt = 0;

void WalkOutlets(uint64_t now) {
    g_outlets.clear();
    g_outletsAt = now;
    std::unordered_map<void*, bool> isOutletClass;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = isOutletClass.find(cls);
        if (it == isOutletClass.end()) it = isOutletClass.emplace(cls, HasClassInChain(obj, L"cordSocket_C")).first;
        if (!it->second || !R::IsLive(obj)) continue;
        if (R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) == 0) continue;
        Outlet o{obj, R::InternalIndexOf(obj), coop::element::PortableWireKey(obj), {}};
        ue_wrap::engine::TryGetActorLocation(obj, o.at);
        g_outlets.push_back(std::move(o));
    }
}

void* FindOutlet(const std::wstring& key, const ue_wrap::FVector& at, bool rewalked = false) {
    const uint64_t now = NowMs();
    if (g_outlets.empty() || now - g_outletsAt > kSocketsMs) { WalkOutlets(now); rewalked = true; }
    void* nearest = nullptr;
    float best = kNearCm * kNearCm;
    for (const Outlet& o : g_outlets) {
        if (!R::IsLiveByIndex(o.actor, o.idx)) continue;
        if (!key.empty() && o.key == key) return o.actor;
        ue_wrap::FVector p = o.at;
        ue_wrap::engine::TryGetActorLocation(o.actor, p);   // an outlet on a prop moves with it
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best) { best = d2; nearest = o.actor; }
    }
    if (!nearest && !rewalked) { WalkOutlets(now); return FindOutlet(key, at, true); }
    return nearest;
}

// ---- the wire ----

bool Fill(void* item, void* socket, coop::net::CordPlugPayload& p) {
    std::memset(&p, 0, sizeof(p));
    const std::wstring key = KeyOf(item);
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(item);
    if (key.empty() && eid == coop::element::kInvalidId) return false;
    coop::net::WireKeyFromString(key, p.item);
    p.elementId = eid == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(eid);
    if (socket) {
        p.plugged = 1;
        coop::net::WireKeyFromString(coop::element::PortableWireKey(socket), p.socket);
        ue_wrap::FVector at{};
        if (!ue_wrap::engine::TryGetActorLocation(socket, at)) return false;
        p.sx = at.X; p.sy = at.Y; p.sz = at.Z;
    }
    return true;
}

void SendFor(coop::net::Session* s, void* item, void* socket, const char* why) {
    coop::net::CordPlugPayload p{};
    if (!Fill(item, socket, p)) {
        UE_LOGI("cord_sync: %s on %p not sent -- the item has no key or id, or the outlet no location", why, item);
        return;
    }
    s->SendReliable(coop::net::ReliableKind::CordPlug, &p, sizeof(p));
    ++g_sent;
    UE_LOGI("cord_sync: sent %s item='%ls' eid=%u outlet='%ls'", why, KeyOf(item).c_str(), p.elementId,
            coop::net::StringFromWireKey(p.socket).c_str());
}

bool ReadBoolField(void* obj, R::InstanceOffset& field) {
    if (!obj) return false;
    const int32_t off = field.Of(obj);
    return off >= 0 && static_cast<const uint8_t*>(obj)[off] != 0;
}

bool CallCordVerb(void* cord, const wchar_t* verb) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(cord), verb);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(cord, f);
}

// USceneComponent::K2_SetWorldLocation with a teleport: a dragged head goes where the dragger's is.
bool SetHeadPose(void* head, const ue_wrap::FVector& at, const ue_wrap::FRotator& rot) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(head), L"K2_SetWorldLocation");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<ue_wrap::FVector>(L"NewLocation", at)) return false;
    f.Set<bool>(L"bTeleport", true);
    if (!ue_wrap::Call(head, f)) return false;
    return ue_wrap::engine::SetComponentWorldRotation(head, rot);
}

// ---- applying ----

void* ResolveItem(const coop::net::CordPlugPayload& p) {
    if (p.item.len > 0)
        if (void* a = coop::prop_key_index::Find(coop::net::StringFromWireKey(p.item))) return a;
    return p.elementId ? coop::remote_prop::ResolveLiveActorByEid(p.elementId) : nullptr;
}

// Runs the cord's own verb so the outlet powers or unpowers the item as the game does. True when the cord
// is where the payload says once it returns.
bool Apply(const coop::net::CordPlugPayload& p, const char* why) {
    void* item = ResolveItem(p);
    void* cord = CordOf(item);
    if (!cord) {
        UE_LOGI("cord_sync: %s for item '%ls' eid=%u -- no corded item here", why,
                coop::net::StringFromWireKey(p.item).c_str(), p.elementId);
        return false;
    }
    void* current = ReadObj(cord, g_cordSocket);
    if (p.plugged == 2) {
        // Another player drags it: out of the outlet, its head where theirs is. This machine's own drag of
        // the same cord, which a relay echoes back, keeps its own.
        if (ReadBoolField(cord, g_cordDrag)) return true;
        if (current && !CallCordVerb(cord, L"disconnect")) return false;
        void* head = ReadObj(cord, g_cordHead);
        if (!head) return false;
        g_remoteDrag.insert(cord);
        return SetHeadPose(head, ue_wrap::FVector{p.sx, p.sy, p.sz}, ue_wrap::FRotator{p.hp, p.hy, p.hr});
    }
    const bool wasDragged = g_remoteDrag.erase(cord) != 0;
    if (!p.plugged) {
        // A drag that ended in no outlet puts the head back on the item; disconnect does that.
        if (!current && !wasDragged) return true;
        if (!CallCordVerb(cord, L"disconnect")) return false;
        ++g_applied;
        UE_LOGI("cord_sync: %s -- item '%ls' unplugged here", why, coop::net::StringFromWireKey(p.item).c_str());
        return true;
    }
    void* outlet = FindOutlet(coop::net::StringFromWireKey(p.socket), ue_wrap::FVector{p.sx, p.sy, p.sz});
    if (!outlet) {
        UE_LOGW("cord_sync: %s -- no outlet '%ls' nor any within %.0f cm of (%.0f, %.0f, %.0f)", why,
                coop::net::StringFromWireKey(p.socket).c_str(), kNearCm, p.sx, p.sy, p.sz);
        return false;
    }
    if (current == outlet) return true;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(cord), L"connect");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"socket", outlet);
    f.Set<uint8_t>(L"load", 0);
    if (!ue_wrap::Call(cord, f)) return false;
    ++g_applied;
    UE_LOGI("cord_sync: %s -- item '%ls' plugged into '%ls' here", why, coop::net::StringFromWireKey(p.item).c_str(),
            coop::net::StringFromWireKey(p.socket).c_str());
    return ReadObj(cord, g_cordSocket) == outlet;
}

// CLIENT: what the host last said of each item, held while a joiner's load settles -- the game's own
// reconnect from the save runs after a delay and would undo a newer unplug.
struct Asserted { coop::net::CordPlugPayload p; uint64_t until; };
std::map<std::wstring, Asserted> g_assert;
uint64_t g_nextAssert = 0;

std::wstring ItemId(const coop::net::CordPlugPayload& p) {
    return p.item.len ? coop::net::StringFromWireKey(p.item) : L"#" + std::to_wstring(p.elementId);
}

void Forget(void* item) {
    coop::net::CordPlugPayload p{};
    if (Fill(item, nullptr, p)) g_assert.erase(ItemId(p));
}

// ---- the watches ----

void OnConnectPost(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || !c.locals) return;
    g_localDrag.erase(c.object);   // a drag that ends in an outlet ends with its plug
    const int32_t lo = g_pLoad.Of(c.function), so = g_pSocket.Of(c.function);
    if (lo < 0 || so < 0 || c.locals[lo] != 0) return;   // the load-time reconnect is every peer's own
    void* socket = nullptr;
    std::memcpy(&socket, c.locals + so, sizeof(socket));
    void* item = ReadObj(c.object, g_cordParent);
    if (!socket || !item || ReadObj(c.object, g_cordSocket) != socket) return;
    Forget(item);
    SendFor(s, item, socket, "plug");
}

sg::Verdict OnDisconnectPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || c.object == g_destroying) return sg::Verdict::Run;
    void* item = ReadObj(c.object, g_cordParent);
    if (!ReadObj(c.object, g_cordSocket)) {
        // Not in an outlet. A drag of this player's that ends here, in no outlet, puts the head back on the
        // item on every screen.
        if (g_localDrag.erase(c.object) && item) SendFor(s, item, nullptr, "drag ended");
        return sg::Verdict::Run;
    }
    if (!item) return sg::Verdict::Run;
    if (c.object == g_inTick) {
        // Stretched too far: the host's copy decides, so a client's copy that strayed keeps the plug.
        if (!IsHost(s)) return sg::Verdict::Cancel;
        SendFor(s, item, nullptr, "stretched out");
        return sg::Verdict::Run;
    }
    Forget(item);
    SendFor(s, item, nullptr, "unplug");
    return sg::Verdict::Run;
}

sg::Verdict OnTickPre(const sg::Call& c) { g_inTick = c.object; return sg::Verdict::Run; }

// After the cord's tick: one this machine's player drags has its head where the tick put it, which is
// streamed to every other screen.
void OnTickPost(const sg::Call& c) {
    g_inTick = nullptr;
    auto* s = Live();
    if (!s || !c.object || !ReadBoolField(c.object, g_cordDrag)) return;
    void* item = ReadObj(c.object, g_cordParent);
    void* head = ReadObj(c.object, g_cordHead);
    if (!item || !head) return;
    const uint64_t now = NowMs();
    DragSent& d = g_localDrag[c.object];
    const ue_wrap::FVector at = ue_wrap::engine::GetComponentLocation(head);
    const float dx = at.X - d.loc.X, dy = at.Y - d.loc.Y, dz = at.Z - d.loc.Z;
    if (now - d.at < kDragMs || (d.at && dx * dx + dy * dy + dz * dz < 1.f)) return;
    coop::net::CordPlugPayload p{};
    if (!Fill(item, nullptr, p)) return;
    const ue_wrap::FRotator rot = ue_wrap::engine::GetComponentWorldRotation(head);
    p.plugged = 2;
    p.sx = at.X; p.sy = at.Y; p.sz = at.Z;
    p.hp = rot.Pitch; p.hy = rot.Yaw; p.hr = rot.Roll;
    s->SendReliable(coop::net::ReliableKind::CordPlug, &p, sizeof(p));
    d.at = now;
    d.loc = at;
}
sg::Verdict OnDestroyedPre(const sg::Call& c) { g_destroying = c.object; return sg::Verdict::Run; }
void OnDestroyedPost(const sg::Call&) { g_destroying = nullptr; }

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[4] = {&kRowConnect, &kRowDisconnect, &kRowTick, &kRowDestroyed};
    const sg::PreFn pres[4] = {nullptr, &OnDisconnectPre, &OnTickPre, &OnDestroyedPre};
    const sg::PostFn posts[4] = {&OnConnectPost, nullptr, &OnTickPost, &OnDestroyedPost};
    bool pending = false;
    for (int i = 0; i < 4; ++i) {
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], kTag, pres[i], posts[i]);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (pending) {
        sg::ResolvePendingNames();
        for (int i = 0; i < 4; ++i) {
            if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], kTag)) continue;
            g_saidLive[i] = true;
            if (sg::NameWatchLive(*rows[i], kTag))
                UE_LOGI("cord_sync: the %ls.%ls watch is live", rows[i]->Class(), rows[i]->Name());
            else
                UE_LOGE("cord_sync: the %ls.%ls watch settled dead -- a cord plugs on its own screen alone",
                        rows[i]->Class(), rows[i]->Name());
        }
    }
    if (g_assert.empty()) return;
    const uint64_t now = NowMs();
    if (now < g_nextAssert || !Live()) return;
    g_nextAssert = now + kAssertStep;
    for (auto it = g_assert.begin(); it != g_assert.end();) {
        if (now > it->second.until) { it = g_assert.erase(it); continue; }
        Apply(it->second.p, "held");
        ++it;
    }
}

void OnCordPlug(coop::net::Session& session, const coop::net::CordPlugPayload& payload, uint8_t senderSlot) {
    const bool host = session.role() == coop::net::Role::Host;
    if (!host && senderSlot != 0) return;   // a client hears the host alone
    Apply(payload, host ? "a client's" : "the host's");
    if (host) {
        session.SendReliable(coop::net::ReliableKind::CordPlug, &payload, sizeof(payload));   // to everyone
        return;
    }
    // Held a while: a newer word from the host replaces it, and this player's own plug drops it. A dragged
    // head is the moment's alone: it ends what was held.
    if (payload.plugged == 2) g_assert.erase(ItemId(payload));
    else g_assert[ItemId(payload)] = Asserted{payload, NowMs() + kAssertMs};
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = Live();
    if (!s || !IsHost(s)) return;
    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    int sent = 0;
    for (const auto& e : entries) {
        if (!e.actor || !R::IsLive(e.actor)) continue;
        void* cord = CordOf(e.actor);
        if (!cord) continue;
        coop::net::CordPlugPayload p{};
        if (!Fill(e.actor, ReadObj(cord, g_cordSocket), p)) continue;
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::CordPlug, &p, sizeof(p))) ++sent;
    }
    UE_LOGI("cord_sync: HOST sent slot %d the cords of %d corded item(s)", peerSlot, sent);
}

void OnDisconnect() {
    if (g_sent || g_applied)
        UE_LOGI("cord_sync: session end -- %llu plug(s) sent, %llu applied", static_cast<unsigned long long>(g_sent),
                static_cast<unsigned long long>(g_applied));
    g_sent = g_applied = 0;
    g_assert.clear();
    g_localDrag.clear();
    g_remoteDrag.clear();
    g_outlets.clear();
    g_outletsAt = 0;
    g_inTick = g_destroying = nullptr;
}

}  // namespace coop::cord_sync
