// coop/props/prop_drive_host.h -- this peer's side: the props it set moving, streamed until they rest.
//
// The held-prop stream is sourced from the player's grab slot alone, so a prop a hook pulls, a throw sends
// on, or a knock sets moving had no channel of its own. This is that channel, and any peer runs it: the
// last peer to set a prop moving owns its motion. Its copy simulates; it streams the pose on
// MsgType::PropDrivePose whenever the prop moved, and one reliable PropDriveEnd closes the stream with the
// final pose, its physics flags and the velocity; every other copy follows (coop/props/prop_drive_stream).
// What its owned props knock moves on its machine first, so it claims those too (coop/props/knock_stream).
// A claim takes the generation after every one this peer has seen for the prop, so the newest owner wins
// everywhere; this peer's row yields when another's newer stream arrives. MTA's unoccupied-vehicle syncer,
// with the syncer moving to whoever last touched the vehicle.
//
// Any peer, game thread. Verb-fed and never scanned: the set holds only what a caller claimed.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::prop_drive_host {

// Put `actor`, a keyed Aprop_C descendant with an element id, under this peer's drive. Idempotent while the
// claim holds, so a feeder may call it every pass for every prop it sees tied; a re-claim of a
// coasting prop cancels its end. A prop in a hand -- this player's grab slot, a peer's held-prop
// stream, or the hotbar hand axis -- is refused silently, since the hand lane owns it, and so is a
// class the wire never expresses. `reason` names the verb for the log. Game thread.
void Claim(void* actor, const char* reason);

// How many props coast with no verb's claim. Game thread.
size_t CoastingCount();

// The verb let go of `actor`: the prop coasts under the stream until it rests, then the end edge
// fires. A no-op for an unclaimed actor. Game thread.
void Release(void* actor);

// An instant push moved `actor`: stream it, coasting, until it rests. A prop a verb has claimed
// stays claimed, and one already coasting starts its rest clock again; the refusals are Claim's.
// `reason` names the push for the log. Game thread.
void Coast(void* actor, const char* reason);

// This player just let go of `actor` while it moves: it streams, coasting, until it rests. The release
// edge runs before this player's hand is cleared, so the own-hand refusal does not apply. Game thread.
// `eid` is the id the hold streamed under, which every peer already resolves.
void CoastLetGo(void* actor, uint32_t eid);

// A peer's world just came up: every driven prop's current pose is re-sent on the next tick, so
// the joiner parks the resting ones too, which the delta gate would otherwise never send it.
// Game thread.
void OnPeerWorldReady();

// Per gameplay tick: read every driven prop's pose (resting ones at 4 Hz), publish the ones that
// moved, close the streams of the props that rested or that a hand took, drop the dead. Game
// thread.
void Tick(coop::net::Session& s);

// Whether this peer streams the prop with element id `eid`, and the generation it streams under. Game thread.
bool Owns(uint32_t eid, uint8_t* gen = nullptr);
bool OwnsActor(void* actor);

// Another peer's stream of `eid` arrived under `gen`: a later claim here takes a newer generation. Game
// thread.
void NoteSeenGen(uint32_t eid, uint8_t gen);

// Another peer's newer stream took `eid`: this peer's row stops, sending no end -- the new owner's ends
// it. Game thread.
void Yield(uint32_t eid);

// Session end. Game thread.
void OnDisconnect();

}  // namespace coop::prop_drive_host
