// coop/props/wall_poster_sync.h -- a poster put up on a wall is on every wall.
//
// A held poster (prop_poster_C) goes up with its `put`: it spawns the wall poster its `posterClass` names
// (poster_C, an actor_save_C -- a saved world object, not a prop) with its type, index, file name and
// custom flag, at the decal's transform, and destroys itself. The prop's destroy crossed, the wall poster
// never did, so the other player saw the poster vanish from the hand and appear nowhere. Here the wall
// poster's birth inside `put` crosses with its class, its Key, those fields and its transform, and every
// other peer spawns the same one under the same Key; taking it down (its use, which hands the prop back and
// destroys it) and a new picture cross by that Key. A custom picture is shared through the media pool. The
// host relays a client's and sends every wall poster to a joiner, who keeps the ones its save already has.
// Wire: WallPoster. Gameplay/network layer (principle 7). Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct WallPosterPayload;
}  // namespace coop::net

namespace coop::wall_poster_sync {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watches. Game thread, per pump tick.
void Tick();

// A peer's poster went up, came down or changed picture: applied here; the host relays it. Game thread.
void OnWallPoster(coop::net::Session& session, const coop::net::WallPosterPayload& payload, uint8_t senderSlot);

// HOST: a joiner's world is up -- every wall poster.
void QueueConnectBroadcastForSlot(int peerSlot);

// The pool brought pictures: every wall poster looks its picture up again. Game thread.
void RefreshPictures();

void OnDisconnect();

}  // namespace coop::wall_poster_sync
