# Fixing desync at the root

Status: analysis and plan, nothing here is built. Written after fixing a dozen co-op bugs one by one on
branch `fix/puppet-slip-and-join-vitals`, against upstream `main` a9ae81c3 (build b225).

**The question.** Every bug fixed so far was a symptom: a slip that killed the wrong player, a lid that
came back twice, a thrown item snapping back, a drone sack only one player had, a bucket that spilled, a
shovel's junk only one player saw, an extinguisher that ruptured on one screen. Upstream's open issues
are more of the same (a printer that never syncs, a mining rig, hash codes, oven repair). What would stop
the stream instead of chasing it?

**The short answer.** The mod's design principles are right -- one owner per element, a client acts by
intent, park the receiver's brain (`docs/architecture.md`, `docs/coop-sync-doctrine.md`) -- but they are
applied **one verb at a time, by opt-in**. Every game function nobody has written a lane for still runs
on whichever machine triggered it and forks the world silently. Five structural gaps produce almost all
of the bugs; closing them generically retires whole families of symptoms at once.

---

## 1. The five root causes

### 1.1 Unsynced by default: replication is an allowlist of verbs

Each synced behaviour is a hand-written lane for one game verb: the drone's call button, the point sack,
door verbs, keypads, the broom stroke, now the drone's drop-sack, the shovel, the extinguisher. Anything
not on the list runs locally on the peer that triggered it. Nothing says so at run time; the gap shows
up as a player's bug report weeks later.

The evidence is in the logs already: one 36-minute client session logged
`steady-world re-seed adopted N NEW runtime-spawned keyed prop(s) -- ... a client authors no PropSpawn`
130 times -- 130 moments where the client's world grew a prop the host never got. Most are harmless
per-peer spawner churn; some were the shovel's junk. Nobody can tell which without reading the code.

