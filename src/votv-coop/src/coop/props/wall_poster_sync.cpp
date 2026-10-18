// coop/props/wall_poster_sync.cpp -- see coop/props/wall_poster_sync.h.

#include "coop/props/wall_poster_sync.h"

#include "coop/media/media_pool.h"        // AutoShare: a custom picture is everyone's
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/prop_synth_key.h"    // RandomKeyString: a Key for a poster the game left without one
#include "coop/text/utf8_codec.h"         // a file name in UTF-8, whatever its letters

#include "ue_wrap/actors/prop.h"          // GetActorSaveKeyString
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"     // StringToFName
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine.h"        // BeginDeferredSpawn, FinishDeferredSpawn, TryGetActorLocation
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

namespace coop::wall_poster_sync {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;

UE_FACT_FN(kRowPut, "wall_poster_sync", L"prop_poster_C", L"put", Declared);
UE_FACT_FN(kRowBeginPlay, "wall_poster_sync", L"poster_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowAction, "wall_poster_sync", L"poster_C", L"actionOptionIndex", Declared);
UE_FACT_FN(kRowPick, "wall_poster_sync", L"poster_C", L"intComs_texturePickerApply", Declared);
constexpr int kTag = 0x57504F53;  // 'WPOS'

constexpr uint8_t kOpPlace = 1, kOpRemove = 2, kOpPicture = 3;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[4] = {}, g_saidLive[4] = {};
void* g_inPut = nullptr;   // the held poster whose put runs now

R::InstanceOffset g_fKey(L"Key"), g_fType(L"type"), g_fIndex(L"index"), g_fFile(L"filename"), g_fCustom(L"Custom");

// Every wall poster this peer knows, by Key; a miss walks the world again.
std::unordered_map<std::wstring, ue_wrap::CachedObjRef> g_byKey;

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

template <class T> bool ReadField(void* obj, R::InstanceOffset& f, T& out) {
    const int32_t off = obj ? f.Of(obj) : -1;
    if (off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(obj) + off, sizeof(T));
    return true;
}
template <class T> bool WriteField(void* obj, R::InstanceOffset& f, const T& v) {
    const int32_t off = obj ? f.Of(obj) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(T));
    return true;
}

std::wstring KeyOf(void* poster) {
    const std::wstring k = ue_wrap::prop::GetActorSaveKeyString(poster);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

std::wstring FileOf(void* poster) {
    R::FName fn{};
    if (!ReadField(poster, g_fFile, fn)) return std::wstring();
    const std::wstring s = R::ToString(fn);
    return s == L"None" ? std::wstring() : s;
}

void WalkPosters() {
    g_byKey.clear();
    const int32_t n = R::NumObjects();
    std::unordered_map<void*, bool> isPoster;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        auto it = isPoster.find(cls);
        if (it == isPoster.end()) it = isPoster.emplace(cls, HasClassInChain(obj, L"poster_C")).first;
        if (!it->second || !R::IsLive(obj) || R::ToString(R::NameOf(obj)).rfind(L"Default__", 0) == 0) continue;
        const std::wstring k = KeyOf(obj);
        if (!k.empty()) g_byKey[k].Set(obj);
    }
}

void* FindPoster(const std::wstring& key) {
    if (key.empty()) return nullptr;
    auto it = g_byKey.find(key);
    if (it != g_byKey.end())
        if (void* a = it->second.Get()) return a;
    WalkPosters();
    it = g_byKey.find(key);
    return it != g_byKey.end() ? it->second.Get() : nullptr;
}

// A class name is ASCII; a file name is any name a player gave a picture, so it travels as UTF-8, cut at a
// whole character.
void CopyStr(const std::wstring& w, char* dst, size_t cap, uint8_t& len) {
    std::string u = coop::text::ToUtf8Exact(w);
    if (u.size() > cap) {
        size_t n = cap;
        while (n > 0 && (static_cast<unsigned char>(u[n]) & 0xC0) == 0x80) --n;
        u.resize(n);
    }
    std::memcpy(dst, u.data(), u.size());
    len = static_cast<uint8_t>(u.size());
}

std::wstring StrOf(const char* src, uint8_t len) {
    std::wstring w;
    if (!coop::text::FromUtf8Strict(src, len, &w)) w.clear();
    return w;
}

bool Fill(void* poster, uint8_t op, coop::net::WallPosterPayload& p) {
    std::memset(&p, 0, sizeof(p));
    p.op = op;
    const std::wstring key = KeyOf(poster);
    if (key.empty()) return false;
    coop::net::WireKeyFromString(key, p.key);
    if (op == kOpRemove) return true;
    const std::wstring file = FileOf(poster);
    CopyStr(file, p.file, sizeof(p.file), p.fileLen);
    uint8_t custom = 0;
    ReadField(poster, g_fCustom, custom);
    p.custom = custom ? 1 : 0;
    if (op == kOpPicture) return true;
    const std::wstring cls = R::ClassNameOf(poster);
    CopyStr(cls, p.cls.data, sizeof(p.cls.data), p.cls.len);
    int32_t type = 0, index = 0;   // the payload is packed: no reference into it
    ReadField(poster, g_fType, type);
    ReadField(poster, g_fIndex, index);
    p.type = type;
    p.index = index;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorLocation(poster, at) || !E::TryGetActorRotation(poster, rot)) return false;
    p.x = at.X; p.y = at.Y; p.z = at.Z;
    p.pitch = rot.Pitch; p.yaw = rot.Yaw; p.roll = rot.Roll;
    return true;
}

