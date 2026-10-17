// coop/player/puppet_ragdoll_guard.h -- a peer's puppet never runs the game's ragdoll verb.
//
// Every ragdoll cause (a banana husk's punch, a fall, a blast, a wisp) ends in fallen(), which writes
// the per-machine dead flag, the black screen and the menu travel. A puppet is a mainPlayer_C, so the
// game reaches it with those causes on every machine that shows it. The verb is refused on a puppet
// at the script gate; run_end_travel still refuses the travel of the false deaths that never ragdoll.
// docs/players.md, Vitals, damage and death, has the whole account.
//
// Gameplay layer (principle 7): the engine through ue_wrap only. Game thread.

#pragma once

namespace coop::puppet_ragdoll_guard {

// Register the refusal, once, on the first tick that finds the gate ready. Idempotent, cheap after
// the first call. Called from subsystems::TickGameplay.
void Tick();

}  // namespace coop::puppet_ragdoll_guard
