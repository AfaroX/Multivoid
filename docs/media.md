# Shared media

## Purpose

The game reads each player's own custom media -- pictures, posters, radio tracks, TV videos -- from that
player's asset folder, so what one player put there was a blank frame or a dead channel on every other
screen. This page covers the pool that copies those files to every player, the frames and posters that
show the same image everywhere, and the TVs and radios that play the same thing at the same point.

## How it works

### The asset folder

`lib_C::GetAssetFolder` names the folder: the gamemode's `assetFolder` when set, otherwise the profile's
`AppData\Local\VotV\Assets`. One subfolder per kind: `paintings\h`, `paintings\s`, `paintings\t`,
`paintings\v` (the four frame shapes), `posters`, `radio` (tracks, and `online.txt` with stream links) and
`tv`. `mainGamemode_C::importAssets` scans it at the world's start and again whenever it is called, so a
file put there mid-session shows in the game's own pickers (`ue_wrap/world/custom_assets`).

### The pool

F1 > Cosmetics > Shared media lists this player's own files by kind and the pool. A share hashes the file
off the game thread and offers it; the host keeps the pool -- kind, name, size and hash per file -- and
sends it to every peer on each change and to a joiner at its world-ready. A client's share uploads the
file to the host first, and the host serves every copy from its own disk. A peer that lacks a pooled
file asks for it. Files travel as `MediaChunk` pieces of 56 KB on the Media lane, the lowest of the
connection's four, so a video never delays a gameplay message; the transport's backpressure paces them.
A piece is written to `multivoid-incoming` under the asset folder, outside every kind's folder (the game's
import lists every file in the TV and radio folders), and a finished file is checked against its size
and hash before it is renamed into place. A name already taken by another file is never overwritten.
Once a batch has landed the game re-imports its folder and every frame and poster looks its image up
again (`coop/media/media_pool`). Names are bare file names with an extension the kind allows, and sizes
are capped per kind: 64 MB for an image, 256 MB for a track, 4 GB for a video.

### Frames and posters

A picture frame (`prop_picture_h_C` and its shapes) and the custom poster (`prop_poster_C`) keep their
image as a file name in their own save record, which crosses when it changes
([props.md](props.md), a prop's own state). A client's pick through the texture picker marks the frame
touched before the pick applies, so the client sends the change and the host takes and re-publishes it.
A record that lands on a frame or a poster looks its image up again, since the poster's own loadData
does not (`coop/media/media_frames`).

### TVs and radios

Every TV (`prop_tv2_C` and its variants) and the internet radio (`prop_radio_internet_C`) play a link
through `openLink(struct_tv {name, link, isUrl}, pausePlayer)` on their media player, and roll what plays
next in `fin`. The host's copy of a device plays: a client's `openLink` is refused on its copy and sent
as an intent, and a client's `fin` is refused. The host sends each device's link, play state and
position on every change, every two seconds while it plays, and to a joiner at its world-ready; a
device the host's world was already playing is found from its player's url. A local file's link is
sent relative to the asset folder (`@assets:`) and rebuilt from the receiver's own, since the absolute
path names each machine's user. A client opens the same link, follows the play state, and seeks when it
drifts past two seconds; a pause or a resume its own player makes is sent to the host as an intent
(`coop/media/media_playback`).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the pool | the host | the list on each change; a client's share and requests are intents |
| a pooled file | its sharer, then the host | pieces from the host's disk to whoever asks |
| a frame's or a poster's image | the host at rest, the picking client for a moment | the prop's save record |
| what a TV or a radio plays | the host | the link, the play state and the position; a client's press is an intent |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `MediaPool` | host to all; client to host; host to one client | the pool; a share, a removal or a request for a file |
| `MediaChunk` (bulk, Media lane) | either direction, to the asker | one piece of a pooled file |
| `MediaPlay` | host to all; client to host | a device's link, play state and position; a client's open, play or pause |

## Known limits

| Limit | Status |
|---|---|
| A file is fetched only while both players are in the session; a pooled file the host does not hold yet waits for its sharer | `[V]` |
| A TV's volume and brightness cross on its save record from the host only; a client's change of them stays on its screen | `[V]` the record lane carries what a client holds or just let go of |
| A video's position is followed to within two seconds, not frame by frame | `[V]` by design: a closer follow would seek often |
| The radio's built-in stations (`prop_radio_local_C`) are not synced | `[V]` they play the game's own sounds, not the asset folder |

## Code map

| Concern | Files |
|---|---|
| the pool and the transfer | `coop/media/media_pool`, `ui/media_panel` |
| frames and posters | `coop/media/media_frames` |
| TVs and radios | `coop/media/media_playback`, `ue_wrap/devices/media_device` |
| the asset folder and its import | `ue_wrap/world/custom_assets` |
