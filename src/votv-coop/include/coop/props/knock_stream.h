// coop/props/knock_stream.h -- a prop knocked moving streams from whoever knocked it until it rests.
//
// A knock is no verb, so a prop another prop struck once moved on each peer by that peer's own physics
// and came to rest wherever each one put it. Every prop carries comp_physicsImpact, and every contact it
// reports goes through the component's hitCpp(HitComponent, OtherActor, ...). After it runs, a prop now
// simulating faster than a crawl goes on this peer's driven-prop channel (coop/props/prop_drive_host),
// which streams it to rest -- when this peer moved it: the other actor is this player, the prop this
// player holds, or a prop this peer already streams. So a throw knocks a stack over on the thrower's
// machine and the thrower streams every prop of it, as vanilla physics moved them there; every other
// copy follows. The host also takes a knock nobody's player caused. A prop another peer streams is
// parked here and knocks nothing. Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::knock_stream {

// Cache the session; the watch registers from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watch. Game thread, per pump tick.
void Tick();

// Session teardown: the counts are dropped.
void OnDisconnect();

}  // namespace coop::props::knock_stream
