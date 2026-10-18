// coop/media/media_playback.cpp -- see coop/media/media_playback.h.

#include "coop/media/media_playback.h"

#include "coop/comms/chat_feed.h"          // Push: a device in another player's hands says so
#include "coop/media/media_pool.h"         // AutoShare, HaveFile: a played file is everyone's
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // LocalPeerId: this peer's slot
#include "coop/player/roster_ledger.h"     // DisplayName
#include "coop/props/prop_key_index.h"     // Find, DebugCollect: devices by their key
#include "coop/text/utf8_codec.h"

#include "ue_wrap/actors/prop.h"            // GetInteractableKeyString
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/media_device.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/custom_assets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::media_playback {
namespace {

namespace sg = ue_wrap::script_gate;
namespace MD = ue_wrap::media_device;
namespace R  = ue_wrap::reflection;

UE_FACT_FN(kRowTvOpen, "media_playback", L"prop_tv2_C", L"openLink", Declared);
UE_FACT_FN(kRowRadioOpen, "media_playback", L"prop_radio_internet_C", L"openLink", Declared);
UE_FACT_FN(kRowTvFin, "media_playback", L"prop_tv2_C", L"fin", Declared);
UE_FACT_FN(kRowRadioFin, "media_playback", L"prop_radio_internet_C", L"fin", Declared);
UE_FACT_FN(kRowTvUiOpen, "media_playback", L"ui_tvInterface_C", L"open", Declared);
UE_FACT_FN(kRowTvUiQuit, "media_playback", L"ui_tvInterface_C", L"quit", Declared);
UE_FACT_FN(kRowRadioUiOpen, "media_playback", L"ui_radioInterface_C", L"open", Declared);
UE_FACT_FN(kRowRadioUiQuit, "media_playback", L"ui_radioInterface_C", L"quit", Declared);
constexpr int kTagOpen = 0x4D504F50, kTagFin = 0x4D504649, kTagUiOpen = 0x4D50554F, kTagUiQuit = 0x4D505551;

constexpr uint8_t kOpState = 0, kOpClaim = 1, kOpRelease = 2;
constexpr uint8_t kNoSlot = 0xFF;
constexpr uint64_t kPollMs = 500, kBeatMs = 2000, kScanMs = 5000;
constexpr int64_t  kDriftMs = 2000;
constexpr int64_t  kJumpMs = 1500;      // a position this far from where play would have put it was moved
constexpr uint64_t kSettleMs = 1500;    // after an open, before the player's state means anything
constexpr uint64_t kLeaseMs = 8000;     // HOST: a holder that says nothing for this long has gone
constexpr size_t   kMaxDevices = 64;
const std::wstring kAssetsTag = L"@assets:";

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

// What a device plays, in the wire's form: a local file is named relative to the asset folder.
struct State {
    std::wstring name, link;
    bool isUrl = false, playing = false;
    uint32_t openSeq = 0;
    int64_t timeMs = 0;
    float volume = -1.f, brightness = -1.f;   // negative: not known
};

struct Dev {
    State st;
    bool have = false;            // a state is set or known
    uint8_t owner = 0;            // the slot whose copy plays: the host until someone uses it
    uint8_t holder = kNoSlot;     // the slot with the device's interface open
    uint64_t holderSeenMs = 0;    // HOST: the holder's last word
    // The owner's side.
    bool dirty = false;
    uint64_t sentMs = 0;
    std::wstring rawUrl;          // the player's own url, to catch a change openLink did not make
    int64_t lastT = 0;
    uint64_t lastAt = 0;
    // A follower's side.
    uint64_t recvMs = 0;
    uint32_t appliedSeq = 0;
    uint64_t openedMs = 0, commandMs = 0;
};

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1, g_maxSeq = 0;
bool g_watched[8] = {}, g_saidLive[8] = {};
std::map<std::wstring, Dev> g_dev;
std::set<std::wstring> g_myOpen;   // devices whose interface this peer has open
uint64_t g_nextPoll = 0, g_nextScan = 0, g_nextSweep = 0;
std::wstring g_folderNorm;         // the asset folder, normalised, for the wire form

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

bool IsHost(coop::net::Session* s) { return s && s->role() == coop::net::Role::Host; }
uint8_t Me(coop::net::Session* s) { return IsHost(s) ? 0 : coop::players::Registry::Get().LocalPeerId(); }

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
    return ue_wrap::custom_assets::AssetFolder() + link.substr(kAssetsTag.size());
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

Dev* DevFor(const std::wstring& key) {
    auto it = g_dev.find(key);
    if (it != g_dev.end()) return &it->second;
    if (key.empty() || g_dev.size() >= kMaxDevices) return nullptr;
    return &g_dev[key];
}

// ---- wire ----

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutF32(std::vector<uint8_t>& b, float f) { uint32_t v; std::memcpy(&v, &f, 4); PutU32(b, v); }
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
    float F32() { const uint32_t v = U32(); float f; std::memcpy(&f, &v, 4); return std::isfinite(f) ? f : -1.f; }
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

void PutState(std::vector<uint8_t>& b, const std::wstring& key, const Dev& d) {
    PutStr(b, key);
    PutU8(b, d.owner);
    PutU8(b, d.holder);
    PutStr(b, d.st.name);
    PutStr(b, d.st.link);
    PutU8(b, d.st.isUrl ? 1 : 0);
    PutU8(b, d.st.playing ? 1 : 0);
    PutU32(b, d.st.openSeq);
    PutU64(b, static_cast<uint64_t>(d.st.timeMs < 0 ? 0 : d.st.timeMs));
    PutF32(b, d.st.volume);
    PutF32(b, d.st.brightness);
}

// The states of `keys` (every known device when empty): from the host to a slot or every peer, from a
// client -- the owner -- to the host.
void SendStates(coop::net::Session* s, int slot, const std::vector<std::wstring>& keys) {
    std::vector<uint8_t> b;
    PutU8(b, kOpState);
    std::vector<const std::pair<const std::wstring, Dev>*> rows;
    for (const auto& kv : g_dev)
        if (keys.empty() || std::find(keys.begin(), keys.end(), kv.first) != keys.end()) rows.push_back(&kv);
    if (rows.empty()) return;
    if (rows.size() > 255) rows.resize(255);
    PutU8(b, static_cast<uint8_t>(rows.size()));
    for (const auto* kv : rows) PutState(b, kv->first, kv->second);
    if (b.size() > coop::blob_chunks::MaxBlobBytes()) return;
    Send(s, IsHost(s) ? slot : 0, b);
}

void Note(const std::wstring& key, uint8_t holder, void* actor) {
    const bool tv = MD::KindOf(actor) == MD::Kind::Tv;
    coop::chat_feed::Push(std::wstring(tv ? L"This TV" : L"This radio") + L" is being used by " +
                              coop::roster_ledger::DisplayName(holder) + L".",
                          coop::chat_feed::Keep::Transient);
    UE_LOGI("media_playback: '%ls' refused here -- slot %u has it", key.c_str(), static_cast<unsigned>(holder));
}

// ---- ownership ----

// This peer's copy plays from here: its next poll sends what it plays. The url last seen as owner is
// forgotten, since a follower's opens since then were the old owner's link, not a new pick.
void TakeOwnCopy(Dev& d) {
    d.dirty = true;
    d.lastAt = 0;
    d.rawUrl.clear();
    d.appliedSeq = d.st.openSeq;
}

void SendClaim(coop::net::Session* s, const std::wstring& key, bool hold) {
    std::vector<uint8_t> b;
    PutU8(b, kOpClaim);
    PutStr(b, key);
    PutU8(b, hold ? 1 : 0);
    Send(s, 0, b);
}

// HOST: `slot` takes the device -- its owner, and its holder when `hold`. False when another holds it.
bool HostClaim(coop::net::Session* s, const std::wstring& key, uint8_t slot, bool hold, uint64_t now) {
    Dev* d = DevFor(key);
    if (!d) return false;
    if (d->holder != kNoSlot && d->holder != slot) return false;
    const bool changed = d->owner != slot || (hold && d->holder != slot);
    d->owner = slot;
    if (hold) { d->holder = slot; d->holderSeenMs = now; }
    if (slot == 0 && changed) TakeOwnCopy(*d);
    if (changed) {
        UE_LOGI("media_playback: HOST '%ls' is slot %u's now%s", key.c_str(), static_cast<unsigned>(slot),
                hold ? " (in use)" : "");
        SendStates(s, -1, {key});
    }
    return true;
}

void HostRelease(coop::net::Session* s, const std::wstring& key, uint8_t slot) {
    auto it = g_dev.find(key);
    if (it == g_dev.end() || it->second.holder != slot) return;
    it->second.holder = kNoSlot;
    SendStates(s, -1, {key});
}

// This peer's player uses the device: it becomes the owner, and the holder when its interface opens.
// False when another player has it.
bool ClaimHere(coop::net::Session* s, void* actor, bool hold) {
    const std::wstring key = KeyOf(actor);
    Dev* d = DevFor(key);
    if (!d) return true;   // a device nobody can name is this peer's alone
    const uint8_t me = Me(s);
    if (d->holder != kNoSlot && d->holder != me) { Note(key, d->holder, actor); return false; }
    const uint64_t now = NowMs();
    if (IsHost(s)) return HostClaim(s, key, 0, hold, now);
    if (d->owner != me || (hold && d->holder != me)) {
        SendClaim(s, key, hold);
        // Taken here at once; the host's answer corrects it if another player got there first.
        if (d->owner != me) TakeOwnCopy(*d);
        d->owner = me;
    }
    if (hold) d->holder = me;
    return true;
}

void ReleaseHere(coop::net::Session* s, void* actor) {
    const std::wstring key = KeyOf(actor);
    if (key.empty() || !g_myOpen.erase(key)) return;
    if (IsHost(s)) { HostRelease(s, key, 0); return; }
    auto it = g_dev.find(key);
    if (it != g_dev.end() && it->second.holder == Me(s)) it->second.holder = kNoSlot;
    std::vector<uint8_t> b;
    PutU8(b, kOpRelease);
    PutStr(b, key);
    Send(s, 0, b);
}

// The owner's copy just opened a link.
void Track(void* actor, const std::wstring& name, const std::wstring& link, bool isUrl, bool pause) {
    Dev* d = DevFor(KeyOf(actor));
    if (!d) return;
    d->st.name = name;
    d->st.link = ToWire(link, isUrl);
    d->st.isUrl = isUrl;
    d->st.playing = !pause;
    d->st.openSeq = ++g_maxSeq;
    d->st.timeMs = 0;
    d->appliedSeq = d->st.openSeq;
    d->have = true;
    d->dirty = true;
    d->rawUrl.clear();
    d->lastT = 0;
    d->lastAt = NowMs();
    ShareIfAsset(d->st.link);
    UE_LOGI("media_playback: %ls '%ls' plays '%ls' here, its owner", MD::KindOf(actor) == MD::Kind::Tv ? L"TV" : L"radio",
            KeyOf(actor).c_str(), name.c_str());
}

// ---- the owner's poll ----

void OwnerPoll(coop::net::Session* s, uint64_t now) {
    const uint8_t me = Me(s);
    std::vector<std::wstring> send;
    for (auto& [key, d] : g_dev) {
        if (d.owner != me) continue;
        void* actor = coop::prop_key_index::Find(key);
        void* player = actor ? MD::PlayerOf(actor) : nullptr;
        if (!player) continue;
        bool playing = false;
        int64_t t = 0;
        MD::IsPlaying(player, playing);
        MD::TimeMs(player, t);
        // A link the device opened another way than openLink (a channel the remote changed, a playlist step)
        // is still the device's: its url, in the wire's form, names it.
        std::wstring url;
        if (MD::Url(player, url) && !url.empty() && url != d.rawUrl) {
            if (!d.rawUrl.empty() || !d.have) {
                const bool isUrl = url.find(L"://") != std::wstring::npos && url.compare(0, 7, L"file://") != 0;
                const std::wstring path = url.compare(0, 7, L"file://") == 0 ? url.substr(7) : url;
                const size_t slash = path.find_last_of(L"\\/");
                d.st.name = slash == std::wstring::npos ? path : path.substr(slash + 1);
                d.st.link = ToWire(path, isUrl);
                d.st.isUrl = isUrl;
                d.st.openSeq = ++g_maxSeq;
                d.appliedSeq = d.st.openSeq;
                d.have = true;
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
        float v = -1.f, br = -1.f;
        if (MD::ReadVolume(actor, v) && std::fabs(v - d.st.volume) > 0.005f) { d.st.volume = v; d.dirty = true; }
        if (MD::ReadBrightness(actor, br) && std::fabs(br - d.st.brightness) > 0.005f) { d.st.brightness = br; d.dirty = true; }
        if (!d.have) {
            // Nothing plays yet, but the interface is open: the holder's word to the host that it is there.
            if (d.holder == me && !IsHost(s) && now - d.sentMs >= kBeatMs) {
                d.sentMs = now;
                SendClaim(s, key, /*hold=*/true);
            }
            continue;
        }
        // On a change at once; every two seconds while it plays or this player has it open, which is also
        // the holder's word to the host that it is still there.
        if (d.dirty || ((playing || d.holder == me) && now - d.sentMs >= kBeatMs)) {
            d.dirty = false;
            d.sentMs = now;
            send.push_back(key);
        }
    }
    if (!send.empty()) SendStates(s, -1, send);
}

// HOST: a device that plays without anyone having opened it this session (the host's world was playing
// it before the session began) is the host's, tracked from its player's url.
void HostScan() {
    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    for (const auto& e : entries) {
        if (!e.actor || e.key.empty() || g_dev.count(e.key) || MD::KindOf(e.actor) == MD::Kind::None) continue;
        void* player = MD::PlayerOf(e.actor);
        bool playing = false;
        if (!player || !MD::IsPlaying(player, playing) || !playing) continue;
        if (Dev* d = DevFor(e.key)) { d->owner = 0; d->dirty = true; }   // the poll reads its url and sends it
    }
}

// HOST: a holder that has gone quiet has gone.
void HostLeases(coop::net::Session* s, uint64_t now) {
    for (auto& [key, d] : g_dev)
        if (d.holder != kNoSlot && d.holder != 0 && now - d.holderSeenMs > kLeaseMs) {
            UE_LOGI("media_playback: HOST frees '%ls': slot %u said nothing for %llu ms", key.c_str(),
                    static_cast<unsigned>(d.holder), static_cast<unsigned long long>(kLeaseMs));
            d.holder = kNoSlot;
            SendStates(s, -1, {key});
        }
}

// ---- a follower's apply ----

void FollowerApply(coop::net::Session* s, uint64_t now) {
    const uint8_t me = Me(s);
    for (auto& [key, d] : g_dev) {
        if (d.owner == me || !d.have || d.st.link.empty()) continue;
        void* actor = coop::prop_key_index::Find(key);
        if (!actor || MD::KindOf(actor) == MD::Kind::None) continue;
        if (d.appliedSeq != d.st.openSeq) {
            if (!Openable(d.st.link)) continue;   // the pool is still bringing the file
            if (MD::OpenLink(actor, d.st.name, FromWire(d.st.link), d.st.isUrl, !d.st.playing)) {
                d.appliedSeq = d.st.openSeq;
                d.openedMs = now;
                d.commandMs = now;
            }
            continue;
        }
        float v = 0.f;
        if (d.st.volume >= 0.f && MD::ReadVolume(actor, v) && std::fabs(v - d.st.volume) > 0.01f) MD::WriteVolume(actor, d.st.volume);
        if (d.st.brightness >= 0.f && MD::ReadBrightness(actor, v) && std::fabs(v - d.st.brightness) > 0.01f)
            MD::WriteBrightness(actor, d.st.brightness);
        void* player = MD::PlayerOf(actor);
        if (!player || now - d.openedMs < kSettleMs || now - d.commandMs < 500) continue;
        bool preparing = false;
        if (MD::IsPreparing(player, preparing) && preparing) continue;
        bool playing = false;
        int64_t t = 0;
        if (!MD::IsPlaying(player, playing) || !MD::TimeMs(player, t)) continue;
        if (d.st.playing && !playing) { MD::Play(player); d.commandMs = now; continue; }
        if (!d.st.playing && playing) { MD::Pause(player); d.commandMs = now; continue; }
        const int64_t expect = d.st.timeMs + (d.st.playing ? static_cast<int64_t>(now - d.recvMs) : 0);
        if ((t > expect ? t - expect : expect - t) > kDriftMs) { MD::Seek(player, expect); d.commandMs = now; }
    }
}

// ---- the watches ----

// Any peer: the device's own openLink. Its owner's runs and is tracked; a free device's makes this peer the
// owner; one in another player's hands is refused.
sg::Verdict OnOpenPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || MD::KindOf(c.object) == MD::Kind::None) return sg::Verdict::Run;
    std::wstring name, link;
    bool isUrl = false, pause = false;
    if (!MD::ReadOpenLinkArgs(c.function, c.locals, name, link, isUrl, pause)) return sg::Verdict::Run;
    if (!ClaimHere(s, c.object, /*hold=*/false)) return sg::Verdict::Cancel;
    Track(c.object, name, link, isUrl, pause);
    return sg::Verdict::Run;
}

// What plays next is the owner's roll alone.
sg::Verdict OnFinPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.object || MD::KindOf(c.object) == MD::Kind::None) return sg::Verdict::Run;
    auto it = g_dev.find(KeyOf(c.object));
    if (it == g_dev.end()) return IsHost(s) ? sg::Verdict::Run : sg::Verdict::Cancel;
    return it->second.owner == Me(s) ? sg::Verdict::Run : sg::Verdict::Cancel;
}

