// coop/media/media_pool.h -- a pool of custom media files every player gets a copy of.
//
// The game reads each player's own pictures, posters, radio tracks and TV videos from that player's
// asset folder (ue_wrap/world/custom_assets), so what one player put there was a blank frame or a dead
// channel on every other screen. A player puts a file in the pool from the F1 menu; the host keeps the
// pool -- {kind, name, size, hash} per file -- and every peer that lacks a pooled file fetches it into
// its own folder under the same name, so the game's references, which are file names, resolve on every
// machine. A client's share uploads the file to the host first; the host serves every copy from its own
// disk. Files travel in pieces on the Media lane, below every gameplay lane, and a finished file is
// checked against its hash before it is renamed into place; a name already taken by another file is
// never overwritten. The game re-imports its folder once a batch has landed.
// Wire: MediaPool (BlobChunkPayload) and MediaChunk (bulk, the media sink). Gameplay/network layer
// (principle 7). Game thread, but for the media sink, which runs on the net thread.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace coop::net { class Session; }

namespace coop::media_pool {

// The kinds of media, each one subfolder of the asset folder.
enum class Kind : uint8_t { PicH = 0, PicS, PicT, PicV, Poster, Radio, Tv, Count };
const wchar_t* KindFolder(Kind k);   // "paintings\h", "posters", "radio", "tv", ...
const char* KindLabel(Kind k);       // what the panel calls it

// Register the media sink and cache the session. Idempotent.
void Install(coop::net::Session* session);

// Per pump tick: hashing results, sends, completions, the debounced re-import. Game thread.
void Tick();

// A MediaPool chunk. Game thread.
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: a joiner's world is up -- the pool to it.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnPeerGone(uint8_t slot);
void OnDisconnect();

// ---- the panel (ui/media_panel), any thread ----

struct LocalFile { Kind kind; std::wstring name; uint64_t size; bool pooled; };
struct PoolRow {
    Kind kind; std::wstring name; uint64_t size; uint64_t hash; uint8_t owner;
    bool ready;            // the host holds the file, so every peer can fetch it
    enum class Here : uint8_t { Have, Fetching, Missing, Conflict, Failed } here;
    float progress;        // 0..1 while Fetching
};
struct Snapshot {
    bool session = false, host = false;
    std::wstring folder;
    std::vector<LocalFile> local;
    std::vector<PoolRow> pool;
    std::wstring status;   // the last thing worth telling the player
};
Snapshot GetSnapshot();

// The panel is showing: the local folders are re-listed every few seconds while it is.
void NotePanelOpen();

// Put one of this player's files in the pool, take one out (the sharer or the host), or re-list now.
void RequestShare(Kind kind, const std::wstring& name);
void RequestUnshare(uint64_t hash);
void RequestRescan();

}  // namespace coop::media_pool