**Bugs in this family:** drone drop-sack, shovel scrap, extinguisher rupture, oven repair (#28), 3D
printer (#49), GPU mining rig (#43), hash codes (#47).

### 1.2 Per-prop state has no live channel -- only birth records

A prop's own state (an extinguisher's `active` and `life`, a reel case's lid, a printer's progress, an
oven's repair) travels **only when the prop is born**: `prop_save_data` sends the record on spawn, drop
and join, "and on none other, so a prop whose save state changes while it sits there never tells anyone"
(`coop/props/prop_record_refresh.h`). Two classes have a hand-written republish (the drive box and the
reel case, by watching their `upd`). Every other class's in-place change is local.

The game already wrote the perfect serializer for this: every prop's `getData` / `loadData` pair, the
same one its save system uses. The mod uses it at birth and nowhere else.

**Bugs in this family:** reel case lid state (fixed by hand), extinguisher state, oven repair, printer
progress, food and concrete drift (the known limit at `docs/props.md`, "local-accumulator props drift").

### 1.3 Physics authority is split four ways, and motion in between is nobody's

Today a prop's motion has a different owner in each phase: the **holder** while held (pose stream), the
**host** at rest, **each peer's own physics** in the seconds after a release, and **no one** when it is
knocked ("a knock is none [of the verbs]", `docs/props.md`, known limits). The host's driven-prop channel
(`coop/props/prop_drive_host`) streams a moving prop only when a specific verb claims it (a hook, a broom,
and now a host release or a rupture).

**Bugs in this family:** thrown items landing apart (0.2-152 cm), a client's throw snapping back, the
bucket's contents spilling, knocked props diverging, the extinguisher's flight, a held prop teleporting.

### 1.4 Every copy runs the whole game, and the wrong copy answers events

Every machine runs every actor's full single-player Blueprint. When the game dispatches an event on a
copy that is not authoritative for it -- a hit on a puppet, a knock on a held prop's remote copy, a slip
on a puppet -- that copy computes an outcome the authoritative one never sees, or writes this machine's
own singletons. The mod refuses these one class at a time (impact damage on puppets, ignite, ragdoll).

**Bugs in this family:** banana-peel slip killing others, damage and fire on puppets, the extinguisher's
rupture (the knock lands on the holder's copy only).

### 1.5 Messages about one prop are not ordered per prop

A prop's spawn, hold, release, place and destroy ride different message kinds with no shared sequence
number, so a destroy and a spawn of the same prop can cross. The trash channel solved this for piles
with a per-element generation (`ctx`); keyed props have none.

**Bugs in this family:** the reel case lid duplicate (a drop and a destroy crossing), the drop-then-pickup
ghost, the dropped `PropSpawn elementId=0` announcements.

---

## 2. The plan: five generic mechanisms

Ordered by value for effort. Each says what it replaces, how it fits the doctrine, and its risk.

### Step 0 -- Measure: a desync auditor (dev-only, no gameplay risk)

Before building anything big, make the mod **tell us** what diverges instead of waiting for reports.

- **World digest.** Every N seconds, host and each client hash, per region, every keyed prop's class,
  quantised position and `getData` record digest, and exchange the region hashes. A mismatched region is
  drilled into and the diverged props are logged by class and key. The grime lane already does this for
  its own index (`coop/interactables/grime_sync.cpp:266`, `posHash` "compare host vs client"); this is
  that, for every keyed prop.
- **Unsynced-birth census.** Name every client birth that never reached the host by the Blueprint
  function on its calling frame (the same frame read `prop_spawn_authoring` already does). Instead of
  "130 runtime-spawned props", the log reads "41 x mainPlayer_C.throwShovelItem, 12 x
  actorChipPile_C.turnIntoScrap, 77 x spawner_X.tick" -- a ranked to-do list generated from real play.
- **Size:** small (a dev lane, one wire kind for the digest exchange). **Payoff:** every later step can
  be judged by numbers, and new gaps announce themselves.

### Step 1 -- Live per-prop state: republish the record when it changes (biggest single win)

Generalise `prop_record_refresh` from two hand-picked classes to **every keyed prop**:

- The peer that is authoritative for a prop (the host at rest; the holder while held) keeps a digest of
  each **recently touched** prop's `getData` record -- touched meaning grabbed, hit, used, or awake --
  and re-publishes the record when the digest changes, budgeted per tick.
- Receivers apply it with the game's own `loadData`, which the mod already uses for every birth.
- A short **exclusion list** names the classes whose `loadData` misbehaves when re-applied in place, each
  with the reason (discovered by the auditor, not guessed).
- **Fits the doctrine:** replicate authoritative state, re-derive the rest. It is the arbiter-holds-
  values direction of roadmap phase 2 (`docs/architecture.md`, "Where the authority is going"); the
  record is exactly the value the future arbiter would hold.
- **Late join:** free -- the join already sends records.
- **Retires:** most of 1.2 at once -- extinguisher state, oven repair, printer progress, lids and caps --
  and the hand-written `prop_record_refresh` cases fold into it.
- **Risk:** `loadData` side effects (re-running `init`, waking bodies). Mitigated by applying only on a
  digest change, never on an unchanged record, and by the exclusion list.

### Step 2 -- One physics author per moving body

Replace the four-way split with a single rule, MTA's unoccupied-vehicle sync: **every awake body has
exactly one syncer, and its pose streams until it sleeps.**

- The **host** streams every awake keyed prop by default -- not only the ones a verb claimed -- through
  the existing driven-prop channel, gated on PhysX's own sleep state, so a world at rest costs nothing
  (scope rule 7: "bandwidth is gated by whether an entity is active").
- A **client** gets a short **lease** on bodies its own actions set moving -- its throw, its knock, the
  contents of the bucket it carries -- and streams them; the lease returns to the host when they sleep.
  The holder-while-held rule is the existing special case of this.
- Receivers drive leased and streamed bodies kinematically, by **move** not teleport (done for held
  props in `8b0456a0`), and hand physics back at the end edge with the author's final pose.
- **Retires:** all of 1.3 -- release divergence, knocks, the bucket, the extinguisher's flight -- and the
  per-verb `Coast`/`Claim` calls collapse into the default.
- **Risk:** bandwidth during big physics events; bounded by the awake set and the existing per-tick
  publish budget.

### Step 3 -- Admit player-caused births by authorship, not by class

The drop-intent door admits a client birth only from a whitelist (reels, modules, drives, discs,
upgrades, container items, the spawn menu, the toolgun, and now the shovel). Generalise the authorship
test `prop_spawn_authoring` already uses for the spawn menu: **a birth whose script call chain starts at
the local player's own input or hand verbs** (`InpActEvt_*`, `playerHandUse_*`, a montage notify on the
player) is player-caused and crosses as an intent; a birth with no player on its chain is ambient world
churn and stays per-peer, as today.

- **Retires:** the shovel-style gaps for every tool and every interaction that spawns things.
- **Fits the doctrine:** "who asked", not "what class" (`docs/coop-entity-expression-map.md`).
- **Risk:** an ambient spawn that happens to run inside a player verb would double; the census from
  step 0 measures exactly that before the switch is flipped.

### Step 4 -- Per-prop generations on every keyed-prop message

Give every keyed prop a generation, bumped on each spawn, place, hold and destroy, carried on every
message about it, as the trash channel's `ctx` already does for piles. The host applies a message only
to the generation it names; a client discards an echo for a generation it has already moved past.

- **Retires:** 1.5 -- the lid-class races -- generically, including the consumed-echo case fixed by hand
  in `6aacf8d3`.
- **Cost:** a wire-format change (a protocol bump), so it belongs with a release.

### Step 5 -- The right copy answers each event

Move the per-class refusals of 1.4 to the **base classes** with one rule: an event that writes shared or
per-machine state runs only on the copy that is authoritative for its subject at that moment -- the
holder's while held, the host's at rest, the owner's for a body. Concretely, a guard at `prop_C`'s
damage and impact entries and at `comp_physicsImpact`, on the same authority table as step 2, plus the
puppet guard generalised from `mainPlayer_C` alone. The authoritative copy's outcome then crosses on the
step-1 record and the step-2 stream.

- **Retires:** the wrong-subject family, including future props with their own damage behaviour.

---

## 3. What this does not remove

- **Non-prop actors with their own state** -- the drone, consoles, the power grid, NPC brains -- still
  need a lane each; the record channel covers keyed props, not level singletons. Steps 0 and 3 at least
  make their gaps visible and narrow the list.
- **Presentation** (sprays, sounds, particles a ruptured extinguisher makes in its own tick) stays per-
  peer by design; some of it will look different until a lane mirrors the trigger.
- **The fingerprint table.** Every new watched function needs an entry only the maintainer's tooling can
  write (`fact_rows_gate`). Generic mechanisms watch **fewer, more central** functions, which helps.

## 4. How to proceed

1. **Talk to the maintainer first.** Steps 1, 2 and 4 restructure the mod's core and line up with the
   roadmap's phase 2. Built on a fork without their agreement, they would likely not be adopted and
   would conflict with ongoing upstream work. The evidence in this document -- logs, decoded game code,
   the fix list -- is a strong opening.
2. **Build step 0 on the fork regardless.** It is dev-only, risk-free, and produces the numbers that
   make every later decision, ours or the maintainer's.
3. **Set up a local build.** Changes at this scale need compile-and-test in minutes, not a GitHub run
   per change (see `BUILDING.md`; this machine lacks Visual Studio 2026 Build Tools, CMake and vcpkg).
4. Then step 1, then step 2: they retire the most bug families, and step 2's lease is where the
   bucket, knock and extinguisher-flight fixes made so far would fold in.
