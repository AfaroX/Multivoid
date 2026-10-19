// coop/props/prop_drive_stream.cpp -- see coop/props/prop_drive_stream.h.

#include "coop/props/prop_drive_stream.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/active_drive.h"
#include "coop/props/prop_key_index.h"  // Find: the index only, never the cold walk
#include "coop/props/prop_park.h"             // Park / Unpark: the parked prop, and a Character welded on it
#include "coop/props/prop_wire_parity.h"      // the join converge's own physics restore
#include "coop/player/local_streams.h"
#include "coop/player/hand_item.h"
#include "coop/props/remote_prop.h"           // ResolveLiveActorByEid, IsActorUnderAnyDrive
#include "coop/props/prop_drive_host.h"       // Owns, Yield, NoteSeenGen: this peer's own streams
#include "coop/player/players_registry.h"     // LocalPeerId: the coin's side
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/game_thread.h"     // RegisterPreObserver: the use press
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"  // ReadMainPlayerLookAtActor
#include "ue_wrap/facts/function_row.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::prop_drive_stream {
namespace {

// A hand owns the prop: a peer's held-prop stream, or this player's own grab -- its grab slot or the
// hotbar hand axis -- which no stream from the host knows of yet. Every branch of this drive yields to
// it, the first pose, the steady drive and both end paths, so a host stream that crossed the grab in
// flight never pulls the prop out of this player's hand.
bool HandOwns(void* actor) {
    return coop::remote_prop::IsActorUnderAnyDrive(actor) || actor == coop::local_streams::LastHeldActor() ||
           coop::hand_item::IsHandAxisActor(actor);
}

namespace AD = coop::active_drive;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;

struct Drive {
    AD::ActiveDrive d;
    uint8_t gen = 0;
    int owner = -1;               // the slot whose stream this is
    bool physicsParked = false;   // this module turned simulation off, so the end turns it on
    uint32_t relatches = 0;       // the game switched the parked copy's simulation back on, this park
    // A placeholder: the holder's release said its stream follows, so the copy is parked here before any
    // pose; it expires, physics back, if no pose comes.
    bool placeholder = false;
    uint64_t placeholderUntilMs = 0;
    uint64_t grabUntilMs = 0;     // this player reached for it: physics back until then, for the grab
};
constexpr uint64_t kPlaceholderMs = 2500;
constexpr uint64_t kGrabWindowMs  = 400;

// A park whose copy the game switches back to simulating this many times is being fought every
// tick, not nudged once: said loudly, once, as the held-prop drive says it (remote_prop).
constexpr uint32_t kRelatchLoud = 60;
// A closed generation and when it closed. It shuts out the datagrams that outlive its stream, for a few
// seconds: after that a lower generation is a new stream from a peer that never saw this one -- a joiner,
// or a rejoiner whose count began again -- not a late datagram.
struct Closed { uint8_t gen = 0; uint64_t atMs = 0; };
constexpr uint64_t kClosedMs = 3000;
std::unordered_map<uint32_t, Drive>  g_drives;    // eid -> drive
std::unordered_map<uint32_t, Closed> g_endedGen;  // eid -> the generation its end edge closed
// eid -> a generation a hand took over before its end edge landed. Its end pose is older than
// whatever the hand did since, so a late end of it closes the generation and writes nothing.
std::unordered_map<uint32_t, Closed> g_yieldedGen;

// A pose whose prop has not arrived here is retried on a throttle, never per tick: the host
// re-sends every tick the prop moves, and though the resolve is two O(1) lookups, a line per
// miss is not. One line names a sustained miss.
struct Miss { uint64_t nextTryMs = 0; uint32_t count = 0; };
std::unordered_map<uint32_t, Miss> g_miss;
constexpr uint64_t kMissRetryMs = 250;
constexpr uint32_t kMissSaidAt  = 40;     // about ten seconds of misses: one line

// An end edge whose prop has not arrived here yet -- a joiner mid-drag whose snapshot is still
// landing -- is kept until the prop resolves, so its copy still gets the rest pose the host's
// came to, or until it expires.
struct PendingEnd { coop::net::PropDriveEndPayload p; uint64_t expiresMs = 0; };
std::unordered_map<uint32_t, PendingEnd> g_pendingEnd;
constexpr uint64_t kPendingEndMs = 30000;

// The drained batch, kept across ticks for its capacity: the session swaps its buffer with this
// one, so the steady state of a drag allocates nothing on either thread. Game thread only.
std::vector<coop::net::PropPoseSnapshot> g_batch;

// Newer-than on the wrapping byte the stream carries.
bool GenAfter(uint8_t a, uint8_t b) { return static_cast<int8_t>(a - b) > 0; }

int LocalSlot(const coop::net::Session& s) {
    return s.role() == coop::net::Role::Host ? 0 : static_cast<int>(coop::players::Registry::Get().LocalPeerId());
}

// Two claims of one prop under the same generation crossed: the same coin on every peer, from the eid and
// both slots. The winning slot.
int CoinOwner(uint32_t eid, int slotA, int slotB) {
    if (slotA > slotB) std::swap(slotA, slotB);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 4; ++i) { h ^= (eid >> (8 * i)) & 0xFF; h *= 0x100000001b3ULL; }
    h ^= static_cast<uint8_t>(slotA); h *= 0x100000001b3ULL;
    h ^= static_cast<uint8_t>(slotB); h *= 0x100000001b3ULL;
    return ((h >> 17) & 1) ? slotB : slotA;
}