// The interface opening on a device: this player takes it, or is refused it.
sg::Verdict OnUiOpenPre(const sg::Call& c) {
    auto* s = Live();
    if (!s || c.fromOurCode || !c.locals) return sg::Verdict::Run;
    const int32_t off = R::FindParamOffset(c.function, L"radio");
    if (off < 0) return sg::Verdict::Run;
    void* device = nullptr;
    std::memcpy(&device, c.locals + off, sizeof(device));
    if (!device || MD::KindOf(device) == MD::Kind::None) return sg::Verdict::Run;
    if (!ClaimHere(s, device, /*hold=*/true)) return sg::Verdict::Cancel;
    const std::wstring key = KeyOf(device);
    if (!key.empty()) g_myOpen.insert(key);
    return sg::Verdict::Run;
}

sg::Verdict OnUiQuitPre(const sg::Call& c) {
    auto* s = Live();
    if (s && c.object) ReleaseHere(s, MD::DeviceOfInterface(c.object));
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[8] = {&kRowTvOpen, &kRowRadioOpen, &kRowTvFin, &kRowRadioFin,
                                               &kRowTvUiOpen, &kRowRadioUiOpen, &kRowTvUiQuit, &kRowRadioUiQuit};
    const int tags[8] = {kTagOpen, kTagOpen, kTagFin, kTagFin, kTagUiOpen, kTagUiOpen, kTagUiQuit, kTagUiQuit};
    const sg::PreFn pres[8] = {&OnOpenPre, &OnOpenPre, &OnFinPre, &OnFinPre,
                               &OnUiOpenPre, &OnUiOpenPre, &OnUiQuitPre, &OnUiQuitPre};
    bool pending = false;
    for (int i = 0; i < 8; ++i) {
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], tags[i], pres[i], nullptr);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (pending) {
        sg::ResolvePendingNames();
        for (int i = 0; i < 8; ++i) {
            if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], tags[i])) continue;
            g_saidLive[i] = true;
            if (sg::NameWatchLive(*rows[i], tags[i]))
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
    if (IsHost(s)) {
        if (now >= g_nextScan) { g_nextScan = now + kScanMs; HostScan(); }
        HostLeases(s, now);
    }
    OwnerPoll(s, now);
    FollowerApply(s, now);
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = Live();
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob) || blob.empty()) return;
    Reader r{blob.data(), blob.size()};
    const uint8_t op = r.U8();
    const bool host = IsHost(s);
    const uint8_t me = Me(s);
    const uint64_t now = NowMs();
    if (op == kOpState) {
        if (!host && senderSlot != 0) return;
        const uint8_t n = r.U8();
        std::vector<std::wstring> relay;
        for (uint8_t i = 0; i < n && r.ok; ++i) {
            const std::wstring key = r.Str();
            const uint8_t owner = r.U8(), holder = r.U8();
            State st;
            st.name = r.Str();
            st.link = r.Str();
            st.isUrl = r.U8() != 0;
            st.playing = r.U8() != 0;
            st.openSeq = r.U32();
            st.timeMs = static_cast<int64_t>(r.U64());
            st.volume = r.F32();
            st.brightness = r.F32();
            if (!r.ok || key.empty()) break;
            Dev* d = DevFor(key);
            if (!d) continue;
            g_maxSeq = (std::max)(g_maxSeq, st.openSeq);
            if (host) {
                // Only the owner speaks for a device; the host's own owner and holder stand.
                if (d->owner != senderSlot) continue;
                if (d->holder == senderSlot) d->holderSeenMs = now;
                d->st = std::move(st);
                d->recvMs = now;
                d->have = true;
                relay.push_back(key);
                continue;
            }
            d->owner = owner;
            d->holder = holder;
            if (owner == me) continue;   // this peer's own copy is the one that plays
            if (holder != me) g_myOpen.erase(key);
            d->st = std::move(st);
            d->recvMs = now;
            d->have = true;
        }
        if (host && !relay.empty()) SendStates(s, -1, relay);
        return;
    }
    if (!host) return;
    const std::wstring key = r.Str();
    if (!r.ok || key.empty()) return;
    if (op == kOpClaim) {
        const bool hold = r.U8() != 0;
        if (!r.ok) return;
        if (!HostClaim(s, key, senderSlot, hold, now)) {
            UE_LOGI("media_playback: HOST refused slot %u's claim on '%ls': slot %u has it", senderSlot, key.c_str(),
                    static_cast<unsigned>(g_dev[key].holder));
            SendStates(s, senderSlot, {key});   // the claimer learns who has it, and follows again
        }
        return;
    }
    if (op == kOpRelease) HostRelease(s, key, senderSlot);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = Live();
    if (s && IsHost(s) && !g_dev.empty()) SendStates(s, peerSlot, {});
}

void OnPeerGone(uint8_t slot) {
    g_asm.ClearSlot(slot);
    auto* s = Live();
    if (!s || !IsHost(s)) return;
    std::vector<std::wstring> changed;
    for (auto& [key, d] : g_dev) {
        bool c = false;
        if (d.holder == slot) { d.holder = kNoSlot; c = true; }
        if (d.owner == slot) { d.owner = 0; TakeOwnCopy(d); c = true; }
        if (c) changed.push_back(key);
    }
    if (!changed.empty()) SendStates(s, -1, changed);
}

void OnDisconnect() {
    g_asm.Clear();
    g_dev.clear();
    g_myOpen.clear();
    g_folderNorm.clear();
    g_maxSeq = 0;
    g_nextPoll = g_nextScan = 0;
}

}  // namespace coop::media_playback
