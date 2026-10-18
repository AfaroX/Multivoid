// coop/media/media_pool.cpp -- see coop/media/media_pool.h.

#include "coop/media/media_pool.h"

#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/props/prop_key_index.h"     // DebugCollect: the frames and posters a landed file refreshes
#include "coop/text/utf8_codec.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/custom_assets.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace coop::media_pool {
namespace {

namespace fs = std::filesystem;
namespace GT = ue_wrap::game_thread;
namespace CA = ue_wrap::custom_assets;

constexpr uint8_t kOpPool = 0, kOpShare = 1, kOpWant = 2, kOpUnshare = 3;
constexpr size_t   kMaxPool = 512;
constexpr size_t   kMaxNameChars = 160;
constexpr size_t   kPageBudget = 48 * 1024;           // one pool page, under the blob ceiling
constexpr int      kPiecesPerTick = 4;                // per outgoing file; backpressure is the real pacer
constexpr size_t   kMaxOutPerSlot = 2;
constexpr uint64_t kRelistMs = 3000, kRewantMs = 10000, kReimportDelayMs = 1500, kReimportGapMs = 5000;

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

uint64_t CapOf(Kind k) {
    switch (k) {
    case Kind::Radio: return 256ull << 20;
    case Kind::Tv:    return 4096ull << 20;
    default:          return 64ull << 20;   // pictures and posters
    }
}

bool ExtAllowed(Kind k, const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring e = name.substr(dot + 1);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    static const wchar_t* kImg[] = {L"png", L"jpg", L"jpeg", L"bmp", L"tga", nullptr};
    static const wchar_t* kAud[] = {L"mp3", L"wav", L"ogg", L"flac", nullptr};
    static const wchar_t* kVid[] = {L"mp4", L"webm", L"avi", L"mkv", L"mov", L"wmv", L"m4v", nullptr};
    const wchar_t** list = k == Kind::Radio ? kAud : k == Kind::Tv ? kVid : kImg;
    for (; *list; ++list) if (e == *list) return true;
    return false;
}

// A pooled name is a bare file name the game would list: no path, no device, no hidden or partial file.
bool NameOk(Kind k, const std::wstring& n) {
    if (n.empty() || n.size() > kMaxNameChars || n[0] == L'.' || n.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : n)
        if (c < 0x20 || c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
            c == L'<' || c == L'>' || c == L'|')
            return false;
    return ExtAllowed(k, n);
}

struct Entry {
    Kind kind = Kind::Count;
    std::wstring name;
    uint64_t size = 0, hash = 0;
    uint8_t owner = 0;
    bool ready = false;
};

struct Out {
    int slot = 0;
    uint64_t hash = 0, size = 0;
    uint32_t count = 0, next = 0;
    std::unique_ptr<std::ifstream> in;
    std::vector<uint8_t> pending;      // a built piece the transport refused, sent again first
};

// One file this peer asked for. The sink (net thread) writes it; the game thread creates and retires it.
struct In {
    int from = 0;
    Kind kind = Kind::Count;
    std::wstring name, tmp, dst;
    uint64_t size = 0, fnv = 1469598103934665603ull, got = 0;
    uint32_t count = 0, next = 0;
    FILE* f = nullptr;
    bool upload = false;               // HOST: a client's share coming in
};
struct Done { uint64_t hash; bool ok; std::wstring why; };

struct HashJob { Kind kind; std::wstring name; uint64_t size, hash; bool ok; };
struct Req { int op; Kind kind; std::wstring name; uint64_t hash; };

std::atomic<coop::net::Session*> g_session{nullptr};
coop::blob_chunks::Assembler g_asm;
uint32_t g_seq = 1;

std::wstring g_folder;
std::vector<Entry> g_pool;
std::map<std::pair<int, std::wstring>, uint64_t> g_local;   // (kind, name) -> size
std::map<std::pair<int, std::wstring>, uint64_t> g_ownHash; // this peer's own files it hashed: -> hash
std::vector<Out> g_out;
std::map<uint64_t, uint64_t> g_wantedAt;                     // hash -> when this peer last asked
std::set<uint64_t> g_failed, g_conflict;
std::atomic<uint64_t> g_panelSeen{0};
uint64_t g_nextRelist = 0, g_reimportAt = 0, g_lastReimport = 0, g_nextSweep = 0;
std::wstring g_status;

// The client's pool under assembly: generation and pages.
uint32_t g_rxGen = 0;
std::map<uint16_t, std::vector<Entry>> g_rxPages;
uint16_t g_rxPageCount = 0;
uint32_t g_txGen = 0;

std::mutex g_inMu;                                            // g_in, g_done: the sink's
std::map<uint64_t, In> g_in;
std::vector<Done> g_done;

std::mutex g_hashMu;
std::vector<HashJob> g_hashDone;
std::atomic<int> g_hashRunning{0};

std::mutex g_reqMu;
std::vector<Req> g_reqs;

std::mutex g_snapMu;
Snapshot g_snap;

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->connected()) ? s : nullptr;
}

