// coop/player/puppet_ragdoll_guard.h -- a peer's puppet never runs the game's own ragdoll verb.
//
// WHY THIS EXISTS. `ragdollMode` is globally scoped: every one of its causes ends in
// `fallen(death)`, and `fallen` writes the PER-MACHINE singletons -- `dead`, the viewport's black
// screen, and `lib_C::loadLevel("menu")`, the run-ending travel. A puppet is a mainPlayer_C with a
// collision capsule, so the game reaches it with those causes on every machine that shows it: the
// banana husk's `steppedOn` punches whatever body stepped on it (`punch` -> `ragdollMode(true,
// false, false)` -> `fallen(false)`, autotest_slip.cpp's bytecode trail), and a fall, a blast and a
// wisp arrive the same way. So a peer slipping somewhere else ran the death chain against this
// machine's own state, and the first report of it said a peel took every peer with it.
//
// ue_wrap/engine/engine_mainplayer.h already states the invariant -- "a puppet never runs
// ragdollMode", its playerRagdoll_C lifecycle assumes a possessed, ticking player -- and our own
// layer keeps it: the faint DISPLAY spawns the body directly (coop/player/remote_player_ragdoll),
// and the one product caller of the verb addresses the local pawn (coop/creatures/wisp_tear_mirror).
// The game was the only one left violating it, and nothing enforced it. This does, at the verb.
//
// WHY HERE AND NOT AT THE TRAVEL. coop/player/run_end_travel refuses a menu travel a puppet
// authored, which is the same symptom's last step; it stays, because a false death reaches it by
// routes that never ragdoll. Refusing the body is the site of the problem (ground rule 1): the
// steps before the travel -- `dead` latched on the copy, `deathEnd`, the five-second black screen --
// never run either.
//
// WHY `punch` IS LEFT ALONE. Its remaining effect on a puppet is an impulse on a body whose pose
// the owner authors and whose correction erases it; its damage is already refused
// (coop/player/player_damage). `ragdollMode` is the one choke point every cause passes through,
// so one refusal covers them all.
//
// Gameplay layer (principle 7): the engine through ue_wrap only. Game thread.

#pragma once

#include <cstdint>

namespace coop::puppet_ragdoll_guard {

// Register the refusal, once, on the first tick that finds the gate ready. Idempotent, cheap after
// the first call. Called from subsystems::TickGameplay.
void Tick();

// How many ragdoll bodies have been refused on a puppet this session. What a drill judges by.
// Any thread.
uint32_t RefusedRagdolls();

}  // namespace coop::puppet_ragdoll_guard
