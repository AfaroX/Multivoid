// coop/props/record_sync.h -- a prop's own save state crosses when it changes, not only at its birth.
//
// prop_save_data carries a prop's record on every birth path and on none other, so a prop whose own
// state changes where it sits -- an extinguisher's rupture, an oven's repair, a printer's progress, a
// case's lid -- changed on the peer that changed it and nowhere else. Two classes had a hand-written
// republish (coop/props/prop_record_refresh); every other class was blind.
//
// This lane republishes any covered prop's record when its state digest changes. The host keeps a
// digest of every covered prop, a bounded slice per frame, and publishes a change to every peer; a
// client keeps one only for what it authors -- the props it holds and has just let go of -- and sends
// a change as the intent the host validates and re-publishes. A record that lands re-bases the
// receiver's digest, so an applied record is never sent back. A prop whose digest moves sweep after
// sweep with no one touching it is a local accumulator (food, curing concrete): those are left to
// their own anchor design, named once per class, rather than streamed.
// Gameplay layer (principle 7). Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::props::record_sync {

// Cache the session. Idempotent.
void Install(coop::net::Session* session);

// Per frame: a bounded slice of digests and publishes. Game thread.
void Tick();

// This peer's player took or let go of `actor`: it authors the prop's state for a while. Game thread.
void NoteTouched(void* actor);

// A record just landed on `actor`: its digest is re-based, so the change is not sent back. Game thread.
void NoteApplied(void* actor);

// Session teardown: the digests and the counts are dropped.
void OnDisconnect();

}  // namespace coop::props::record_sync