void Share(void* poster, bool custom) {
    const std::wstring file = FileOf(poster);
    if (custom && !file.empty()) coop::media_pool::AutoShare(coop::media_pool::Kind::Poster, file);
}

bool CallNoArgs(void* obj, const wchar_t* fnName) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(obj), fnName);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(obj, f);
}

void SetPicture(void* poster, const coop::net::WallPosterPayload& p) {
    const R::FName fn = ue_wrap::fname_utils::StringToFName(StrOf(p.file, p.fileLen));
    WriteField(poster, g_fFile, fn);
    const uint8_t custom = p.custom ? 1 : 0;
    WriteField(poster, g_fCustom, custom);
}

void Apply(const coop::net::WallPosterPayload& p, const char* why) {
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    void* have = FindPoster(key);
    if (p.op == kOpRemove) {
        if (have && CallNoArgs(have, L"K2_DestroyActor")) {
            g_byKey.erase(key);
            UE_LOGI("wall_poster: %s -- '%ls' taken down here", why, key.c_str());
        }
        return;
    }
    if (p.op == kOpPicture) {
        if (!have) return;
        SetPicture(have, p);
        CallNoArgs(have, L"setPoster");
        UE_LOGI("wall_poster: %s -- '%ls' shows '%ls' here", why, key.c_str(), StrOf(p.file, p.fileLen).c_str());
        return;
    }
    if (have) return;   // a joiner's save already had it, or the echo of this peer's own
    const std::wstring clsName = StrOf(p.cls.data, p.cls.len);
    void* cls = R::FindClass(clsName.c_str());
    if (!cls) {
        UE_LOGW("wall_poster: %s -- class '%ls' not loaded here; '%ls' not put up", why, clsName.c_str(), key.c_str());
        return;
    }
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const ue_wrap::FRotator rot{p.pitch, p.yaw, p.roll};
    void* poster = E::BeginDeferredSpawn(cls, at, rot);
    if (!poster) return;
    // Its Key, type, index and picture before its BeginPlay, which looks the picture up.
    WriteField(poster, g_fKey, ue_wrap::fname_utils::StringToFName(key));
    const int32_t type = p.type, index = p.index;
    WriteField(poster, g_fType, type);
    WriteField(poster, g_fIndex, index);
    SetPicture(poster, p);
    if (!E::FinishDeferredSpawn(poster, at, rot)) return;
    g_byKey[key].Set(poster);
    UE_LOGI("wall_poster: %s -- '%ls' put up here (%ls, '%ls')", why, key.c_str(), clsName.c_str(),
            StrOf(p.file, p.fileLen).c_str());
}

void Send(coop::net::Session* s, const coop::net::WallPosterPayload& p) {
    s->SendReliable(coop::net::ReliableKind::WallPoster, &p, sizeof(p));
}

// ---- the watches ----

sg::Verdict OnPutPre(const sg::Call& c) { g_inPut = c.object; return sg::Verdict::Run; }
void OnPutPost(const sg::Call&) { g_inPut = nullptr; }

