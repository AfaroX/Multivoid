// coop/media/media_playback.cpp -- see coop/media/media_playback.h.

#include "coop/media/media_playback.h"

#include "coop/media/media_pool.h"     // AutoShare, HaveFile: a played file is everyone's
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"     // Find, DebugCollect: devices by their key
#include "coop/text/utf8_codec.h"

#include "ue_wrap/actors/prop.h"            // GetInteractableKeyString
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/media_device.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/custom_assets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace coop::media_playback {
namespace {

namespace sg = ue_wrap::script_gate;
namespace MD = ue_wrap::media_device;

UE_FACT_FN(kRowTvOpen, "media_playback", L"prop_tv2_C", L"openLink", Declared);
UE_FACT_FN(kRowRadioOpen, "media_playback", L"prop_radio_internet_C", L"openLink", Declared);
UE_FACT_FN(kRowTvFin, "media_playback", L"prop_tv2_C", L"fin", Declared);
UE_FACT_FN(kRowRadioFin, "media_playback", L"prop_radio_internet_C", L"fin", Declared);
constexpr int kTagOpen = 0x4D504F50, kTagFin = 0x4D504649;  // 'MPOP' 'MPFI'

constexpr uint8_t kOpState = 0, kOpOpen = 1, kOpPlay = 2, kOpSeek = 3;
constexpr uint64_t kPollMs = 500, kBeatMs = 2000, kScanMs = 5000;
constexpr int64_t  kDriftMs = 2000;
constexpr uint64_t kSettleMs = 1500;     // after an open, before the player's state means anything
constexpr uint64_t kUserHoldMs = 2500;   // after a client's own press, before the host's state is enforced
constexpr size_t   kMaxDevices = 64;
constexpr int64_t  kJumpMs = 1500;      // a position this far from where play would have put it was moved
constexpr uint64_t kPendingMs = 10 * 60 * 1000;   // how long a play waits for its file to arrive
const std::wstring kAssetsTag = L"@assets:";

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

// What a device plays, in the wire's form: a local file is named relative to the asset folder.
struct State {
    std::wstring name, link;
    bool isUrl = false, playing = false;
    uint32_t openSeq = 0;
    int64_t timeMs = 0;
};

// HOST: a device it tracks.
struct HostDev {
    State st;
    bool dirty = true;
    uint64_t sentMs = 0;
    std::wstring rawUrl;    // the player's own url, to catch a change openLink did not make
    int64_t lastT = 0;
    uint64_t lastAt = 0;
};

// HOST: a play a client asked for whose file has not reached the host yet.
struct PendingOpen { std::wstring key, name, link; bool isUrl = false, pause = false; uint64_t atMs = 0; };

// CLIENT: what the host says, and what this peer last did about it.
struct ClientDev {
    State want;
    uint64_t recvMs = 0;
    uint32_t appliedSeq = 0;
    uint64_t openedMs = 0, commandMs = 0, userMs = 0;
    bool lastPlaying = false, haveLast = false;
    int64_t lastT = 0;
    uint64_t lastAt = 0;
};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1, g_openSeq = 0;
bool g_watched[4] = {}, g_saidLive[4] = {};
std::map<std::wstring, HostDev> g_host;
std::map<std::wstring, ClientDev> g_client;
std::vector<PendingOpen> g_pending;
uint64_t g_nextPoll = 0, g_nextScan = 0, g_nextSweep = 0;
std::wstring g_folderNorm;   // the asset folder, normalised, for the wire form

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

std::wstring Norm(std::wstring p) {
    for (auto& c : p) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(std::towlower(c)); }
    return p;
}

const std::wstring& FolderNorm() {
    if (g_folderNorm.empty()) {
        const std::wstring f = ue_wrap::custom_assets::AssetFolder();
        if (!f.empty()) g_folderNorm = Norm(f);
    }
    return g_folderNorm;
}

std::wstring ToWire(const std::wstring& link, bool isUrl) {
    if (isUrl) return link;
    const std::wstring& f = FolderNorm();
    const std::wstring n = Norm(link);
    if (!f.empty() && n.size() > f.size() && n.compare(0, f.size(), f) == 0)
        return kAssetsTag + link.substr(f.size());
    return link;
}

std::wstring FromWire(const std::wstring& link) {
    if (link.compare(0, kAssetsTag.size(), kAssetsTag) != 0) return link;
    const std::wstring f = ue_wrap::custom_assets::AssetFolder();
    return f + link.substr(kAssetsTag.size());
}

// A wire link that names a file in the asset folder, as the pool's kind and name.
bool AssetFile(const std::wstring& wireLink, coop::media_pool::Kind& kind, std::wstring& name) {
    if (wireLink.compare(0, kAssetsTag.size(), kAssetsTag) != 0) return false;
    return coop::media_pool::FromAssetRelative(wireLink.substr(kAssetsTag.size()), kind, name);
}

// Whether this peer can open the link now: a URL always, an asset file once it is here.
bool Openable(const std::wstring& wireLink) {
    coop::media_pool::Kind kind;
    std::wstring name;
    return !AssetFile(wireLink, kind, name) || coop::media_pool::HaveFile(kind, name);
}

void ShareIfAsset(const std::wstring& wireLink) {
    coop::media_pool::Kind kind;
    std::wstring name;
    if (AssetFile(wireLink, kind, name)) coop::media_pool::AutoShare(kind, name);
}

std::wstring KeyOf(void* actor) {
    const std::wstring k = ue_wrap::prop::GetInteractableKeyString(actor);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

// ---- wire ----

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutStr(std::vector<uint8_t>& b, const std::wstring& w) {
    std::string u = coop::text::ToUtf8Exact(w);
    if (u.size() > 2048) u.resize(2048);
    b.push_back(static_cast<uint8_t>(u.size() & 0xFF));
    b.push_back(static_cast<uint8_t>(u.size() >> 8));
    b.insert(b.end(), u.begin(), u.end());
}

struct Reader {
    const uint8_t* p; size_t n, o = 0; bool ok = true;
    bool Has(size_t k) { if (o + k > n) ok = false; return ok; }
    uint8_t U8() { if (!Has(1)) return 0; return p[o++]; }
    uint32_t U32() { if (!Has(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[o + i]) << (8 * i); o += 4; return v; }
    uint64_t U64() { if (!Has(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[o + i]) << (8 * i); o += 8; return v; }
    std::wstring Str() {
        if (!Has(2)) return std::wstring();
        const size_t len = static_cast<size_t>(p[o] | (p[o + 1] << 8));
        o += 2;
        if (len > 2048 || !Has(len)) { ok = false; return std::wstring(); }
        std::wstring w;
        if (!coop::text::FromUtf8Strict(reinterpret_cast<const char*>(p + o), len, &w)) ok = false;
        o += len;
        return w;
    }
};

bool Send(coop::net::Session* s, int slot, const std::vector<uint8_t>& b) {
    return slot < 0 ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::MediaPlay, g_seq++, b)
                    : coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::MediaPlay, g_seq++, b);
}

void PutState(std::vector<uint8_t>& b, const std::wstring& key, const State& st) {
    PutStr(b, key);
    PutStr(b, st.name);
    PutStr(b, st.link);
    PutU8(b, st.isUrl ? 1 : 0);
    PutU8(b, st.playing ? 1 : 0);
    PutU32(b, st.openSeq);
    PutU64(b, static_cast<uint64_t>(st.timeMs < 0 ? 0 : st.timeMs));
}

// HOST: the states of `keys` (all when empty) to a slot, or every peer.
void SendStates(coop::net::Session* s, int slot, const std::vector<std::wstring>& keys) {
    std::vector<uint8_t> b;
    PutU8(b, kOpState);
    std::vector<const std::pair<const std::wstring, HostDev>*> rows;
    for (const auto& kv : g_host)
        if (keys.empty() || std::find(keys.begin(), keys.end(), kv.first) != keys.end()) rows.push_back(&kv);
    if (rows.empty()) return;
    PutU8(b, static_cast<uint8_t>(rows.size() > 255 ? 255 : rows.size()));
    size_t n = 0;
    for (const auto* kv : rows) {
        if (++n > 255) break;
        PutState(b, kv->first, kv->second.st);
    }
    if (b.size() > coop::blob_chunks::MaxBlobBytes()) return;
    Send(s, slot, b);
}

// ---- the host's own devices ----

void Track(void* actor, const std::wstring& name, const std::wstring& link, bool isUrl, bool pause) {
    const std::wstring key = KeyOf(actor);
    if (key.empty()) return;
    if (!g_host.count(key) && g_host.size() >= kMaxDevices) return;
    HostDev& d = g_host[key];
    d.st.name = name;
    d.st.link = ToWire(link, isUrl);
    d.st.isUrl = isUrl;
    d.st.playing = !pause;
    d.st.openSeq = ++g_openSeq;
    d.st.timeMs = 0;
    d.dirty = true;
    d.rawUrl.clear();
    d.lastT = 0;
    d.lastAt = NowMs();
    ShareIfAsset(d.st.link);
    UE_LOGI("media_playback: HOST %ls '%ls' plays '%ls'", MD::KindOf(actor) == MD::Kind::Tv ? L"TV" : L"radio",
            key.c_str(), name.c_str());
}

void HostPending(uint64_t now) {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (now - it->atMs > kPendingMs) { it = g_pending.erase(it); continue; }
        void* actor = coop::prop_key_index::Find(it->key);
        if (!actor || !Openable(it->link)) { ++it; continue; }
        const std::wstring local = FromWire(it->link);
        if (MD::OpenLink(actor, it->name, local, it->isUrl, it->pause)) {
            Track(actor, it->name, local, it->isUrl, it->pause);
            UE_LOGI("media_playback: HOST played '%ls' on '%ls' now that the file is here", it->name.c_str(), it->key.c_str());
        }
        it = g_pending.erase(it);
    }
}

void HostPoll(coop::net::Session* s, uint64_t now) {
    std::vector<std::wstring> send;
    for (auto& [key, d] : g_host) {
        void* actor = coop::prop_key_index::Find(key);
        void* player = actor ? MD::PlayerOf(actor) : nullptr;
        if (!player) continue;
        bool playing = false;
        int64_t t = 0;
        MD::IsPlaying(player, playing);
        MD::TimeMs(player, t);
        // A link the device opened another way than openLink (a channel the remote changed, a
        // playlist step) is still the device's: its url, in the wire's form, names it.
        std::wstring url;
        if (MD::Url(player, url) && !url.empty() && url != d.rawUrl) {
            if (!d.rawUrl.empty()) {
                const bool isUrl = url.find(L"://") != std::wstring::npos && url.compare(0, 7, L"file://") != 0;
                std::wstring path = url.compare(0, 7, L"file://") == 0 ? url.substr(7) : url;
                const size_t slash = path.find_last_of(L"\\/");
                d.st.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
                d.st.link = ToWire(path, isUrl);
                d.st.isUrl = isUrl;
                d.st.openSeq = ++g_openSeq;
                d.dirty = true;
            }
            d.rawUrl = url;
        }
        if (playing != d.st.playing) { d.st.playing = playing; d.dirty = true; }
        // A scrub: the position moved further than play would have moved it.
        if (d.lastAt) {
            const int64_t expect = d.lastT + (playing ? static_cast<int64_t>(now - d.lastAt) : 0);
            if ((t > expect ? t - expect : expect - t) > kJumpMs) d.dirty = true;
        }
        d.lastT = t;
        d.lastAt = now;
        d.st.timeMs = t;
        if (d.dirty || (playing && now - d.sentMs >= kBeatMs)) {
            d.dirty = false;
            d.sentMs = now;
            send.push_back(key);
        }
    }
    if (!send.empty()) SendStates(s, -1, send);
}

// HOST: a device that plays without the host having seen it open (the host's world was playing it
// before the session began) is tracked from its player's url.
void HostScan() {
    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    for (const auto& e : entries) {
        if (!e.actor || e.key.empty() || g_host.count(e.key) || MD::KindOf(e.actor) == MD::Kind::None) continue;
        void* player = MD::PlayerOf(e.actor);
        bool playing = false;
        std::wstring url;
        if (!player || !MD::IsPlaying(player, playing) || !playing || !MD::Url(player, url) || url.empty()) continue;
        const bool isUrl = url.find(L"://") != std::wstring::npos && url.compare(0, 7, L"file://") != 0;
        const std::wstring path = url.compare(0, 7, L"file://") == 0 ? url.substr(7) : url;
        const size_t slash = path.find_last_of(L"\\/");
        Track(e.actor, slash == std::wstring::npos ? path : path.substr(slash + 1), path, isUrl, false);
        g_host[e.key].rawUrl = url;
    }
}

// ---- a client's devices ----

void ClientApply(uint64_t now) {
    for (auto& [key, d] : g_client) {
        void* actor = coop::prop_key_index::Find(key);
        if (!actor || MD::KindOf(actor) == MD::Kind::None) continue;
        if (d.appliedSeq != d.want.openSeq) {
            if (!Openable(d.want.link)) continue;   // the pool is still bringing the file
            if (MD::OpenLink(actor, d.want.name, FromWire(d.want.link), d.want.isUrl, !d.want.playing)) {
                d.appliedSeq = d.want.openSeq;
                d.openedMs = now;
                d.commandMs = now;
                d.haveLast = false;
                d.lastAt = 0;   // a new link's position is not the old one's: no scrub to see yet
            }
            continue;
        }
        void* player = MD::PlayerOf(actor);
        if (!player || now - d.openedMs < kSettleMs) continue;
        bool preparing = false;
        if (MD::IsPreparing(player, preparing) && preparing) continue;
        bool playing = false;
        int64_t t = 0;
        if (!MD::IsPlaying(player, playing) || !MD::TimeMs(player, t)) continue;
        // A change this peer did not command is its player's own press: the host is told, and its
        // answer is waited for rather than fought.
        if (d.haveLast && playing != d.lastPlaying && now - d.commandMs > 1000 && playing != d.want.playing) {
            d.userMs = now;
            if (auto* s = Live()) {
                std::vector<uint8_t> b;
                PutU8(b, kOpPlay);
                PutStr(b, key);
                PutU8(b, playing ? 1 : 0);
                PutU64(b, static_cast<uint64_t>(t < 0 ? 0 : t));
                Send(s, 0, b);
            }
        }
        // A scrub this peer did not command is its player's own: the host moves its copy there.
        if (d.lastAt && now - d.commandMs > 1000) {
            const int64_t moved = d.lastT + (playing ? static_cast<int64_t>(now - d.lastAt) : 0);
            if ((t > moved ? t - moved : moved - t) > kJumpMs) {
                d.userMs = now;
                d.want.timeMs = t;
                d.recvMs = now;
                if (auto* s = Live()) {
                    std::vector<uint8_t> b;
                    PutU8(b, kOpSeek);
                    PutStr(b, key);
                    PutU64(b, static_cast<uint64_t>(t < 0 ? 0 : t));
                    Send(s, 0, b);
                }
            }
        }
        d.lastT = t;
        d.lastAt = now;
        d.lastPlaying = playing;
        d.haveLast = true;
        if (now - d.userMs < kUserHoldMs) continue;
        if (d.want.playing && !playing) { MD::Play(player); d.commandMs = now; continue; }
        if (!d.want.playing && playing) { MD::Pause(player); d.commandMs = now; continue; }
        const int64_t expect = d.want.timeMs + (d.want.playing ? static_cast<int64_t>(now - d.recvMs) : 0);
        if ((t > expect ? t - expect : expect - t) > kDriftMs) {
            MD::Seek(player, expect);
            d.commandMs = now;
            d.lastT = expect;
            d.lastAt = now;
        }
    }
}

// ---- the watches ----

sg::Verdict OnOpenPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || MD::KindOf(c.object) == MD::Kind::None) return sg::Verdict::Run;
    std::wstring name, link;
    bool isUrl = false, pause = false;
    if (!MD::ReadOpenLinkArgs(c.function, c.locals, name, link, isUrl, pause)) return sg::Verdict::Run;
    if (s->role() == coop::net::Role::Host) {
        Track(c.object, name, link, isUrl, pause);
        return sg::Verdict::Run;
    }
    const std::wstring key = KeyOf(c.object);
    if (key.empty()) return sg::Verdict::Run;   // a device the host cannot name: this peer's alone
    const std::wstring wire = ToWire(link, isUrl);
    ShareIfAsset(wire);   // the host and the others fetch it, and play it once it is there
    std::vector<uint8_t> b;
    PutU8(b, kOpOpen);
    PutStr(b, key);
    PutStr(b, name);
    PutStr(b, wire);
    PutU8(b, isUrl ? 1 : 0);
    PutU8(b, pause ? 1 : 0);
    Send(s, 0, b);
    UE_LOGI("media_playback: CLIENT asked the host to play '%ls' on '%ls'", name.c_str(), key.c_str());
    return sg::Verdict::Cancel;
}