void AdvanceGen(std::unordered_map<uint32_t, Closed>& m, uint32_t eid, uint8_t gen);

// A stream from `owner` under `gen` for a prop this peer may stream itself: true when that stream is the
// prop's motion from now on -- this peer's own row, if any, yields to it. The losing generation is closed
// here, so its late datagrams start nothing.
bool TheirsWins(uint32_t eid, uint8_t gen, int owner, int me) {
    uint8_t mine = 0;
    if (!coop::prop_drive_host::Owns(eid, &mine)) return true;
    const bool theirs = GenAfter(gen, mine) || (gen == mine && CoinOwner(eid, me, owner) == owner);
    if (theirs) coop::prop_drive_host::GiveUp(eid);
    else AdvanceGen(g_endedGen, eid, gen);
    return theirs;
}

// A frozen or static prop keeps its physics off whatever the stream did: it is moved kinematically
// and left so, the stuck wall-attachable rule the held-prop receiver applies.
bool PhysicsStaysOff(void* actor) { return PR::IsFrozen(actor) || PR::IsStatic(actor); }

// By eid first -- the registry's O(1) row, which a joiner's keyed copy carries from the bind --
// then the key INDEX; never the cold object-array walk the general key resolver falls back to.
void* Resolve(uint32_t eid, const coop::net::WireKey& key) {
    if (void* a = coop::remote_prop::ResolveLiveActorByEid(eid)) return a;
    if (key.len == 0) return nullptr;
    return coop::prop_key_index::Find(coop::remote_prop::KeyToWString(key));
}

void GiveBackPhysics(Drive& dr, void* actor) {
    if (!dr.physicsParked || !actor) return;
    const bool simulate = !PhysicsStaysOff(actor);
    coop::prop_park::Unpark(actor, dr.d.mesh, simulate);
    if (simulate) dr.physicsParked = false;
}

// The one way a stream gives a prop up to a hand, from the tick, the first pose and the end edge
// alike: this player's own hand gets back the physics the park took, which no peer's release will
// restore, and the generation is closed as superseded, so neither a pose nor the end of it can
// write its stale pose once the hand lets go.
bool IsOwnHand(void* actor) {
    return actor == coop::local_streams::LastHeldActor() || coop::hand_item::IsHandAxisActor(actor);
}
// Both records only ever move forward while fresh. The tick can yield an OLDER generation's row in the
// same pass that the first pose yielded a newer one, and writing the older one back reopened the newer
// generation: its end then found no yield and parked the stale rest pose after the hand let go.
void AdvanceGen(std::unordered_map<uint32_t, Closed>& m, uint32_t eid, uint8_t gen) {
    const uint64_t now = AD::NowMs();
    auto it = m.find(eid);
    if (it == m.end()) m.emplace(eid, Closed{gen, now});
    else if (GenAfter(gen, it->second.gen) || now - it->second.atMs >= kClosedMs) it->second = Closed{gen, now};
}
// `gen` is at or before a generation closed here within the last few seconds.
bool ClosedAtOrAfter(const std::unordered_map<uint32_t, Closed>& m, uint32_t eid, uint8_t gen) {
    auto it = m.find(eid);
    return it != m.end() && !GenAfter(gen, it->second.gen) && AD::NowMs() - it->second.atMs < kClosedMs;
}

