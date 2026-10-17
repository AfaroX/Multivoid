// coop/interactables/drone_sack_intent.h -- the drone's drop-sack action, as a client's intent the
// host performs. Overview: docs/devices.md. Gameplay/network layer (principle 7); the engine is
// reached through ue_wrap::drone.
//
// The drone is host-simulated and every client's copy is a mirror. Its drop-sack action ran on the
// presser's own copy: on a client it spawned a sack there and cleared that copy's hasSack, while
// the host's drone kept its sack. The client's sack crossed as an untracked held item and opened the
// drone's inventory from anywhere, a second bag beside the one the drone still carried.
//
// This lane refuses the client's own drone_C::dropSack at the script-body gate and sends the press
// on the drone lane's intent kind. The drone is a singleton both peers load, so it is not named;
// the host tests the sender's reach to its own drone and that the drone carries a sack, then runs
// the same verb, whose sack reaches every peer as a host birth and whose hasSack the drone's state
// stream already carries. Nothing to replay on a late join: a drop the host has not run changed
// nothing.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drone_sack_intent {

// Watch the drone's drop-sack verb (client) and cache the session. Idempotent; retried until the
// gate's name resolves. Game thread.
void Install(coop::net::Session* session);

// Per pump tick: settle the watch; run one queued drop per peer per token (host). Game thread.
void Tick(coop::net::Session& session);

// HOST: a client asks for the drop. Queued here and performed from Tick under the rate limit.
void OnDropSackIntent(coop::net::Session& session, uint8_t senderSlot);

// Session teardown: the counts are dropped; each sender's queue clears with its occupant (PerSlotState).
void OnDisconnect();

}  // namespace coop::drone_sack_intent
