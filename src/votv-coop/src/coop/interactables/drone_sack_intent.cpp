// coop/interactables/drone_sack_intent.cpp -- see coop/interactables/drone_sack_intent.h.

#include "coop/interactables/drone_sack_intent.h"

#include "coop/element/intent_authority.h"
#include "coop/net/intent_bucket.h"
#include "coop/net/intent_queue.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"  // PerSlotState
#include "coop/props/active_drive.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::drone_sack_intent {
namespace {

namespace D  = ue_wrap::drone;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

// The verb every route to a drop reaches, watched on the drone's own class by name: the drone is a
// level actor, never kept by pointer across worlds.
UE_FACT_FN(kRowDropSack, "drone_sack_intent", L"drone_C", L"dropSack", Declared);
constexpr int kTagDropSack = 0x44534143;  // 'DSAC'

// The drone hovers over its pad and the sack hangs below it, so the reach is the pad's, wider than
// the console's arm's length.
constexpr float kDroneReachUU = 700.0f;

// One press is one drop; a second while the host's drone is empty is refused. The bound only keeps
// a stalled client's backlog from arriving all at once.
constexpr coop::net::IntentBudget kDropBudget{2.0f, 1.0f};
constexpr size_t kMaxPending = 2;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchInstalled = false;
bool g_watchLive = false;

uint64_t g_sent = 0, g_dropped = 0, g_denied = 0;

// One sender: its queued drops with their bucket, and the wait's latch, cleared with its occupant.
struct Slot {
    coop::net::IntentQueue<coop::net::DroneFlyIntentPayload> queue;
    bool waitSaid = false;   // a wait for the body, said once a streak
};
coop::roster_ledger::PerSlotState<Slot> g_slots;

// ---- host side -------------------------------------------------------------------------------
// One drop, run or refused and then consumed, or left queued while the host has no body for the
// sender (the mid-join window, which the neighbouring lanes wait out the same way).
bool Execute(coop::net::Session& s, uint8_t slot) {
    const auto token = coop::element::IntentTarget::ForClientIntent(s, slot, kDroneReachUU);
    if (!token.HasBody()) {
        if (!g_slots[slot].waitSaid) {
            g_slots[slot].waitSaid = true;
            UE_LOGI("[DRONE-SACK] slot %u's drop waits: the host has no body for it yet",
                    static_cast<unsigned>(slot));
        }
        return false;
    }
    void* drone = D::EnsureResolved() ? D::Find() : nullptr;
    if (!drone) {
        ++g_denied;
        UE_LOGI("[DRONE-SACK] DENY slot=%u -- no drone in this world", static_cast<unsigned>(slot));
        return true;
    }
    if (token.Authorize(drone).outcome != coop::element::IntentOutcome::Ok) {
        ++g_denied;
        UE_LOGI("[DRONE-SACK] DENY slot=%u -- the drone is out of the sender's reach", static_cast<unsigned>(slot));
        return true;
    }
    // The host's own drone is the truth: a client's mirror showed the option from the streamed
    // hasSack, which a drop by another peer may already have cleared here.
    if ((D::ReadFxBits(drone) & D::kFxHasSack) == 0) {
        ++g_denied;
        UE_LOGI("[DRONE-SACK] DENY slot=%u -- the drone carries no sack here", static_cast<unsigned>(slot));
        return true;
    }
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(drone), kRowDropSack.Name());
    if (!fn) {
        ++g_denied;
        UE_LOGW("[DRONE-SACK] slot=%u -- dropSack did not resolve on the drone", static_cast<unsigned>(slot));
        return true;
    }
    R::CallFunction(drone, fn, nullptr);
    ++g_dropped;
    UE_LOGI("[DRONE-SACK] DROPPED the drone's sack for slot=%u (#%llu) -- the sack crosses as a host birth",
            static_cast<unsigned>(slot), static_cast<unsigned long long>(g_dropped));
    return true;
}

// ---- the client's gate -----------------------------------------------------------------------
// Every drop on a client is a player's: the mirror's own tick, the drone's only other caller, is
// suppressed there (coop/interactables/drone_sync). The host's own call reaches this watch too and
// runs, since the gate refuses on a client alone.
sg::Verdict OnDropSackPre(const sg::Call& call) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || !call.object) return sg::Verdict::Run;
    coop::net::DroneFlyIntentPayload p{};
    p.verb = coop::net::kDroneVerbDropSack;
    if (!s->SendReliable(coop::net::ReliableKind::DroneFlyIntent, &p, sizeof(p))) {
        UE_LOGW("[DRONE-SACK] a drop was not sent (the session refused it); the drone keeps its sack");
        return sg::Verdict::Cancel;
    }
    ++g_sent;
    UE_LOGI("[DRONE-SACK] CLIENT SENT a drop (#%llu) -- the host's drone drops its sack",
            static_cast<unsigned long long>(g_sent));
    return sg::Verdict::Cancel;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (!g_watchInstalled)
        g_watchInstalled = sg::WatchName(kRowDropSack, kTagDropSack, &OnDropSackPre, nullptr);
}

void Tick(coop::net::Session& session) {
    if (!g_watchInstalled) {
        g_watchInstalled = sg::WatchName(kRowDropSack, kTagDropSack, &OnDropSackPre, nullptr);
        return;
    }
    if (!g_watchLive && sg::NameWatchLive(kRowDropSack, kTagDropSack)) {
        g_watchLive = true;
        UE_LOGI("[DRONE-SACK] the drop-sack gate is live");
    }
    if (!session.running() || session.role() != coop::net::Role::Host) return;
    for (uint8_t slot = 1; slot < coop::net::kMaxPeers; ++slot) {
        Slot& sl = g_slots[slot];
        sl.queue.Serve(kDropBudget, coop::active_drive::NowMs, [&](const coop::net::DroneFlyIntentPayload&) {
            if (!Execute(session, slot)) return coop::net::Served::Wait;  // a wait runs nothing: no token spent
            sl.waitSaid = false;
            return coop::net::Served::Done;
        });
    }
}

void OnDropSackIntent(coop::net::Session& session, uint8_t senderSlot) {
    if (session.role() != coop::net::Role::Host) return;
    if (senderSlot < 1 || senderSlot >= coop::net::kMaxPeers) return;
    coop::net::DroneFlyIntentPayload p{};
    p.verb = coop::net::kDroneVerbDropSack;
    if (!g_slots[senderSlot].queue.Push(std::move(p), kMaxPending)) ++g_denied;
}

void OnDisconnect() {
    if (g_sent || g_dropped || g_denied)
        UE_LOGI("[DRONE-SACK] session end -- sent=%llu dropped=%llu denied=%llu",
                static_cast<unsigned long long>(g_sent), static_cast<unsigned long long>(g_dropped),
                static_cast<unsigned long long>(g_denied));
    g_sent = g_dropped = g_denied = 0;
}

}  // namespace coop::drone_sack_intent