// A hand owns the prop over the stream from `owner`. The owner's own hold still parked here is no hand: its
// release, which says the stream follows, is still in flight, so that hold yields to the stream.
bool HandOwnsOver(void* actor, int owner) {
    if (IsOwnHand(actor)) return true;
    const int held = coop::remote_prop::DrivingSlot(actor);
    if (held < 0) return false;
    if (held == owner) { coop::remote_prop::YieldSlotToStream(held); return false; }
    return true;
}
void YieldToHand(uint32_t eid, uint8_t gen, Drive* dr, void* actor) {
    if (dr && actor && IsOwnHand(actor)) GiveBackPhysics(*dr, actor);
    AdvanceGen(g_endedGen, eid, gen);
    AdvanceGen(g_yieldedGen, eid, gen);
}
bool YieldedAtOrAfter(uint32_t eid, uint8_t gen) { return ClosedAtOrAfter(g_yieldedGen, eid, gen); }

// The last stream ends applied here, a ring of kRecentEnds (prop_drive_stream.h).
AppliedEnd g_ends[kRecentEnds];
size_t g_endCount = 0;   // ends recorded since the session began; the newest is at (g_endCount - 1) % kRecentEnds

// The end edge on a live prop: the final pose, then the host's physics flags through the same
// parity restore the join's converge applies after its teleport, then the velocity -- only into
// a body this module parked and the flags leave simulating, and only when it is not zero, since
// assigning a velocity wakes a body at rest.
// What another peer's end just set down here, and when: its copy settles against its neighbours for a moment,
// and those contacts are that peer's settling, not a knock of this one's (coop/props/knock_stream).
std::unordered_map<void*, uint64_t> g_settledAt;
constexpr uint64_t kSettleQuietMs = 1500;

void ApplyEnd(void* actor, const coop::net::PropDriveEndPayload& p, bool parkedHere) {
    g_settledAt[actor] = AD::NowMs();
    E::SetActorLocation(actor, ue_wrap::FVector{p.x, p.y, p.z});
    g_ends[g_endCount++ % kRecentEnds] = AppliedEnd{p.eid, ue_wrap::FVector{p.x, p.y, p.z}, ::GetTickCount64()};
    E::SetActorRotation(actor, ue_wrap::FRotator{p.pitch, p.yaw, p.roll});
    // The host's frozen and sleep at the end first: a hook that bites a frozen prop unfreezes it on
    // the host only (hook_C's attach_a runs setPropProps), so a copy still frozen here takes the
    // host's flags before the physics decision reads them.
    coop::prop_wire_parity::ConvergeFrozenSleep(actor, p.physFlags);
    if (!PhysicsStaysOff(actor)) {
        coop::prop_wire_parity::RestoreSpParityPhysicsAfterConverge(actor, p.physFlags);
        const float lin2 = p.linVelX * p.linVelX + p.linVelY * p.linVelY + p.linVelZ * p.linVelZ;
        if (parkedHere && coop::prop_wire_parity::SpParitySimulate(p.physFlags) && lin2 > 0.f) {
            void* mesh = PR::GetStaticMesh(actor);
            ue_wrap::engine::SetComponentLinearVelocity(mesh, p.linVelX, p.linVelY, p.linVelZ);
            ue_wrap::engine::SetComponentAngularVelocity(mesh, p.angVelX, p.angVelY, p.angVelZ);
        }
    }
    // The park is over, whatever the flags left the body: a Character it stopped moves again.
    coop::prop_park::Unpark(actor, /*mesh=*/nullptr, /*rootSimulates=*/false);
}

}  // namespace

size_t RecentEnds(AppliedEnd* out, size_t max) {
    const size_t have = g_endCount < kRecentEnds ? g_endCount : kRecentEnds;
    size_t n = 0;
    for (; n < have && n < max; ++n) out[n] = g_ends[(g_endCount - 1 - n) % kRecentEnds];
    return n;
}