std::wstring PathOf(Kind k, const std::wstring& name) {
    return g_folder + L"\\" + KindFolder(k) + L"\\" + name;
}

// ---- wire ----

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(static_cast<uint8_t>(v & 0xFF)); b.push_back(static_cast<uint8_t>(v >> 8)); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF)); }
void PutName(std::vector<uint8_t>& b, const std::wstring& w) {
    std::string u = coop::text::ToUtf8Exact(w);
    if (u.size() > 0xFFFF) u.resize(0xFFFF);
    PutU16(b, static_cast<uint16_t>(u.size()));
    b.insert(b.end(), u.begin(), u.end());
}

struct Reader {
    const uint8_t* p; size_t n, o = 0; bool ok = true;
    bool Has(size_t k) { if (o + k > n) ok = false; return ok; }
    uint8_t U8() { if (!Has(1)) return 0; return p[o++]; }
    uint16_t U16() { if (!Has(2)) return 0; const uint16_t v = static_cast<uint16_t>(p[o] | (p[o + 1] << 8)); o += 2; return v; }
    uint32_t U32() { if (!Has(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(p[o + i]) << (8 * i); o += 4; return v; }
    uint64_t U64() { if (!Has(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(p[o + i]) << (8 * i); o += 8; return v; }
    std::wstring Name() {
        const uint16_t len = U16();
        if (!Has(len)) return std::wstring();
        std::wstring w;
        const bool good = coop::text::FromUtf8Strict(reinterpret_cast<const char*>(p + o), len, &w);
        o += len;
        if (!good) ok = false;
        return w;
    }
};

void PutEntry(std::vector<uint8_t>& b, const Entry& e) {
    PutU8(b, static_cast<uint8_t>(e.kind));
    PutU64(b, e.size);
    PutU64(b, e.hash);
    PutU8(b, e.owner);
    PutU8(b, e.ready ? 1 : 0);
    PutName(b, e.name);
}
bool ReadEntry(Reader& r, Entry& e) {
    const uint8_t k = r.U8();
    e.size = r.U64(); e.hash = r.U64(); e.owner = r.U8(); e.ready = r.U8() != 0; e.name = r.Name();
    if (!r.ok || k >= static_cast<uint8_t>(Kind::Count)) return false;
    e.kind = static_cast<Kind>(k);
    return NameOk(e.kind, e.name) && e.size <= CapOf(e.kind) && e.hash != 0;
}

bool SendTo(coop::net::Session* s, int slot, const std::vector<uint8_t>& blob) {
    return slot < 0 ? coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::MediaPool, g_seq++, blob)
                    : coop::blob_chunks::SendBlobToSlot(s, slot, coop::net::ReliableKind::MediaPool, g_seq++, blob);
}

// HOST: the pool as pages, to one slot or (slot < 0) every peer.
void SendPool(coop::net::Session* s, int slot) {
    std::vector<uint8_t> body;
    uint16_t n = 0;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> bodies;
    for (const Entry& e : g_pool) {
        std::vector<uint8_t> one;
        PutEntry(one, e);
        if (!body.empty() && body.size() + one.size() > kPageBudget) { bodies.emplace_back(n, std::move(body)); body.clear(); n = 0; }
        body.insert(body.end(), one.begin(), one.end());
        ++n;
    }
    bodies.emplace_back(n, std::move(body));   // an empty pool is one empty page
    const uint32_t gen = ++g_txGen;
    for (size_t i = 0; i < bodies.size(); ++i) {
        std::vector<uint8_t> b;
        PutU8(b, kOpPool);
        PutU32(b, gen);
        PutU16(b, static_cast<uint16_t>(i));
        PutU16(b, static_cast<uint16_t>(bodies.size()));
        PutU16(b, bodies[i].first);
        b.insert(b.end(), bodies[i].second.begin(), bodies[i].second.end());
        if (!SendTo(s, slot, b))
            UE_LOGW("media_pool: pool page %zu of %zu not sent%s -- the next change re-sends the pool", i + 1,
                    bodies.size(), slot < 0 ? "" : " to a joiner");
    }
}

void SendWant(coop::net::Session* s, int slot, uint64_t hash) {
    std::vector<uint8_t> b;
    PutU8(b, kOpWant);
    PutU64(b, hash);
    SendTo(s, slot, b);
}

// ---- files ----

void Relist() {
    g_local.clear();
    if (g_folder.empty()) return;
    for (int k = 0; k < static_cast<int>(Kind::Count); ++k) {
        std::error_code ec;
        const fs::path dir = fs::path(g_folder) / KindFolder(static_cast<Kind>(k));
        if (!fs::is_directory(dir, ec)) continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::wstring name = it->path().filename().wstring();
            if (!NameOk(static_cast<Kind>(k), name)) continue;
            g_local[{k, name}] = static_cast<uint64_t>(it->file_size(ec));
        }
    }
}

bool HaveLocal(const Entry& e) {
    auto it = g_local.find({static_cast<int>(e.kind), e.name});
    return it != g_local.end() && it->second == e.size;
}
bool NameTaken(const Entry& e) {
    auto it = g_local.find({static_cast<int>(e.kind), e.name});
    return it != g_local.end() && it->second != e.size;
}

void StartHash(Kind k, const std::wstring& name) {
    const std::wstring path = PathOf(k, name);
    g_hashRunning.fetch_add(1);
    std::thread([k, name, path]() {
        HashJob j{k, name, 0, 1469598103934665603ull, false};
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::vector<char> buf(1 << 20);
            while (in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const std::streamsize got = in.gcount();
                for (std::streamsize i = 0; i < got; ++i) { j.hash ^= static_cast<uint8_t>(buf[i]); j.hash *= 1099511628211ull; }
                j.size += static_cast<uint64_t>(got);
            }
            j.ok = true;
        }
        std::lock_guard<std::mutex> lk(g_hashMu);
        g_hashDone.push_back(std::move(j));
        g_hashRunning.fetch_sub(1);
    }).detach();
}

Entry* FindByHash(uint64_t h) {
    for (Entry& e : g_pool) if (e.hash == h) return &e;
    return nullptr;
}

// Start sending a file this peer holds to `slot`.
void StartOut(int slot, const Entry& e) {
    size_t mine = 0;
    for (const Out& o : g_out) {
        if (o.slot == slot && o.hash == e.hash) return;   // already on its way
        if (o.slot == slot) ++mine;
    }
    if (mine >= kMaxOutPerSlot) return;                    // the asker asks again; the queue stays short
    Out o;
    o.slot = slot;
    o.hash = e.hash;
    o.size = e.size;
    o.count = static_cast<uint32_t>(e.size == 0 ? 1 : (e.size + coop::net::kMediaChunkBytes - 1) / coop::net::kMediaChunkBytes);
    o.in = std::make_unique<std::ifstream>(PathOf(e.kind, e.name), std::ios::binary);
    if (!*o.in) {
        UE_LOGW("media_pool: '%ls' is pooled but could not be opened here -- not sent", e.name.c_str());
        return;
    }
    UE_LOGI("media_pool: sending '%ls' (%llu B, %u pieces) to slot %d", e.name.c_str(),
            static_cast<unsigned long long>(e.size), o.count, slot);
    g_out.push_back(std::move(o));
}

// Expect a file from `from`; the sink takes its pieces from here on.
void Expect(const Entry& e, int from, bool upload) {
    std::lock_guard<std::mutex> lk(g_inMu);
    if (g_in.count(e.hash)) return;
    In in;
    in.from = from;
    in.kind = e.kind;
    in.name = e.name;
    in.size = e.size;
    in.count = static_cast<uint32_t>(e.size == 0 ? 1 : (e.size + coop::net::kMediaChunkBytes - 1) / coop::net::kMediaChunkBytes);
    in.dst = PathOf(e.kind, e.name);
    // Outside every kind's folder: the game's import lists every file in the TV and radio folders, a
    // partial one included.
    in.tmp = g_folder + L"\\multivoid-incoming\\" + std::to_wstring(e.hash) + L".part";
    in.upload = upload;
    g_in[e.hash] = std::move(in);
}

void DropIn(In& in) {
    if (in.f) { std::fclose(in.f); in.f = nullptr; }
    if (!in.tmp.empty()) ::DeleteFileW(in.tmp.c_str());
}

void Broadcast() { if (auto* s = Live(); s && s->role() == coop::net::Role::Host) SendPool(s, -1); }

void Status(const std::wstring& w) { g_status = w; }

// A client's copy of the pool landed, or a host change: fetch what is missing.
void FetchMissing(coop::net::Session* s) {
    if (s->role() == coop::net::Role::Host) return;
    const uint64_t now = NowMs();
    for (const Entry& e : g_pool) {
        if (!e.ready || HaveLocal(e) || g_failed.count(e.hash)) continue;
        if (NameTaken(e)) { g_conflict.insert(e.hash); continue; }
        {
            std::lock_guard<std::mutex> lk(g_inMu);
            if (g_in.count(e.hash)) continue;
        }
        auto w = g_wantedAt.find(e.hash);
        if (w != g_wantedAt.end() && now - w->second < kRewantMs) continue;
        g_wantedAt[e.hash] = now;
        Expect(e, 0, false);
        SendWant(s, 0, e.hash);
    }
}

void AddOwn(coop::net::Session* s, const HashJob& j) {
    if (!j.ok || j.size == 0) { Status(L"Could not read " + j.name); return; }
    if (j.size > CapOf(j.kind)) { Status(j.name + L" is over the size limit for its kind"); return; }
    g_ownHash[{static_cast<int>(j.kind), j.name}] = j.hash;
    if (s->role() == coop::net::Role::Host) {
        if (FindByHash(j.hash)) return;
        if (g_pool.size() >= kMaxPool) { Status(L"The pool is full"); return; }
        g_pool.push_back(Entry{j.kind, j.name, j.size, j.hash, 0, true});
        Status(L"Shared " + j.name);
        UE_LOGI("media_pool: HOST shared its own '%ls' (%llu B)", j.name.c_str(), static_cast<unsigned long long>(j.size));
        Broadcast();
        return;
    }
    std::vector<uint8_t> b;
    PutU8(b, kOpShare);
    PutU8(b, static_cast<uint8_t>(j.kind));
    PutU64(b, j.size);
    PutU64(b, j.hash);
    PutName(b, j.name);
    SendTo(s, 0, b);
    Status(L"Offered " + j.name + L" to the host");
}

void RefreshImages() {
    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    int n = 0;
    for (const auto& e : entries)
        if (e.actor && CA::IsImageProp(e.actor) && CA::RefreshImage(e.actor)) ++n;
    UE_LOGI("media_pool: re-imported the asset folder; %d picture frame(s) and poster(s) looked up their image again", n);
}

void RunReqs(coop::net::Session* s) {
    std::vector<Req> reqs;
    { std::lock_guard<std::mutex> lk(g_reqMu); reqs.swap(g_reqs); }
    for (const Req& r : reqs) {
        if (r.op == 0) {   // share
            if (!NameOk(r.kind, r.name)) { Status(L"That file cannot be shared"); continue; }
            if (g_hashRunning.load() >= 2) { Status(L"Still reading the last files -- try again in a moment"); continue; }
            StartHash(r.kind, r.name);
            Status(L"Reading " + r.name + L" ...");
        } else if (r.op == 1) {   // unshare
            if (s->role() == coop::net::Role::Host) {
                g_pool.erase(std::remove_if(g_pool.begin(), g_pool.end(), [&](const Entry& e) { return e.hash == r.hash; }),
                             g_pool.end());
                Broadcast();
            } else {
                std::vector<uint8_t> b;
                PutU8(b, kOpUnshare);
                PutU64(b, r.hash);
                SendTo(s, 0, b);
            }
        } else {
            g_nextRelist = 0;
        }
    }
}

void PumpOut(coop::net::Session* s) {
    for (auto it = g_out.begin(); it != g_out.end();) {
        Out& o = *it;
        if (!s->IsSlotWorldReady(o.slot) && s->role() == coop::net::Role::Host) { it = g_out.erase(it); continue; }
        for (int n = 0; n < kPiecesPerTick && o.next < o.count; ++n) {
            if (o.pending.empty()) {
                coop::net::MediaChunkHeader h{o.hash, o.size, o.next, o.count};
                const uint64_t off = static_cast<uint64_t>(o.next) * coop::net::kMediaChunkBytes;
                const uint64_t left = o.size > off ? o.size - off : 0;
                const size_t len = static_cast<size_t>(left < coop::net::kMediaChunkBytes ? left : coop::net::kMediaChunkBytes);
                o.pending.resize(sizeof(h) + len);
                std::memcpy(o.pending.data(), &h, sizeof(h));
                if (len) {
                    o.in->read(reinterpret_cast<char*>(o.pending.data() + sizeof(h)), static_cast<std::streamsize>(len));
                    if (o.in->gcount() != static_cast<std::streamsize>(len)) { o.next = o.count + 1; break; }   // the file changed
                }
            }
            if (!s->SendReliableToSlot(o.slot, coop::net::ReliableKind::MediaChunk, o.pending.data(),
                                       static_cast<int>(o.pending.size())))
                break;   // the transport's backpressure: the same piece goes first next tick
            o.pending.clear();
            ++o.next;
        }
        if (o.next > o.count) {
            UE_LOGW("media_pool: a file being sent to slot %d changed or shrank on disk -- the send stopped", o.slot);
            it = g_out.erase(it);
        } else if (o.next == o.count && o.pending.empty()) {
            UE_LOGI("media_pool: sent %llu B to slot %d", static_cast<unsigned long long>(o.size), o.slot);
            it = g_out.erase(it);
        } else {
            ++it;
        }
    }
}

void TakeDone(coop::net::Session* s) {
    std::vector<Done> done;
    std::vector<uint64_t> uploads;
    {
        std::lock_guard<std::mutex> lk(g_inMu);
        done.swap(g_done);
        for (const Done& d : done) {
            auto it = g_in.find(d.hash);
            if (it == g_in.end()) continue;
            if (it->second.upload && d.ok) uploads.push_back(d.hash);
            DropIn(it->second);
            g_in.erase(it);
        }
    }
    bool any = false;
    for (const Done& d : done) {
        g_wantedAt.erase(d.hash);
        if (d.ok) { any = true; continue; }
        g_failed.insert(d.hash);
        UE_LOGW("media_pool: a file did not arrive whole (%ls)", d.why.c_str());
        Status(L"A shared file failed: " + d.why);
    }
    if (any) {
        g_nextRelist = 0;
        g_reimportAt = NowMs() + kReimportDelayMs;
    }
    if (s->role() == coop::net::Role::Host && !uploads.empty()) {
        for (uint64_t h : uploads) if (Entry* e = FindByHash(h)) e->ready = true;
        Broadcast();
    }
}

void BuildSnapshot(coop::net::Session* s) {
    Snapshot snap;
    snap.session = s != nullptr;
    snap.host = s && s->role() == coop::net::Role::Host;
    snap.folder = g_folder;
    snap.status = g_status;
    std::set<std::pair<int, std::wstring>> pooledNames;
    for (const Entry& e : g_pool) pooledNames.insert({static_cast<int>(e.kind), e.name});
    for (const auto& [k, size] : g_local)
        snap.local.push_back(LocalFile{static_cast<Kind>(k.first), k.second, size, pooledNames.count(k) > 0});
    std::lock_guard<std::mutex> lk(g_inMu);
    for (const Entry& e : g_pool) {
        PoolRow r{e.kind, e.name, e.size, e.hash, e.owner, e.ready, PoolRow::Here::Missing, 0.f};
        auto in = g_in.find(e.hash);
        if (HaveLocal(e)) r.here = PoolRow::Here::Have;
        else if (in != g_in.end()) {
            r.here = PoolRow::Here::Fetching;
            r.progress = in->second.count ? static_cast<float>(in->second.next) / in->second.count : 0.f;
        } else if (g_conflict.count(e.hash)) r.here = PoolRow::Here::Conflict;
        else if (g_failed.count(e.hash)) r.here = PoolRow::Here::Failed;
        snap.pool.push_back(std::move(r));
    }
    std::lock_guard<std::mutex> lk2(g_snapMu);
    g_snap = std::move(snap);
}

// ---- the sink: one piece, on the net thread ----

void OnPiece(int peerSlot, const uint8_t* data, int len) {
    if (!data || len < static_cast<int>(sizeof(coop::net::MediaChunkHeader))) return;
    coop::net::MediaChunkHeader h{};
    std::memcpy(&h, data, sizeof(h));
    const uint8_t* body = data + sizeof(h);
    const size_t blen = static_cast<size_t>(len) - sizeof(h);
    std::lock_guard<std::mutex> lk(g_inMu);
    auto it = g_in.find(h.hash);
    if (it == g_in.end()) return;                       // not asked for
    In& in = it->second;
    if (in.from != peerSlot) return;                    // another link's copy: not the one asked for
    if (h.size != in.size || h.count != in.count || h.index != in.next) {
        g_done.push_back(Done{h.hash, false, in.name + L": a piece arrived out of order"});
        return;
    }
    if (h.index == 0) {
        std::error_code ec;
        fs::create_directories(fs::path(in.dst).parent_path(), ec);
        fs::create_directories(fs::path(in.tmp).parent_path(), ec);
        in.f = _wfopen(in.tmp.c_str(), L"wb");
        if (!in.f) { g_done.push_back(Done{h.hash, false, in.name + L": could not be written here"}); return; }
    }
    if (!in.f) return;
    if (blen && std::fwrite(body, 1, blen, in.f) != blen) {
        g_done.push_back(Done{h.hash, false, in.name + L": the disk refused the write"});
        return;
    }
    for (size_t i = 0; i < blen; ++i) { in.fnv ^= body[i]; in.fnv *= 1099511628211ull; }
    in.got += blen;
    ++in.next;
    if (in.next < in.count) return;
    std::fclose(in.f);
    in.f = nullptr;
    if (in.got != in.size || in.fnv != h.hash) {
        g_done.push_back(Done{h.hash, false, in.name + L": the copy does not match the original"});
        return;
    }
    // Never over another file: a name taken since the request keeps its file, and the copy is dropped.
    if (!::MoveFileExW(in.tmp.c_str(), in.dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
        g_done.push_back(Done{h.hash, false, in.name + L": the name is taken by another file here"});
        return;
    }
    in.tmp.clear();
    g_done.push_back(Done{h.hash, true, in.name});
}

}  // namespace

const wchar_t* KindFolder(Kind k) {
    switch (k) {
    case Kind::PicH:   return L"paintings\\h";
    case Kind::PicS:   return L"paintings\\s";
    case Kind::PicT:   return L"paintings\\t";
    case Kind::PicV:   return L"paintings\\v";
    case Kind::Poster: return L"posters";
    case Kind::Radio:  return L"radio";
    case Kind::Tv:     return L"tv";
    default:           return L"";
    }
}

const char* KindLabel(Kind k) {
    switch (k) {
    case Kind::PicH:   return "Picture (wide)";
    case Kind::PicS:   return "Picture (square)";
    case Kind::PicT:   return "Picture (table)";
    case Kind::PicV:   return "Picture (tall)";
    case Kind::Poster: return "Poster";
    case Kind::Radio:  return "Radio";
    case Kind::Tv:     return "TV";
    default:           return "?";
    }
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (session) session->SetMediaSink(&OnPiece);
}

void Tick() {
    auto* s = Live();
    const uint64_t now = NowMs();
    if (!s) {
        std::lock_guard<std::mutex> lk(g_snapMu);
        g_snap.session = false;
        return;
    }
    if (g_folder.empty()) {
        g_folder = CA::AssetFolder();
        if (g_folder.empty()) return;
        UE_LOGI("media_pool: the asset folder is '%ls'", g_folder.c_str());
        g_nextRelist = 0;
    }
    if (now >= g_nextSweep) {
        g_nextSweep = now + 1000;
        g_asm.Sweep(std::chrono::steady_clock::now(), std::chrono::seconds(20));
    }
    RunReqs(s);
    {
        std::vector<HashJob> jobs;
        { std::lock_guard<std::mutex> lk(g_hashMu); jobs.swap(g_hashDone); }
        for (const HashJob& j : jobs) AddOwn(s, j);
    }
    TakeDone(s);
    const bool panel = now - g_panelSeen.load(std::memory_order_relaxed) < 2000;
    if (now >= g_nextRelist && (panel || g_nextRelist == 0)) {
        g_nextRelist = now + kRelistMs;
        Relist();
        FetchMissing(s);
    }
    PumpOut(s);
    if (g_reimportAt && now >= g_reimportAt && now - g_lastReimport >= kReimportGapMs) {
        g_reimportAt = 0;
        g_lastReimport = now;
        if (CA::Reimport()) RefreshImages();
        else UE_LOGW("media_pool: the game's importAssets did not run -- new files show after the next world load");
    }
    BuildSnapshot(s);
}

void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = Live();
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob) || blob.empty()) return;
    Reader r{blob.data(), blob.size()};
    const uint8_t op = r.U8();
    const bool host = s->role() == coop::net::Role::Host;
    if (op == kOpPool) {
        if (host || senderSlot != 0) return;
        const uint32_t gen = r.U32();
        const uint16_t page = r.U16(), pages = r.U16(), n = r.U16();
        if (!r.ok || pages == 0 || page >= pages || n > kMaxPool) return;
        if (gen != g_rxGen) { g_rxGen = gen; g_rxPages.clear(); g_rxPageCount = pages; }
        std::vector<Entry> list;
        for (uint16_t i = 0; i < n; ++i) {
            Entry e;
            if (!ReadEntry(r, e)) return;
            list.push_back(std::move(e));
        }
        g_rxPages[page] = std::move(list);
        if (g_rxPages.size() < g_rxPageCount) return;
        g_pool.clear();
        for (auto& [i, l] : g_rxPages) for (auto& e : l) if (g_pool.size() < kMaxPool) g_pool.push_back(std::move(e));
        g_rxPages.clear();
        UE_LOGI("media_pool: CLIENT took the host's pool (%zu file(s))", g_pool.size());
        Relist();
        FetchMissing(s);
        return;
    }
    if (op == kOpWant) {
        const uint64_t hash = r.U64();
        if (!r.ok) return;
        if (host) {   // a client asks for a pooled file
            const Entry* e = FindByHash(hash);
            if (e && e->ready && HaveLocal(*e)) StartOut(senderSlot, *e);
            return;
        }
        if (senderSlot != 0) return;   // the host asks this client for a file it shared
        for (const auto& [k, h] : g_ownHash)
            if (h == hash) {
                Entry e{static_cast<Kind>(k.first), k.second, g_local.count(k) ? g_local[k] : 0, h, 0, true};
                if (e.size) StartOut(0, e);
                return;
            }
        return;
    }
    if (!host) return;
    if (op == kOpShare) {
        Entry e;
        const uint8_t k = r.U8();
        e.size = r.U64(); e.hash = r.U64(); e.name = r.Name();
        if (!r.ok || k >= static_cast<uint8_t>(Kind::Count)) return;
        e.kind = static_cast<Kind>(k);
        e.owner = senderSlot;
        if (!NameOk(e.kind, e.name) || e.size == 0 || e.size > CapOf(e.kind) || e.hash == 0) {
            UE_LOGW("media_pool: slot %u offered a file the pool refuses (kind %u, %llu B)", senderSlot, k,
                    static_cast<unsigned long long>(e.size));
            return;
        }
        if (FindByHash(e.hash) || g_pool.size() >= kMaxPool) return;
        Relist();
        if (NameTaken(e)) {
            UE_LOGW("media_pool: slot %u offered '%ls', but the host holds another file under that name -- refused",
                    senderSlot, e.name.c_str());
            return;
        }
        e.ready = HaveLocal(e);
        g_pool.push_back(e);
        UE_LOGI("media_pool: HOST took slot %u's '%ls' (%llu B)%s", senderSlot, e.name.c_str(),
                static_cast<unsigned long long>(e.size), e.ready ? "" : " -- asking for the file");
        if (!e.ready) { Expect(e, senderSlot, true); SendWant(s, senderSlot, e.hash); }
        Broadcast();
        return;
    }
    if (op == kOpUnshare) {
        const uint64_t hash = r.U64();
        if (!r.ok) return;
        const size_t before = g_pool.size();
        g_pool.erase(std::remove_if(g_pool.begin(), g_pool.end(),
                                    [&](const Entry& e) { return e.hash == hash && e.owner == senderSlot; }),
                     g_pool.end());
        if (g_pool.size() != before) Broadcast();
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = Live();
    if (s && s->role() == coop::net::Role::Host) SendPool(s, peerSlot);
}

