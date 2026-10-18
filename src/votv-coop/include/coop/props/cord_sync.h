// coop/props/cord_sync.h -- an item's power cord is in the same outlet on every screen.
//
// A corded item (prop_corded_C: a TV, a monitor, a lamp) spawns its cord (cord_C) as a child actor, and
// the cord's own verbs plug it: `connect(socket, load)` puts its head in a cordSocket_C and powers the
// item through the outlet's cordPlugged, and `disconnect()` takes it out. A child actor has no identity
// of its own on the wire, so each player's plug stayed on their screen, and the item went unpowered on
// every other. Here a player's connect and disconnect cross: the item by its key, the outlet by its
// portable key (coop/element/portable_identity) with its location as the fallback, and every other peer
// runs the same verb on its copy. While a player drags a cord, its head's pose streams at about 15 Hz, so
// every screen shows the cord running from the item to that player's hand until it goes into an outlet or
// back onto the item. A cord stretched too far comes out on the host's copy, which sends it; a client's own
// tick leaves it in. The game's load-time reconnect and a dying cord's disconnect stay
// local. The host relays a client's plug and sends every corded item's cord to a joiner.
// Wire: CordPlug. Gameplay/network layer (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct CordPlugPayload;
}  // namespace coop::net

namespace coop::cord_sync {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watches; a joiner re-asserts what the host sent while its load settles.
// Game thread, per pump tick.
void Tick();

// A peer's plug or unplug: applied to this copy; the host relays it. Game thread.
void OnCordPlug(coop::net::Session& session, const coop::net::CordPlugPayload& payload, uint8_t senderSlot);

// HOST: a joiner's world is up -- every corded item's cord, in or out.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::cord_sync