void TickApplyAndDrive(coop::net::Session& s) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::TickApplyAndDrive");
    const uint64_t nowMs = AD::NowMs();
    const int me = LocalSlot(s);
    g_batch.clear();
    if (s.TakeRemotePropDriveBatch(g_batch)) {
        for (const coop::net::PropPoseSnapshot& e : g_batch) {
            const uint32_t eid = e.elementId;
            if (eid == 0) continue;
            const int owner = coop::net::PropDriveOwnerOf(e);
            if (owner == me) continue;
            coop::prop_drive_host::NoteSeenGen(eid, e.ctx);
            if (!TheirsWins(eid, e.ctx, owner, me)) continue;   // this peer's newer claim moves it
            // The generation gate: a pose of a closed generation, or older than the live drive's,
            // is a datagram that outlived its stream.
            if (ClosedAtOrAfter(g_endedGen, eid, e.ctx)) continue;
            auto it = g_drives.find(eid);
            if (it != g_drives.end() && !it->second.placeholder) {
                if (GenAfter(it->second.gen, e.ctx)) continue;
                if (it->second.gen == e.ctx) {
                    // Two claims under one generation crossed: the coin every peer flips alike keeps one.
                    if (it->second.owner != owner) {
                        if (CoinOwner(eid, it->second.owner, owner) != owner) continue;
                        it->second.owner = owner;
                    }
                    // The steady state: the row already names the actor. No resolve, no string.
                    if (void* live = it->second.d.LiveActor()) {
                        if (!HandOwnsOver(live, owner))   // a hand's stream wins
                            AD::BeginLerpToPose(it->second.d, ue_wrap::FVector{e.x, e.y, e.z},
                                                ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
                        continue;
                    }
                }
            }
            // A first pose, a new generation, or a row whose actor died: resolve, on a throttle.
            auto miss = g_miss.find(eid);
            if (miss != g_miss.end() && nowMs < miss->second.nextTryMs) continue;
            void* actor = Resolve(eid, e.key);
            if (!actor) {
                Miss& m = g_miss[eid];
                m.nextTryMs = nowMs + kMissRetryMs;
                if (++m.count == kMissSaidAt)
                    UE_LOGI("[PROP-DRIVE] CLIENT eid=%u has not resolved in %u tries -- the host is "
                            "streaming a prop this peer does not have (a join still landing, or a "
                            "prop it never received)", eid, m.count);
                continue;
            }
            g_miss.erase(eid);
            if (!PR::IsDescendantOfProp(actor)) continue;
            // A hand's stream on the same prop wins: the holder is its syncer for as long as it
            // holds it, and the host closes this stream the moment it sees the hand.
            if (HandOwnsOver(actor, owner)) {
                // The first pose found the prop already in a hand: the generation is the hand's.
                YieldToHand(eid, e.ctx, nullptr, actor);
                UE_LOGI("[PROP-DRIVE] CLIENT eid=%u gen=%u -- a hand holds it at the first pose; "
                        "generation yielded", eid, static_cast<unsigned>(e.ctx));
                continue;
            }
            Drive& dr = g_drives[eid];
            if (dr.d.actor && dr.d.actor != actor) GiveBackPhysics(dr, dr.d.LiveActor());
            AD::ResetDriveState(dr.d);
            dr.d.actor    = actor;
            dr.d.actorIdx = R::InternalIndexOf(actor);
            dr.d.mesh     = PR::GetStaticMesh(actor);
            dr.d.isTrashMirror = true;  // the fixed-delay follow of a carried clump: freeze on a gap
            dr.d.lastEid  = eid;
            dr.gen        = e.ctx;
            dr.owner      = owner;
            dr.placeholder = false;
            dr.grabUntilMs = 0;
            dr.physicsParked = false;
            dr.relatches  = 0;
            if (dr.d.mesh && !PhysicsStaysOff(actor)) {
                coop::prop_park::Park(actor, dr.d.mesh);
                dr.physicsParked = true;
            }
            g_endedGen.erase(eid);
            g_yieldedGen.erase(eid);
            // A pose of a closed generation never reaches here, so a pending end for this prop is
            // of an OLDER generation than the one parking now: stale, and dropped before it can
            // land on the new stream.
            g_pendingEnd.erase(eid);
            UE_LOGI("[PROP-DRIVE] CLIENT park eid=%u gen=%u actor=%p (physics %s)",
                    eid, static_cast<unsigned>(e.ctx), actor,
                    dr.physicsParked ? "off" : "left as it was");
            AD::BeginLerpToPose(dr.d, ue_wrap::FVector{e.x, e.y, e.z},
                                ue_wrap::FRotator{e.pitch, e.yaw, e.roll}, nowMs);
        }
    }
    for (auto it = g_drives.begin(); it != g_drives.end();) {
        Drive& dr = it->second;
        void* actor = dr.d.LiveActor();
        if (!actor) { it = g_drives.erase(it); continue; }   // the destroy crossed on its own seam
        if (dr.placeholder && nowMs >= dr.placeholderUntilMs) {
            // The stream the release promised never came: the copy goes back to its own physics.
            GiveBackPhysics(dr, actor);
            UE_LOGI("[PROP-DRIVE] eid=%u -- the stream slot %d's release promised never came; physics back",
                    it->first, dr.owner);
            it = g_drives.erase(it);
            continue;
        }
        if (dr.placeholder ? IsOwnHand(actor) : HandOwnsOver(actor, dr.owner)) {
            // A hand took it before the end edge landed: the holder's drive owns the actor from here,
            // and its release restores the physics. This drive yields for good rather than writing
            // a stale target back once the hand lets go.
            const bool ownHand = IsOwnHand(actor);
            YieldToHand(it->first, dr.gen, &dr, actor);
            UE_LOGI("[PROP-DRIVE] CLIENT yield eid=%u -- %s owns the actor", it->first,
                    ownHand ? "this player's hand" : "a held-prop stream");
            it = g_drives.erase(it);
            continue;
        }
        if (dr.grabUntilMs) {
            // This player reached for it: its physics stay on for the grab; a reach that grabbed nothing
            // parks it again.
            if (nowMs < dr.grabUntilMs) { ++it; continue; }
            dr.grabUntilMs = 0;
            if (dr.d.mesh && !PhysicsStaysOff(actor)) {
                coop::prop_park::Park(actor, dr.d.mesh);
                dr.physicsParked = true;
            }
        }
        if (dr.placeholder) { ++it; continue; }
        // The physics receiver's re-latch, as the held-prop drive does it: a parked copy stays
        // kinematic when the game here switches its simulation back on. That includes a copy parked
        // frozen, which the park left as it was, and which an unstick or an unfreeze here has since
        // freed: it is parked kinematic from then on, and the end edge hands its physics back.
        if (dr.d.mesh && ue_wrap::engine::IsComponentSimulatingPhysics(dr.d.mesh)) {
            coop::prop_park::Park(actor, dr.d.mesh);
            dr.physicsParked = true;
            if (++dr.relatches == 1) {
                UE_LOGI("[PROP-DRIVE] CLIENT eid=%u -- the game turned simulation back on under the park; "
                        "re-latched kinematic", it->first);
            } else if (dr.relatches == kRelatchLoud) {
                UE_LOGW("[PROP-DRIVE] CLIENT eid=%u -- re-latched %u times: something here keeps switching the "
                        "parked copy's simulation back on", it->first, static_cast<unsigned>(dr.relatches));
            }
        }
        AD::AdvanceLerp(dr.d, nowMs);
        ++it;
    }
    // An end edge that arrived before its prop: applied the moment the prop resolves.
    for (auto it = g_pendingEnd.begin(); it != g_pendingEnd.end();) {
        if (nowMs >= it->second.expiresMs || g_drives.count(it->first)) {
            it = g_pendingEnd.erase(it);   // expired, or a newer stream parked the prop first
            continue;
        }
        void* actor = coop::remote_prop::ResolveLiveActorByEid(it->first);
        if (!actor) { ++it; continue; }
        if (PR::IsDescendantOfProp(actor) && !HandOwns(actor) && !YieldedAtOrAfter(it->first, it->second.p.gen)) {
            ApplyEnd(actor, it->second.p, /*parkedHere=*/false);
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) applied late (the "
                    "prop arrived after its stream ended)", it->first,
                    static_cast<unsigned>(it->second.p.gen), it->second.p.x, it->second.p.y,
                    it->second.p.z);
        }
        it = g_pendingEnd.erase(it);
    }
}

