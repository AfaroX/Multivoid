# Design: items inside a carried bucket stay in it

Status: analysis only. Nothing here is built. Written against branch `fix/puppet-slip-and-join-vitals`
(base: upstream `main` a9ae81c3, build b225).

**Reported behaviour** (two-player test, host + client, b225): single items sync fine; when a
bucket holds a stack of items and either player moves the bucket, the stack falls out and scatters.

---

## 1. What the bucket is, and how the game keeps its contents

Read from the game's own archive (`VotV-WindowsNoEditor.pak`, uncompressed and unencrypted, so a
blueprint's name table is readable as length-prefixed strings; read-only).

**The class.** `prop_bucket_C` (`/Game/objects/prop_bucket`), with siblings `prop_bucket_mop_C`,
`prop_compostBucket_C`, `prop_concreteBucket_C`. Its name table carries `trigger` (a box with
`OnComponentBeginOverlap` / `OnComponentEndOverlap`), `itemsInside`, the water, soap and sponge logic,
and an import of `prop_openContainer_C`, with no cast node to that class -- the shape a parent-class
import has. **To confirm at run time**: that `prop_bucket_C`'s super is `prop_openContainer_C`.

**The parent.** `prop_openContainer_C` (`/Game/objects/prop_openContainer`; also the parent of the
three bowls and the Easter basket). Its name table carries:

| Name | What it says |
|---|---|
| `glueInside`, `itemsInside` (array), `GetOverlappingActors`, `K2_AttachToActor`, `GetAttachParentActor` | the container collects the actors overlapping its trigger and **attaches them to itself** |
| `playerHoldPre`, `isHolding`, `canHold` | the attach is tied to the container being held |
| `detachOnAngle`, `detachAngle` | tilted past an angle, it lets its contents go (a pour) |
| `SetPhysicsLinearVelocity`, `SetPhysicsAngularVelocity...`, `GetPhysicsAngularVelocityInDegrees` | released contents are handed the container's motion |
| `skipPhysics`, `IsAnyRigidBodyAwake`, `ReceiveTick` | the logic runs, at least partly, on the container's tick |

So in single player the game does **not** rely on physics to keep a stack in a moving bucket: while a
player holds it, the items are glued to it, and they come loose on a tilt or a release.

**Not established from names alone** (needs the bytecode or a run-time probe, section 4):
- whether `isHolding` is written by `playerHoldPre` (an event the holder's own player fires) or
  derived each tick from the local player's grab;
- which body detaches the contents on release (`ReceiveTick` seeing `!isHolding`, or an event).

## 2. Why the stack spills in co-op

**The carrier's machine runs single player.** Its own player grabs the bucket, its own `playerHoldPre`
and tick run, and the contents are glued. Nothing in the mod interferes with `prop_bucket_C`: the
client-side container guard cancels `prop_openContainer_C::ReceiveTick` only for
`prop_garbageContainer_C` instances (`src/votv-coop/include/coop/interactables/garbage_sync.h:23-26`,
`src/votv-coop/src/coop/interactables/garbage_sync.cpp:66-72`).

**Every other machine drives a copy of the bucket and never glues.** A remote hold reaches a receiver
as a pose stream. At the hold's first pose the receiver replays only the **base** prop's grab prelude,
`prop_C::playerGrabbed_pre`, via `ue_wrap::prop::CallBaseGrabPrelude`
(`src/votv-coop/src/coop/props/remote_prop.cpp:214-258`); what a subclass adds to its prelude or its
hold is not replayed -- the project already lists this as a known limit for the sprinkler and the
glowstick (`docs/props.md:311`). The receiver then turns the bucket's simulation off and moves it by
`SetActorLocation` every packet, "a kinematic follow" (`remote_prop.cpp:270`). The items inside are
separate props simulating on that machine, resting against a container that is teleported out from
under them at 60 Hz: they fall out and scatter.

**Which spill counts.** The host owns every prop at rest, and a prop that something else knocks has
no lane: "A prop that a pushed prop knocks moves on the host only" (`docs/props.md:319`), and nothing
converges where a prop comes to rest (`docs/props.md:310`). So:

| Carrier | Carrier's screen | Other screen | Who is right afterwards |
|---|---|---|---|
| client | items glued, ride along | the **host's** items spill | the host's world says they spilled; the client still sees them in the bucket -- the two disagree for the rest of the session |
| host | items glued, ride along | the client's items spill | the host's world says they are in the bucket; the client sees them on the floor until something re-syncs them |

Either way the copies diverge, and with a client carrier the host's authoritative world loses the
stack.

## 3. Candidate designs

All three are judged against CONTRIBUTING.md: fix the cause at its site, no crutches (ground rule 1),
own substrate (rule 3), and every lane answers a mid-activity join (principle 8, `CONTRIBUTING.md:46`).

### A. Replay the container's own hold on every receiver (recommended)

The game already knows how to carry its contents; the receivers just never run that logic.

- **Where:** the prelude replay in `remote_prop.cpp:237-258`. For a prop of the
  `prop_openContainer_C` lineage, after the base prelude, run the container's own hold on the
  receiver's copy -- `playerHoldPre` or `glueInside`, whichever section 4 shows is the entry -- so it
  attaches the props overlapping its trigger on **that** machine. Attached children follow a
  kinematically driven parent, so the stack rides the stream.
- **Release:** at the hold's `PropRelease` (`remote_prop.cpp:414`, which already restores the
  container's physics and hands it the holder's velocity), run the container's own release so the
  contents detach and take its motion -- whatever section 4 finds the game's own detach to be.
