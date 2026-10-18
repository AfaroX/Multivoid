// coop/media/media_playback.h -- a TV or a radio plays what its last user set, on every screen.
//
// Every TV and the internet radio play a link through openLink on their own media player
// (ue_wrap/devices/media_device), and roll what plays next in `fin`, so each peer's TV ran its own channel.
// Here each device has an owner: the player who used it last. The owner's copy plays -- its picks, its
// pauses, its scrubs, its volume, its brightness, its next-track roll -- and the owner sends the device's
// state; every other peer, the host included, follows it: it opens the same link (a local file named
// relative to its own asset folder, which the media pool fills), follows the play state, seeks past two
// seconds of drift, and takes the volume and the brightness. Only one player uses a device at a time, as
// the base laptop: opening its interface asks the host for it, and a device in another player's hands
// refuses the interface with a note in the chat. The host keeps the owner and the holder, relays an owning
// client's state to the others, and sends every device's last state to a joiner.
// Wire: MediaPlay. Gameplay/network layer (principle 7). Game thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::media_playback {

// Cache the session; the watches register from Tick. Idempotent.
void Install(coop::net::Session* session);

// Per pump tick: the watches, the owner's polls and sends, a follower's applies. Game thread.
void Tick();

// A MediaPlay chunk. Game thread.
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: a joiner's world is up -- every device that has a state.
void QueueConnectBroadcastForSlot(int peerSlot);

// A peer left: the devices it held are free, and the host takes the ones it owned.
void OnPeerGone(uint8_t slot);
void OnDisconnect();

}  // namespace coop::media_playback