void OnEnd(const coop::net::PropDriveEndPayload& p, int senderSlot) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnEnd");
    coop::prop_drive_host::NoteSeenGen(p.eid, p.gen);
    // The end of a stream this peer's own newer claim already took over closes nothing here.
    uint8_t mine = 0;
    if (coop::prop_drive_host::Owns(p.eid, &mine) && !GenAfter(p.gen, mine)) {
        AdvanceGen(g_endedGen, p.eid, p.gen);
        return;
    }
    if (coop::prop_drive_host::Owns(p.eid)) coop::prop_drive_host::GiveUp(p.eid);
    auto it = g_drives.find(p.eid);
    // Of two claims under one generation that crossed, the coin's loser's end closes nothing.
    if (it != g_drives.end() && !it->second.placeholder && it->second.gen == p.gen && it->second.owner >= 0 &&
        senderSlot != it->second.owner)
        return;
    if (it == g_drives.end()) {
        // Never parked here. The generation still closes, so a pose of it in flight cannot park
        // the prop now; the rest pose is applied if the prop is here, kept for it if not, and
        // dropped for a prop a hand holds -- that stream owns it -- or one a hand took this generation
        // over from: the hand moved it since, so this pose is older than where it lies. An end older than
        // the generation already closed here is stale outright and touches nothing.
        if (auto closed = g_endedGen.find(p.eid); closed != g_endedGen.end() && GenAfter(closed->second.gen, p.gen) &&
                                                   AD::NowMs() - closed->second.atMs < kClosedMs)
            return;
        if (YieldedAtOrAfter(p.eid, p.gen)) {
            AdvanceGen(g_endedGen, p.eid, p.gen);
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -- a hand took this generation over; "
                    "closed, pose not applied", p.eid, static_cast<unsigned>(p.gen));
            return;
        }
        AdvanceGen(g_endedGen, p.eid, p.gen);
        void* actor = coop::remote_prop::ResolveLiveActorByEid(p.eid);
        if (actor) {
            if (PR::IsDescendantOfProp(actor) && !HandOwns(actor)) {
                ApplyEnd(actor, p, /*parkedHere=*/false);
                UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) (never parked here)",
                        p.eid, static_cast<unsigned>(p.gen), p.x, p.y, p.z);
            }
        } else {
            g_pendingEnd[p.eid] = PendingEnd{p, AD::NowMs() + kPendingEndMs};
        }
        return;
    }
    Drive& dr = it->second;
    if (!dr.placeholder && GenAfter(dr.gen, p.gen)) return;   // an end for an older generation than the live drive
    if (void* actor = dr.d.LiveActor()) {
        if (dr.placeholder ? IsOwnHand(actor) : HandOwnsOver(actor, dr.owner)) {
            // A hand took it before the tick yielded: the same yield, so this player's own hand gets
            // the physics back. A peer's held-prop stream parked it after ours did and its release
            // hands the physics back; the pose is left alone either way.
            YieldToHand(p.eid, p.gen, &dr, actor);
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -- a held-prop stream owns the actor; "
                    "row dropped, nothing touched", p.eid, static_cast<unsigned>(p.gen));
        } else {
            ApplyEnd(actor, p, dr.physicsParked);
            dr.physicsParked = false;
            UE_LOGI("[PROP-DRIVE] CLIENT END eid=%u gen=%u -> (%.1f,%.1f,%.1f) flags=0x%02x",
                    p.eid, static_cast<unsigned>(p.gen), p.x, p.y, p.z,
                    static_cast<unsigned>(p.physFlags));
        }
    }
    g_drives.erase(it);
    AdvanceGen(g_endedGen, p.eid, p.gen);
}

