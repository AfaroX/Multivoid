// coop/media/media_playback.h -- a TV or a radio plays the same thing, at the same point, on every screen.
//
// Every TV and the internet radio play a link through openLink on their own media player
// (ue_wrap/devices/media_device), and pick what plays next in `fin` with their own random roll, so each
// peer's TV ran its own channel. Here the host's copy of a device is the one that plays: a client's
// openLink is refused on its copy and sent to the host as an intent (MediaPlay), the host runs it, and
// the host sends each device's link, play state and position -- on every change, every two seconds
// while it plays, and to a joiner at its world-ready. A client opens the same link, a local file named
// relative to its own asset folder (the pool puts the same file there under the same name), and seeks
// when it drifts past two seconds; a client's `fin` is refused, the host's choice coming instead. A
// pause, a resume or a scrub a client's player makes on its own copy is sent to the host the same way,
// and a scrub on the host's goes out at once. A played file not yet pooled is shared, and every other
// peer opens it once the pool has brought it. A client's own pick plays on its screen at once, reports
// its position, and the host's copy joins it there before the host sends anything about it.
// Gameplay/network layer (principle 7). Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::media_playback {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Per pump tick: the watches, the host's polls and sends, a client's applies. Game thread.
void Tick();

// A MediaPlay chunk. Game thread.
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: a joiner's world is up -- every device that has a link.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnPeerGone(uint8_t slot);
void OnDisconnect();

}  // namespace coop::media_playback
