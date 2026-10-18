// coop/props/part_index.cpp -- see coop/props/part_index.h.

#include "coop/props/part_index.h"

#include "coop/element/portable_identity.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"  // ResolveLiveActorByEid

#include "ue_wrap/actors/prop.h"          // IsDescendantOfProp
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"        // IsChildActor

#include <windows.h>

#include <cstdint>
#include <unordered_map>

namespace coop::part_index {
namespace {

namespace R = ue_wrap::reflection;

constexpr uint64_t kRewalkMs = 2000;

std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_parts;
uint64_t g_walkedAt = 0;
bool g_walked = false;

void Walk(uint64_t now) {
    g_parts.clear();
    g_walkedAt = now;
    g_walked = true;
    std::unordered_map<void*, bool> isPropClass;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = isPropClass.find(cls);
        if (it == isPropClass.end()) it = isPropClass.emplace(cls, ue_wrap::prop::IsDescendantOfProp(obj)).first;
        if (!it->second || !R::IsLive(obj) || !ue_wrap::engine::IsChildActor(obj)) continue;
        if (R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) == 0) continue;
        std::wstring key = coop::element::PortableWireKey(obj);
        if (key.empty()) continue;
        g_parts[std::move(key)].Set(obj);
    }
    UE_LOGI("part_index: %zu part(s) indexed by their portable key", g_parts.size());
}

}  // namespace

bool IsPartKey(const std::wstring& wireKey) { return wireKey.rfind(L"mv_", 0) == 0; }

void* Find(const std::wstring& wireKey) {
    if (!IsPartKey(wireKey)) return nullptr;
    auto it = g_parts.find(wireKey);
    if (it != g_parts.end())
        if (void* p = it->second.Get()) return p;
    const uint64_t now = static_cast<uint64_t>(::GetTickCount64());
    if (g_walked && now - g_walkedAt < kRewalkMs) return nullptr;
    Walk(now);
    it = g_parts.find(wireKey);
    return it != g_parts.end() ? it->second.Get() : nullptr;
}

std::wstring WireKeyFor(void* actor) {
    if (!actor) return std::wstring();
    if (ue_wrap::engine::IsChildActor(actor)) return coop::element::PortableWireKey(actor);
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(actor);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

void* ResolveWire(const std::wstring& wireKey, uint32_t eid) {
    if (!wireKey.empty() && wireKey != L"None") {
        void* a = IsPartKey(wireKey) ? Find(wireKey) : coop::prop_key_index::Find(wireKey);
        if (a) return a;
    }
    return eid ? coop::remote_prop::ResolveLiveActorByEid(eid) : nullptr;
}

void OnDisconnect() {
    g_parts.clear();
    g_walked = false;
    g_walkedAt = 0;
}

}  // namespace coop::part_index