namespace {

UE_FACT_FN(kRowUseInput, "prop_drive_stream", L"mainPlayer_C", L"InpActEvt_use_K2Node_InputActionEvent_41", Declared);
bool g_grabYieldInstalled = false;
uint64_t g_grabYields = 0;

// This player pressed use: a prop this stream parks under the crosshair gets its physics back first, so the
// game's grab, which takes only a simulating body, takes it. The drive goes; the generation stays open, so a
// grab that does not happen leaves the next pose to park it again and the end edge to land it, and one that
// does is yielded to the hand at the next pose or end, as the tick yields one.
void OnUseInputPre(void* self, void* /*function*/, void* /*params*/) {
    if (!self) return;
    // A press with something already in the hand drops it; it grabs nothing.
    if (coop::local_streams::LastHeldActor()) return;
    void* target = ue_wrap::engine::ReadMainPlayerLookAtActor(self);
    if (!target) return;
    // Another player's coast is handed over the same way (remote_prop), and another peer's stream below.
    if (coop::remote_prop::YieldCoastForGrab(target)) return;
    if (g_drives.empty()) return;
    for (auto it = g_drives.begin(); it != g_drives.end(); ++it) {
        if (it->second.d.LiveActor() != target) continue;
        // The row stays: a grab yields it at the next tick, and a reach that grabbed nothing parks the copy
        // again when the window closes, so it never drifts on its own here.
        GiveBackPhysics(it->second, target);
        it->second.grabUntilMs = AD::NowMs() + kGrabWindowMs;
        ++g_grabYields;
        UE_LOGI("[PROP-DRIVE] eid=%u gen=%u -- this player reaches for it mid-stream; physics back for the grab",
                it->first, static_cast<unsigned>(it->second.gen));
        return;
    }
}

}  // namespace

