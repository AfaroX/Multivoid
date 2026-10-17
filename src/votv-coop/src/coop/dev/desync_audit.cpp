// coop/dev/desync_audit.cpp -- see coop/dev/desync_audit.h.

#include "coop/dev/desync_audit.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/local_streams.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/daynightcycle.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace coop::dev::desync_audit {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// One snapshot per minute of the shared clock. The clock is game seconds; a minute keeps the cost --
// two reads and, for a prop with its own save state, one getData per keyed prop -- off every frame.
constexpr float kPeriodGameS = 60.0f;

bool  g_haveBucket = false;
long  g_lastBucket = 0;
bool  g_headerWritten = false;
std::map<std::wstring, uint64_t> g_localBirths;  // "class <- caller" -> count, the client's

std::wstring AuditPath() { return ue_wrap::paths::ExeDir() + L"\\multivoid-desync.txt"; }

FILE* Open(const wchar_t* mode) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, AuditPath().c_str(), mode) != 0) return nullptr;
    return f;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

// What moves the prop right now on this peer, which the comparator uses to skip a pose that is
// legitimately in flight: H this player's hand, S another player's held stream or a driven stream.
char Mover(void* actor) {
    if (actor == coop::local_streams::LastHeldActor()) return 'H';
    if (coop::remote_prop::IsActorUnderAnyDrive(actor) || coop::prop_drive_stream::IsParked(actor)) return 'S';
    return '-';
}

void Snapshot(coop::net::Session& s, long bucket, float clock) {
    std::vector<coop::prop_key_index::DebugEntry> entries;
    coop::prop_key_index::DebugCollect(entries);
    FILE* f = Open(L"a");
    if (!f) {
        UE_LOGW("desync_audit: could not open %ls -- snapshot %ld skipped", AuditPath().c_str(), bucket);
        return;
    }
    const char* role = s.role() == coop::net::Role::Host ? "host" : "client";
    if (!g_headerWritten) {
        g_headerWritten = true;
        std::fprintf(f, "# multivoid desync audit -- role=%s; columns: key class x y z pitch yaw roll phys mover eid record\n",
                     role);
    }
    std::fprintf(f, "@snapshot %ld role=%s clock=%.1f props=%zu\n", bucket, role, clock, entries.size());
    size_t written = 0, records = 0;
    for (const auto& e : entries) {
        void* a = e.actor;
        if (!a || !R::IsLiveByIndex(a, e.idx)) continue;
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!ue_wrap::engine::TryGetActorLocation(a, loc) || !ue_wrap::engine::TryGetActorRotation(a, rot)) continue;
        // Physics state: T static, F frozen, Z asleep, A awake -- the comparator only judges a pose at rest.
        char phys = 'A';
        if (ue_wrap::prop::IsStatic(a)) phys = 'T';
        else if (ue_wrap::prop::IsFrozen(a)) phys = 'F';
        else if (ue_wrap::prop::IsSleeping(a)) phys = 'Z';
        const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(a);
        uint64_t digest = 0;
        void* cls = R::ClassOf(a);
        if (cls && SR::OverridesGetData(cls)) {
            SR::SaveRecord rec;
            if (SR::CaptureRecord(a, rec)) { digest = SR::StateDigest(rec); ++records; }
        }
        std::fprintf(f, "%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%.1f\t%c\t%c\t%u\t%016llx\n",
                     Narrow(e.key).c_str(), Narrow(R::ClassNameOf(a)).c_str(), loc.X, loc.Y, loc.Z,
                     rot.Pitch, rot.Yaw, rot.Roll, phys, Mover(a),
                     eid == coop::element::kInvalidId ? 0u : static_cast<unsigned>(eid),
                     static_cast<unsigned long long>(digest));
        ++written;
    }
    if (!g_localBirths.empty()) {
        std::fprintf(f, "@local-births %ld\n", bucket);
        for (const auto& kv : g_localBirths)
            std::fprintf(f, "%llu\t%s\n", static_cast<unsigned long long>(kv.second), Narrow(kv.first).c_str());
    }
    std::fclose(f);
    UE_LOGI("desync_audit: snapshot %ld at clock %.0f -- %zu props, %zu records digested, %zu local-only birth kinds",
            bucket, clock, written, records, g_localBirths.size());
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::desync_audit);
    return s;
}

void Tick(coop::net::Session& session) {
    if (!IsEnabled() || !session.connected()) return;
    float total = 0.f, day = 0.f, scale = 0.f;
    if (!ue_wrap::daynightcycle::ReadClock(total, day, scale)) return;
    const long bucket = static_cast<long>(std::floor(total / kPeriodGameS));
    if (!g_haveBucket) { g_haveBucket = true; g_lastBucket = bucket; return; }  // first sight: cut at the next edge
    if (bucket == g_lastBucket) return;
    g_lastBucket = bucket;
    Snapshot(session, bucket, total);
}

std::wstring CallerName() {
    if (!IsEnabled()) return std::wstring();
    const ue_wrap::ufunction_hook::CallerFrame fr = ue_wrap::ufunction_hook::CurrentCallerFrame();
    if (!fr.function) return std::wstring();
    void* outer = R::OuterOf(fr.function);
    std::wstring n = outer ? R::ToString(R::NameOf(outer)) : std::wstring(L"?");
    n += L".";
    n += R::ToString(R::NameOf(fr.function));
    return n;
}

void NoteLocalOnlyBirth(const std::wstring& cls, const std::wstring& caller) {
    if (!IsEnabled()) return;
    ++g_localBirths[cls + L" <- " + (caller.empty() ? std::wstring(L"(no script caller)") : caller)];
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    if (!g_localBirths.empty()) {
        if (FILE* f = Open(L"a")) {
            std::fprintf(f, "@local-births session-end\n");
            for (const auto& kv : g_localBirths)
                std::fprintf(f, "%llu\t%s\n", static_cast<unsigned long long>(kv.second), Narrow(kv.first).c_str());
            std::fclose(f);
        }
    }
    g_localBirths.clear();
    g_haveBucket = false;
    g_headerWritten = false;
}

}  // namespace coop::dev::desync_audit
