// coop/dev/patron_gate.h -- [dev, LOCAL patron-build only] neutralise the Shadow-Test-Build anti-mod gate.
//
// The patron "Shadow Test Build" of VOTV 0.10 (NOT the public release) scans <game>/Binaries for a
// .dll and Content/Paks for a .pak at the tail of mainGamemode::ReceiveBeginPlay, and on a match pops
// a modal "Modded client detected!" box and QuitGame, so a UE4SS mod cannot run on it; the public
// build is expected to carry no such gate. This instrument runs the mod LOCALLY on the unpacked
// patron build for the 0.10 port: OFF by default ([dev] patron_mod_gate_bypass), never shipped, no
// game-file edit (RULE 3 substrate; diagnostic/port tool, RULE 2 exempt). Mechanism in the .cpp.

#pragma once

namespace coop::dev::patron_gate {

// Resolve UFileSDKBPLibrary::GetFilesFromDirectory and install the (idempotent) Func patch, gated by
// [dev] patron_mod_gate_bypass -- a single bool read when off, and a no-op once armed. Game thread;
// called from boot as soon as the game-thread context is live, which is well before the first
// gamemode BeginPlay. Safe to call repeatedly (a retry if the plugin class is not registered yet).
void EnsureArmed();

}  // namespace coop::dev::patron_gate