// A wall poster's birth inside a held poster's put: this player put it up.
void OnBeginPlayPost(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || !g_inPut) return;
    if (KeyOf(c.object).empty()) {
        // A Key now, the one every peer names it by, and the one its save keeps.
        WriteField(c.object, g_fKey, ue_wrap::fname_utils::StringToFName(coop::prop_synth_key::RandomKeyString()));
    }
    coop::net::WallPosterPayload p{};
    if (!Fill(c.object, kOpPlace, p)) {
        UE_LOGW("wall_poster: a poster went up here with no Key or transform -- not sent");
        return;
    }
    g_byKey[KeyOf(c.object)].Set(c.object);
    Share(c.object, p.custom != 0);
    Send(s, p);
    UE_LOGI("wall_poster: sent the poster put up here '%ls'", KeyOf(c.object).c_str());
}

// The poster's use: taking it down hands the prop back and destroys it.
coop::net::WallPosterPayload g_pendingRemove{};
sg::Verdict OnActionPre(const sg::Call& c) {
    g_pendingRemove = {};
    if (Live() && !c.fromOurCode && c.object) Fill(c.object, kOpRemove, g_pendingRemove);
    return sg::Verdict::Run;
}
void OnActionPost(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || g_pendingRemove.op != kOpRemove) return;
    if (R::IsLive(c.object)) return;   // a picture pick or anything that left it up
    g_byKey.erase(coop::net::StringFromWireKey(g_pendingRemove.key));
    Send(s, g_pendingRemove);
    UE_LOGI("wall_poster: sent the poster taken down here '%ls'",
            coop::net::StringFromWireKey(g_pendingRemove.key).c_str());
}

// A new picture picked for a poster on the wall.
void OnPickPost(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object) return;
    coop::net::WallPosterPayload p{};
    if (!Fill(c.object, kOpPicture, p)) return;
    Share(c.object, p.custom != 0);
    Send(s, p);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[4] = {&kRowPut, &kRowBeginPlay, &kRowAction, &kRowPick};
    const sg::PreFn pres[4] = {&OnPutPre, nullptr, &OnActionPre, nullptr};
    const sg::PostFn posts[4] = {&OnPutPost, &OnBeginPlayPost, &OnActionPost, &OnPickPost};
    bool pending = false;
    for (int i = 0; i < 4; ++i) {
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], kTag, pres[i], posts[i]);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (!pending) return;
    sg::ResolvePendingNames();
    for (int i = 0; i < 4; ++i) {
        if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], kTag)) continue;
        g_saidLive[i] = true;
        if (sg::NameWatchLive(*rows[i], kTag))
            UE_LOGI("wall_poster: the %ls.%ls watch is live", rows[i]->Class(), rows[i]->Name());
        else
            UE_LOGE("wall_poster: the %ls.%ls watch settled dead -- a wall poster stays on its own screen",
                    rows[i]->Class(), rows[i]->Name());
    }
}

void OnWallPoster(coop::net::Session& session, const coop::net::WallPosterPayload& payload, uint8_t senderSlot) {
    const bool host = session.role() == coop::net::Role::Host;
    if (!host && senderSlot != 0) return;
    Apply(payload, host ? "a client's" : "the host's");
    if (!host) return;
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (i != senderSlot) session.SendReliableToSlot(i, coop::net::ReliableKind::WallPoster, &payload, sizeof(payload));
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = Live();
    if (!s || s->role() != coop::net::Role::Host) return;
    WalkPosters();
    int sent = 0;
    for (auto& [key, ref] : g_byKey) {
        void* poster = ref.Get();
        coop::net::WallPosterPayload p{};
        if (!poster || !Fill(poster, kOpPlace, p)) continue;
        if (s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::WallPoster, &p, sizeof(p))) ++sent;
    }
    UE_LOGI("wall_poster: HOST sent slot %d %d wall poster(s)", peerSlot, sent);
}

void RefreshPictures() {
    WalkPosters();
    for (auto& [key, ref] : g_byKey)
        if (void* poster = ref.Get()) CallNoArgs(poster, L"setPoster");
}

void OnDisconnect() {
    g_byKey.clear();
    g_inPut = nullptr;
    g_pendingRemove = {};
}

}  // namespace coop::wall_poster_sync
