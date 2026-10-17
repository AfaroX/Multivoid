// coop/items/shovel_scrap.h -- the scrap a client's shovel makes reaches the host.
//
// The shovel is the player's: its swing's notify calls a chip pile's turnIntoScrap, which spawns the
// pile's scrap and destroys the pile, and the next swing calls the player's throwShovelItem, which
// spawns what the shovel carries and launches it. On a client both run on the client's own world, and
// a client's fresh prop spawn is never broadcast, so the pile's destroy crossed and its scrap did not:
// the host saw a pile vanish and no junk.
//
// This lane marks the births inside those two bodies, as the container extraction marks its own
// (coop/props/container_contents_sync), and the place-detection drain admits a marked birth as an
// ordinary drop intent: the host spawns it, the thrower's flight follows on the same FIFO, and the
// host's spawn binds this client's copy (coop/props/prop_drop_intent). A birth that does not outlive
// the next drain -- one the shovel takes straight into its load -- dies before it is admitted.
// Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::shovel_scrap {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the two watches. Game thread, per pump tick.
void Tick();

// True while one of the two bodies is running on a client: a birth seen now is the shovel's.
// Game thread.
bool InFlight();

// Session teardown: the depth and the counts are dropped.
void OnDisconnect();

}  // namespace coop::shovel_scrap
