// coop/props/prop_drive_stream.h -- CLIENT-side apply of the host's driven-prop stream.
//
// The host publishes the pose of every prop under an external drive -- a hook's constraint, a
// body still sliding after the hook let go -- on MsgType::PropDrivePose, and closes each prop's
// stream with a reliable PropDriveEnd. This module parks the local copy on the first pose, its mesh
// stops simulating so physics leaves it alone, and drives it through the same fixed-delay snapshot
// interpolation the held-prop and trash-carry receivers use (coop/props/active_drive.h), keyed by
// eid. The end edge snaps the final pose, hands the prop its physics and velocity back, and closes
// the generation so a pose still in flight cannot park it again.
//
// CLIENT-side, game thread only. Not folded into remote_prop, the per-slot held-prop receiver: a
// prop here is in nobody's hand, and when a hand takes it that stream wins.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <cstdint>

namespace coop::net {
class Session;
struct PropDriveEndPayload;
}  // namespace coop::net

namespace coop::prop_drive_stream {

// Any peer's game tick: drain what the other peers streamed, park and drive each prop, advance every live
// drive (a gap freezes at the last pose). A stream of a prop this peer owns is followed only when its claim
// is newer, and this peer's own row yields to it; of two claims that crossed, the coin picks.
void TickApplyAndDrive(coop::net::Session& s);

// Game thread: the peer in `senderSlot` closed a prop's stream. Validated at the router.
void OnEnd(const coop::net::PropDriveEndPayload& p, int senderSlot);

// True while this stream has `actor` parked: its pose is the host's stream's, so a converge that
// would move it or re-run its init() leaves it alone, as it leaves a held prop. Game thread.
bool IsParked(void* actor);

// Register the use-press observer that hands a prop still under the host's stream to this player's grab.
// Idempotent; retried until mainPlayer_C is loaded. Game thread.
void InstallGrabYield();

// A holder's release said it streams the prop's flight: the copy is parked now, before the first pose, so it
// does not fly here on its own; a placeholder that no pose follows gives the physics back. Game thread.
void ExpectStream(void* actor, uint32_t eid, int ownerSlot);

// This peer just claimed `eid` under `gen`: the older streams of it are closed here. Game thread.
void NoteOwnClaim(uint32_t eid, uint8_t gen);

// A peer left: the props it streamed get their physics back here. Game thread.
void OnPeerGone(int slot);

// Session end: every driven prop gets its physics back. Game thread.
void OnDisconnect();

// The stream ends applied here lately, newest first, at most `max` of the last kRecentEnds: each eid,
// the host's final pose and the tick it landed (GetTickCount64). Cleared with the session. For the fall
// drill's verdict, which polls it at 4 Hz: an end pushed out between two polls is lost to it. Game thread.
inline constexpr size_t kRecentEnds = 8;
struct AppliedEnd {
    uint32_t eid = 0;
    ue_wrap::FVector pose{};
    uint64_t atMs = 0;
};
size_t RecentEnds(AppliedEnd* out, size_t max);

}  // namespace coop::prop_drive_stream
