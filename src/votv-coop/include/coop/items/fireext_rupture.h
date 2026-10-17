// coop/items/fireext_rupture.h -- a fire extinguisher's rupture is the host's, and its flight is streamed.
//
// A hard knock reaches prop_fireExt_C's receivedPhyiscsDamage and addDamage, and past a threshold they
// call shoot: the burst that flies the extinguisher, rolled at random and saved as its own record
// (active, life). The knock is felt by the copy that was hit, so a client smacking the one it holds
// ruptured its copy alone; the host's copy, driven kinematic by the client's hold, felt nothing and
// stayed whole. And the host's own rupture flew only on the host: nothing streamed it.
//
// On a client, shoot is refused at the script-body gate; the client lets go of the extinguisher if it
// holds it, and once its release has gone out on the FIFO it names the extinguisher to the host
// (FireExtRupture). The host tests the sender's reach, ends any hold still driving its copy, and runs
// the same verb. Every shoot the host runs -- its own knock or a client's -- puts the extinguisher on
// the driven-prop channel, which streams the flight until it rests (coop/props/prop_drive_host).
// Gameplay/network layer (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct FireExtRupturePayload;
}  // namespace coop::net

namespace coop::fireext_rupture {

// Cache the session; the watch registers from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watch; on a client, send each queued rupture once its release has gone out.
// Game thread, per pump tick.
void Tick();

// HOST: a client's extinguisher ruptured. Resolved by key, eid as the keyless fallback. Game thread.
void OnRupture(coop::net::Session& session, const coop::net::FireExtRupturePayload& payload, uint8_t senderSlot);

// Session teardown: the queue and the counts are dropped.
void OnDisconnect();

}  // namespace coop::fireext_rupture
