// coop/dev/desync_audit.h -- every peer writes what its world holds, so a script can say what diverged.
//
// Once a minute of the shared clock (the host's, which every client mirrors) each peer appends a snapshot
// of every keyed prop it holds to multivoid-desync.txt beside the game: key, class, position, rotation,
// physics state, what moves it, and a digest of its own save record with the transform and key left
// out. It also counts the client births the drop-intent drain did not admit, by class and by the
// Blueprint function on the birth's calling frame. The comparison script docs/testing.md describes
// pairs a host's file with a client's and names what diverged.
//
// Dev-only, ini-gated ([dev] desync_audit=1); off, every entry point is one cached bool read. It only
// reads: nothing it does changes the world or the wire. Game thread.

#pragma once

#include <string>

namespace coop::net { class Session; }

namespace coop::dev::desync_audit {

bool IsEnabled();

// Per tick: cut a snapshot when the shared clock crosses into a new minute. Game thread.
void Tick(coop::net::Session& session);

// The Blueprint function on the current call's calling frame, as "Class.function"; empty when there is
// none or the snapshots are off. Call from inside a native post callback (the finish-spawn seam).
std::wstring CallerName();

// CLIENT: a fresh birth the drop-intent drain did not admit -- it stays on this client. Game thread.
void NoteLocalOnlyBirth(const std::wstring& cls, const std::wstring& caller);

// Session teardown: the birth tally is written and cleared.
void OnDisconnect();

}  // namespace coop::dev::desync_audit