void InstallGrabYield() {
    if (g_grabYieldInstalled) return;
    const ue_wrap::facts::Resolved fn = ue_wrap::facts::Resolve(kRowUseInput);
    if (!fn) return;   // mainPlayer_C not loaded yet: asked again next tick
    if (!ue_wrap::game_thread::RegisterPreObserver(fn, &OnUseInputPre)) {
        UE_LOGW("prop_drive_stream: the use-press observer did not register -- a prop under the host's stream "
                "stays ungrabbable here until it rests");
        g_grabYieldInstalled = true;
        return;
    }
    g_grabYieldInstalled = true;
    UE_LOGI("prop_drive_stream: the use-press observer is live -- a prop still moving on the host is grabbable here");
}

void ExpectStream(void* actor, uint32_t eid, int ownerSlot) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::ExpectStream");
    if (!actor || eid == 0 || g_drives.count(eid)) return;
    Drive& dr = g_drives[eid];
    dr.d.actor    = actor;
    dr.d.actorIdx = R::InternalIndexOf(actor);
    dr.d.mesh     = PR::GetStaticMesh(actor);
    dr.d.isTrashMirror = true;
    dr.d.lastEid  = eid;
    dr.owner      = ownerSlot;
    dr.placeholder = true;
    dr.placeholderUntilMs = AD::NowMs() + kPlaceholderMs;
    if (dr.d.mesh && !PhysicsStaysOff(actor)) {
        coop::prop_park::Park(actor, dr.d.mesh);
        dr.physicsParked = true;
    }
}

void NoteOwnClaim(uint32_t eid, uint8_t gen) {
    // Every older stream of the prop is over here: its late datagrams start nothing.
    if (eid) AdvanceGen(g_endedGen, eid, static_cast<uint8_t>(gen - 1));
}

void OnPeerGone(int slot) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::OnPeerGone");
    for (auto it = g_drives.begin(); it != g_drives.end();) {
        if (it->second.owner != slot) { ++it; continue; }
        GiveBackPhysics(it->second, it->second.d.LiveActor());
        UE_LOGI("[PROP-DRIVE] eid=%u -- slot %d left mid-stream; the copy takes its own physics back", it->first, slot);
        it = g_drives.erase(it);
    }
}

bool SettledByPeerRecently(void* actor) {
    if (!actor) return false;
    auto it = g_settledAt.find(actor);
    if (it == g_settledAt.end()) return false;
    if (AD::NowMs() - it->second < kSettleQuietMs) return true;
    g_settledAt.erase(it);
    return false;
}

bool IsParked(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_drive_stream::IsParked");
    if (!actor) return false;
    // The live actor, not the cached pointer: a row whose actor died is erased only on the next tick,
    // and a new actor at the freed address is not parked.
    for (const auto& kv : g_drives)
        if (kv.second.d.LiveActor() == actor) return true;
    return false;
}

void OnDisconnect() {
    for (auto& kv : g_drives) GiveBackPhysics(kv.second, kv.second.d.LiveActor());
    g_drives.clear();
    g_settledAt.clear();
    g_endedGen.clear();
    g_yieldedGen.clear();
    g_miss.clear();
    g_pendingEnd.clear();
    g_endCount = 0;
    g_batch.clear();
}

}  // namespace coop::prop_drive_stream