void OnPeerGone(uint8_t slot) {
    g_asm.ClearSlot(slot);
    g_out.erase(std::remove_if(g_out.begin(), g_out.end(), [&](const Out& o) { return o.slot == slot; }), g_out.end());
    {
        std::lock_guard<std::mutex> lk(g_inMu);
        for (auto it = g_in.begin(); it != g_in.end();) {
            if (it->second.from == slot) { DropIn(it->second); it = g_in.erase(it); }
            else ++it;
        }
    }
    auto* s = Live();
    if (s && s->role() == coop::net::Role::Host) {
        const size_t before = g_pool.size();
        g_pool.erase(std::remove_if(g_pool.begin(), g_pool.end(),
                                    [&](const Entry& e) { return e.owner == slot && !e.ready; }),
                     g_pool.end());
        if (g_pool.size() != before) Broadcast();
    }
}

void OnDisconnect() {
    g_asm.Clear();
    g_out.clear();
    {
        std::lock_guard<std::mutex> lk(g_inMu);
        for (auto& [h, in] : g_in) DropIn(in);
        g_in.clear();
        g_done.clear();
    }
    g_pool.clear();
    g_wantedAt.clear();
    g_failed.clear();
    g_conflict.clear();
    g_rxPages.clear();
    g_rxGen = 0;
    g_folder.clear();
    g_reimportAt = 0;
    g_status.clear();
    std::lock_guard<std::mutex> lk(g_snapMu);
    g_snap = Snapshot{};
}

Snapshot GetSnapshot() {
    std::lock_guard<std::mutex> lk(g_snapMu);
    return g_snap;
}

void NotePanelOpen() { g_panelSeen.store(NowMs(), std::memory_order_relaxed); }

void RequestShare(Kind kind, const std::wstring& name) {
    std::lock_guard<std::mutex> lk(g_reqMu);
    g_reqs.push_back(Req{0, kind, name, 0});
}

void RequestUnshare(uint64_t hash) {
    std::lock_guard<std::mutex> lk(g_reqMu);
    g_reqs.push_back(Req{1, Kind::Count, std::wstring(), hash});
}

void RequestRescan() {
    std::lock_guard<std::mutex> lk(g_reqMu);
    g_reqs.push_back(Req{2, Kind::Count, std::wstring(), 0});
}

}  // namespace coop::media_pool