// CLIENT: what plays next is the host's roll.
sg::Verdict OnFinPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || s->role() != coop::net::Role::Client || c.fromOurCode || !c.object ||
        MD::KindOf(c.object) == MD::Kind::None || KeyOf(c.object).empty())
        return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[4] = {&kRowTvOpen, &kRowRadioOpen, &kRowTvFin, &kRowRadioFin};
    bool pending = false;
    for (int i = 0; i < 4; ++i) {
        const int tag = i < 2 ? kTagOpen : kTagFin;
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], tag, i < 2 ? &OnOpenPre : &OnFinPre, nullptr);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (pending) {
        sg::ResolvePendingNames();
        for (int i = 0; i < 4; ++i) {
            const int tag = i < 2 ? kTagOpen : kTagFin;
            if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], tag)) continue;
            g_saidLive[i] = true;
            if (sg::NameWatchLive(*rows[i], tag))
                UE_LOGI("media_playback: the %ls.%ls watch is live", rows[i]->Class(), rows[i]->Name());
            else
                UE_LOGE("media_playback: the %ls.%ls watch settled dead -- that device plays on each screen alone",
                        rows[i]->Class(), rows[i]->Name());
        }
    }
    auto* s = Live();
    if (!s) return;
    const uint64_t now = NowMs();
    if (now >= g_nextSweep) {
        g_nextSweep = now + 1000;
        g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(10));
    }
    if (now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;
    if (s->role() == coop::net::Role::Host) {
        if (now >= g_nextScan) { g_nextScan = now + kScanMs; HostScan(); }
        HostPending(now);
        HostPoll(s, now);
    } else {
        ClientApply(now);
    }
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = Live();
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob) || blob.empty()) return;
    Reader r{blob.data(), blob.size()};
    const uint8_t op = r.U8();
    const bool host = s->role() == coop::net::Role::Host;
    const uint64_t now = NowMs();
    if (op == kOpState) {
        if (host || senderSlot != 0) return;
        const uint8_t n = r.U8();
        for (uint8_t i = 0; i < n && r.ok; ++i) {
            const std::wstring key = r.Str();
            State st;
            st.name = r.Str();
            st.link = r.Str();
            st.isUrl = r.U8() != 0;
            st.playing = r.U8() != 0;
            st.openSeq = r.U32();
            st.timeMs = static_cast<int64_t>(r.U64());
            if (!r.ok || key.empty()) break;
            if (!g_client.count(key) && g_client.size() >= kMaxDevices) continue;
            ClientDev& d = g_client[key];
            d.want = std::move(st);
            d.recvMs = now;
        }
        return;
    }
    if (!host) return;
    const std::wstring key = r.Str();
    void* actor = key.empty() ? nullptr : coop::prop_key_index::Find(key);
    if (!r.ok || !actor || MD::KindOf(actor) == MD::Kind::None) return;
    if (op == kOpOpen) {
        const std::wstring name = r.Str(), link = r.Str();
        const bool isUrl = r.U8() != 0, pause = r.U8() != 0;
        if (!r.ok) return;
        if (!Openable(link)) {
            g_pending.erase(std::remove_if(g_pending.begin(), g_pending.end(),
                                           [&](const PendingOpen& p) { return p.key == key; }),
                            g_pending.end());
            g_pending.push_back(PendingOpen{key, name, link, isUrl, pause, now});
            UE_LOGI("media_playback: HOST holds slot %u's '%ls' on '%ls' until the file arrives", senderSlot,
                    name.c_str(), key.c_str());
            return;
        }
        const std::wstring local = FromWire(link);
        // Ours to run: the watch tracks it (fromOurCode), so the open is tracked here instead.
        if (MD::OpenLink(actor, name, local, isUrl, pause)) {
            Track(actor, name, local, isUrl, pause);
            UE_LOGI("media_playback: HOST played slot %u's '%ls' on '%ls'", senderSlot, name.c_str(), key.c_str());
        }
        return;
    }
    if (op == kOpSeek) {
        const int64_t t = static_cast<int64_t>(r.U64());
        if (!r.ok) return;
        if (void* player = MD::PlayerOf(actor)) MD::Seek(player, t);
        auto it = g_host.find(key);
        if (it != g_host.end()) {
            it->second.st.timeMs = t;
            it->second.lastT = t;
            it->second.lastAt = now;
            it->second.dirty = true;
        }
        return;
    }
    if (op == kOpPlay) {
        const bool playing = r.U8() != 0;
        const int64_t t = static_cast<int64_t>(r.U64());
        if (!r.ok) return;
        void* player = MD::PlayerOf(actor);
        if (!player) return;
        if (playing) {
            MD::Seek(player, t);
            MD::Play(player);
        } else {
            MD::Pause(player);
        }
        auto it = g_host.find(key);
        if (it != g_host.end()) { it->second.st.playing = playing; it->second.dirty = true; }
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = Live();
    if (s && s->role() == coop::net::Role::Host && !g_host.empty()) SendStates(s, peerSlot, {});
}

void OnPeerGone(uint8_t slot) {
    g_asm.ClearSlot(slot);
}

void OnDisconnect() {
    g_asm.Clear();
    g_host.clear();
    g_client.clear();
    g_pending.clear();
    g_folderNorm.clear();
    g_nextPoll = g_nextScan = 0;
}

}  // namespace coop::media_playback