- **Tilt / pour:** the receiver's copy is driven to the carrier's rotation, so if the container's own
  angle check runs on the receiver while it is held, it pours at the same angle the carrier's does.
  If the check reads the local player's grab, it has to read the remote hold instead (section 4).
- **Who authors:** the carrier, through the hold it already streams. No new wire message: the hold's
  first pose and its release are the two edges, and both already arrive.
- **Late join:** the replay is keyed per hold at its first pose (`hold.preludeGen`), so a peer that
  joins mid-carry runs it at the first pose it receives, against the contents the host's snapshot
  placed in the bucket.
- **Generalises** the known limit at `docs/props.md:311`: the same seam is where the sprinkler's hose
  detach and the glowstick's own prelude would be replayed.
- **Cost:** small in code (one lineage test, two verb calls, one doc row), but it rests on knowing the
  container's hold and release entries exactly -- section 4 first.
- **Residual:** the items' rest positions after a release still come from each peer's own physics
  (the general limit at `docs/props.md:310`); with a host carrier, the host's release could coast the
  detached contents on the driven-prop channel the way a host release now coasts the prop itself
  (`coop/props/prop_drive_host`).

### B. The carrier streams the contents' poses

- The carrier publishes every glued item's pose alongside the container's, and receivers drive them
  kinematically, as the driven-prop channel does for the host today (`coop/props/prop_drive_host`,
  `coop/props/prop_drive_stream`).
- **Against it:** that channel is host-authored; a client carrier would need a client-authored
  multi-prop stream, a new lane with its own authority, generation and late-join rules. A full bucket
  is 10-20 extra pose streams per carry. It reproduces with bandwidth what the game does with one
  attach. Large (several hundred lines plus a wire kind and a protocol bump).

### C. Drive held props by a swept kinematic target instead of a teleport

- Moving the receiver's copy with a kinematic target lets local physics carry the contents.
- **Against it:** it changes every held prop on every receiver, and the game itself does not trust
  physics to hold a stack in a moving container -- that is why `glueInside` exists. With an
  interpolated, jittery stream, stacks would still spill. Small code, high risk, likely insufficient.

## 4. Recommendation

**Design A.** Before writing it, settle the two open questions with a run-time probe rather than a
guess (the project's own instrument style, `src/votv-coop/src/coop/dev/`):

1. On the carrier, a script-gate watch (POST, dev-flagged) on the `prop_openContainer_C` bodies --
   `playerHoldPre`, `glueInside`, `ReceiveTick` -- logging `isHolding` and `itemsInside` before and
   after, through one pick-up, carry, tilt and release of a filled bucket. That names the hold entry,
   the release/detach path, and whether `isHolding` is event-written or tick-derived.
2. Confirm `prop_bucket_C`'s super is `prop_openContainer_C` (`R::ClassOf` super chain at run time).

Then implement A at `remote_prop.cpp:237-258` and `:414`, update `docs/props.md` (the "Holding,
throwing, dropping" section and the known-limit row at `:311`), and run the test below.

**Rough size:** probe ~80 lines (dev-only); the change itself ~100-150 lines plus docs. One build, one
two-player session to verify.

### Two-player test for A

Run each with the host carrying, then the client carrying:

1. Fill a bucket with 5-10 small items. Pick it up, walk around, stop. **Expect:** the stack stays in
   the bucket on both screens.
2. Tilt the bucket past the pour angle while held. **Expect:** it pours on both screens at about the
   same moment.
3. Drop or throw the filled bucket. **Expect:** the contents come loose with the bucket's motion on
   both screens, and end close to each other.
4. Have the second player join (or rejoin) mid-carry. **Expect:** the joiner sees the stack in the
   bucket.
5. Repeat 1 with a bowl and the Easter basket (same parent class) as a control.

Both players keep `multivoid.log` from the session; the probe and the replay log what ran where.
