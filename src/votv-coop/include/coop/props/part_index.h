// coop/props/part_index.h -- a part its parent spawns, found by the name every peer gives it.
//
// A door a cupboard, a fridge or a microwave spawns (prop_swinger_C and its kin) is a child actor: the
// parent's construction spawns it on every peer, and its own Key is minted per process, so the held-prop
// stream had nothing a receiver could resolve, and a door swung on one screen stayed shut on every other.
// Such a part's portable key (coop/element/portable_identity: the parent's key and the part's component
// name, "mv_" and a hash) is the same on every peer. The holder streams the part under it, and this index
// turns it back into the local part. Gameplay layer. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace coop::part_index {

// Whether `wireKey` names a part (a portable key) rather than a prop's own Key.
bool IsPartKey(const std::wstring& wireKey);

// The live part `wireKey` names here, or null. A miss walks the world again, at most every two seconds.
void* Find(const std::wstring& wireKey);

// The key a peer names `actor` by on the wire: a part's portable key, else the prop's own Key; "" when it has
// neither. Game thread.
std::wstring WireKeyFor(void* actor);

// The local actor a wire key (a part's or a prop's) and an eid name, key first; null when neither resolves.
// Game thread.
void* ResolveWire(const std::wstring& wireKey, uint32_t eid);

void OnDisconnect();

}  // namespace coop::part_index
