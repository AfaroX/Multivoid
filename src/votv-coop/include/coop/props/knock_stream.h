// coop/props/knock_stream.h -- a prop the host's world knocks moving streams until it rests.
//
// The driven-prop channel streams a prop only when a verb claims it: a hook, a broom, a host release,
// a rupture. A knock is no verb, so a prop another prop struck moved on each peer by that peer's own
// physics and came to rest wherever each one put it ("a knock is none", docs/props.md).
//
// Every prop carries comp_physicsImpact, and every contact it reports goes through the component's
// hitCpp. On the host, after it runs, a prop that is now simulating and moving faster than a crawl is
// put on the driven-prop channel, which streams its pose until it rests and ends with the host's
// pose, so every peer's copy comes to rest where the host's did. A client needs nothing: its copy
// follows the stream. A carried prop pushes host props too, now that a held copy moves rather than
// teleports (coop/props/active_drive), so a client's knock is a host knock as well.
// Gameplay layer (principle 7). Game thread.

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
