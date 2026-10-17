// coop/props/record_sync.cpp -- see coop/props/record_sync.h.

#include "coop/props/record_sync.h"

#include "coop/net/session.h"
#include "coop/player/local_streams.h"
#include "coop/props/active_drive.h"        // NowMs
#include "coop/props/prop_drive_stream.h"   // IsParked
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"
#include "coop/props/remote_prop.h"         // IsActorUnderAnyDrive

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::props::record_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

constexpr uint64_t kSweepMs          = 2000;   // the host re-walks its covered props this often
constexpr size_t   kCapturesPerTick  = 16;     // getData dispatches per frame, the cost bound
constexpr size_t   kPublishesPerTick = 4;      // records sent per frame, the wire bound
constexpr uint64_t kMinIntervalMs    = 1000;   // one prop's records at most this often
constexpr uint64_t kTouchMs          = 8000;   // how long a touch makes a prop this peer's to author
constexpr int      kAccumulatorStreak = 3;     // untouched changes in a row that name an accumulator

struct Row {
    ue_wrap::CachedObjRef ref;
    uint64_t digest = 0;
    bool     based = false;
    uint64_t touchedMs = 0;       // last time this peer's player held or let go of it
    uint64_t publishedMs = 0;
    int      untouchedStreak = 0;
    bool     accumulator = false;
};

std::atomic<coop::net::Session*> g_session{nullptr};
std::unordered_map<std::wstring, Row> g_rows;       // by Key
std::vector<std::wstring> g_work;                    // this sweep's keys, consumed front to back
size_t   g_workAt = 0;
uint64_t g_nextSweepMs = 0;
std::set<std::wstring> g_accumulatorClasses;        // named once each
uint64_t g_published = 0, g_based = 0, g_rebased = 0;

coop::net::Session* Live() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->connected()) ? s : nullptr;
}

std::wstring KeyOf(void* actor) {
    std::wstring k = ue_wrap::prop::GetInteractableKeyString(actor);
    return (k.empty() || k == L"None") ? std::wstring() : k;
}

// Another peer's hand or the host's driven stream moves it here: that peer authors its state now.
bool MovedByOther(void* actor) {
    return coop::remote_prop::IsActorUnderAnyDrive(actor) || coop::prop_drive_stream::IsParked(actor);
}

// One prop: digest, then publish a change if this peer authors it and the bounds allow.
void Visit(coop::net::Session* s, const std::wstring& key, void* actor, uint64_t now, size_t& publishes) {
    if (!coop::prop_save_data::Covers(actor) || MovedByOther(actor)) return;
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(actor, rec)) return;
    const uint64_t d = SR::StateDigest(rec);
    Row& row = g_rows[key];
    if (!row.ref.Is(actor)) { row = Row{}; row.ref.Set(actor); }
    if (!row.based) { row.digest = d; row.based = true; ++g_based; return; }
    if (d == row.digest) return;
    const bool touched = actor == coop::local_streams::LastHeldActor() || now - row.touchedMs < kTouchMs;
    if (touched) {
        row.untouchedStreak = 0;
    } else if (++row.untouchedStreak >= kAccumulatorStreak && !row.accumulator) {
        row.accumulator = true;
        const std::wstring cls = R::ClassNameOf(actor);
        if (g_accumulatorClasses.insert(cls).second)
            UE_LOGI("record_sync: '%ls' changes on its own sweep after sweep -- an accumulator, left to its anchor "
                    "rather than streamed", cls.c_str());
    }
    if (row.accumulator) { row.digest = d; return; }
    // A client authors only what it touched; the host authors everything else at rest.
    if (s->role() == coop::net::Role::Client && !touched) { row.digest = d; return; }
    if (now - row.publishedMs < kMinIntervalMs || publishes >= kPublishesPerTick) return;  // next pass sends it
    if (coop::prop_save_data::Publish(s, actor, key)) {
        row.digest = d;
        row.publishedMs = now;
        ++publishes;
        if (++g_published <= 20 || g_published % 200 == 0)
            UE_LOGI("record_sync: %ls '%ls' changed in place -- record %s (%llu sent)", R::ClassNameOf(actor).c_str(),
                    key.c_str(), s->role() == coop::net::Role::Host ? "published" : "sent to the host",
                    static_cast<unsigned long long>(g_published));
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    coop::net::Session* s = Live();
    if (!s) return;
    const uint64_t now = coop::active_drive::NowMs();
    size_t captures = 0, publishes = 0;
    if (s->role() == coop::net::Role::Host) {
        if (g_workAt >= g_work.size() && now >= g_nextSweepMs) {
            g_nextSweepMs = now + kSweepMs;
            g_work.clear();
            g_workAt = 0;
            std::vector<coop::prop_key_index::DebugEntry> entries;
            coop::prop_key_index::DebugCollect(entries);
            g_work.reserve(entries.size());
            for (const auto& e : entries) if (!e.key.empty()) g_work.push_back(e.key);
        }
        while (g_workAt < g_work.size() && captures < kCapturesPerTick) {
            const std::wstring& key = g_work[g_workAt++];
            void* a = coop::prop_key_index::Find(key);
            if (!a) continue;
            ++captures;
            Visit(s, key, a, now, publishes);
        }
        return;
    }
    // A client walks only what it authors: the prop in its hand and the ones it let go of lately.
    if (void* held = coop::local_streams::LastHeldActor()) NoteTouched(held);
    for (auto it = g_rows.begin(); it != g_rows.end() && captures < kCapturesPerTick;) {
        Row& row = it->second;
        void* a = row.ref.Get();
        if (!a || now - row.touchedMs >= kTouchMs) { it = g_rows.erase(it); continue; }
        ++captures;
        const std::wstring key = it->first;
        ++it;
        Visit(s, key, a, now, publishes);
    }
}

void NoteTouched(void* actor) {
    if (!actor || !Live()) return;
    const std::wstring key = KeyOf(actor);
    if (key.empty()) return;
    Row& row = g_rows[key];
    if (!row.ref.Is(actor)) {
        row = Row{};
        row.ref.Set(actor);
        // The first sight of a prop a client starts to author is its base: what it holds now is what the
        // host already has, and only a change from here is the client's to send.
        SR::SaveRecord rec;
        if (coop::prop_save_data::Covers(actor) && SR::CaptureRecord(actor, rec)) {
            row.digest = SR::StateDigest(rec);
            row.based = true;
        }
    }
    row.touchedMs = coop::active_drive::NowMs();
    row.untouchedStreak = 0;
}

void NoteApplied(void* actor) {
    if (!actor || !Live()) return;
    const std::wstring key = KeyOf(actor);
    if (key.empty()) return;
    SR::SaveRecord rec;
    if (!SR::CaptureRecord(actor, rec)) return;
    Row& row = g_rows[key];
    if (!row.ref.Is(actor)) { const uint64_t t = row.touchedMs; row = Row{}; row.ref.Set(actor); row.touchedMs = t; }
    row.digest = SR::StateDigest(rec);
    row.based = true;
    ++g_rebased;
}

void OnDisconnect() {
    if (g_published || g_based)
        UE_LOGI("record_sync: session end -- %llu records sent on change, %llu props based, %llu re-based on apply, "
                "%zu accumulator classes", static_cast<unsigned long long>(g_published),
                static_cast<unsigned long long>(g_based), static_cast<unsigned long long>(g_rebased),
                g_accumulatorClasses.size());
    g_rows.clear();
    g_work.clear();
    g_workAt = 0;
    g_nextSweepMs = 0;
    g_accumulatorClasses.clear();
    g_published = g_based = g_rebased = 0;
}

}  // namespace coop::props::record_sync
