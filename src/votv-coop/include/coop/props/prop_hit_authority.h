// coop/props/prop_hit_authority.h -- one copy of a prop answers a hit on it: the holder's or the host's.
//
// Every machine runs every prop's whole Blueprint, so a knock or a swing that reached a prop ran its
// damage on whichever copy felt it: a client's copy of a prop at rest broke, opened or burst there
// alone, and the host's copy of a prop a client carries felt its knocks a second time. The mod refused
// this one class at a time (the extinguisher's rupture, a puppet's impact damage).
//
// Here the rule is on the base entries every prop's damage goes through -- addDamage, the swing's and an
// explosion's, and receivedPhyiscsDamage and impactDamageCPP, the physics contact's: they run only on
// the copy answerable for the prop. While a player holds it that is the holder's copy; otherwise it is
// the host's. A client's swing at a prop it does not hold is not lost: it is refused on the client's
// copy and sent to the host (PropHitIntent), which runs the same addDamage on its copy within the
// sender's reach. What the damage changes crosses on the prop's record (coop/props/record_sync), its
// stream (coop/props/knock_stream) or its destroy. `[dev] prop_hit_authority_off=1` turns the rule off.
// Gameplay/network layer (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropHitIntentPayload;
}  // namespace coop::net

namespace coop::props::prop_hit_authority {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watches. Game thread, per pump tick.
void Tick();

// HOST: a client's player hit a prop. Resolved by key, eid as the keyless fallback. Game thread.
void OnHitIntent(coop::net::Session& session, const coop::net::PropHitIntentPayload& payload, uint8_t senderSlot);

// Session teardown: the counts are dropped.
void OnDisconnect();

}  // namespace coop::props::prop_hit_authority
