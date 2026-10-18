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

F1 > Cosmetics > Shared media lists this player's own files by kind, in or out of a session, and the
pool. Add files opens the system's file dialog on its own thread; each picked file is copied into the
asset folder -- a picture into the kind selected, a track into `radio`, a video into `tv`, a taken name
given a numbered one -- and, in a session, shared. A file a player plays on a TV or the radio, or hangs
in a frame, is shared by itself, so nobody presses Share for it and nobody confirms a download. A share hashes the file
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
next in `fin`. Each device has an owner: the player who used it last, the host until anyone does. The
owner's copy plays -- its picks, pauses, scrubs, volume, brightness and next-track roll -- and the owner
sends the device's state on every change and every two seconds while it plays; the host relays an owning
client's state to everyone else and sends every device's last state to a joiner at its world-ready. A
position further from where play would have put it than a second and a half is a scrub, and goes out at
once. Every other peer, the host included, follows: it opens the same link, follows the play state, seeks
when it drifts past two seconds, and takes the volume (a TV's `audioVolume`, the radio's sound's volume
multiplier), the brightness and the playlist order -- `playmode` (one on loop, in order, at random,
shuffled) and `playIndex`, where the order stands -- so whoever owns the device next rolls on in the same
order from the same place. A local file's link is sent relative to the asset folder (`@assets:`) and
rebuilt from the receiver's own, since the absolute path names each machine's user; a peer the pool has
not brought the file to yet opens it once it lands and seeks to where the owner is, so nobody's download
holds the owner's screen back.

Only one player uses a device at a time, as with the base laptop. Opening a TV's or a radio's interface
asks the host for it: the host keeps each device's owner and holder, and a device in another player's
hands refuses the interface and its `openLink` with "This TV is being used by <name>." in the chat. Closing
the interface frees it; the host also frees a holder that has said nothing for eight seconds, and a player
who leaves frees what they held and hands what they owned to the host. `fin` runs only on the owner's copy
(`coop/media/media_playback`).

## Who owns what

| State | Owner | Shape |
|---|---|---|
| the pool | the host | the list on each change; a client's share and requests are intents |
| a pooled file | its sharer, then the host | pieces from the host's disk to whoever asks |
| a frame's or a poster's image | the host at rest, the picking client for a moment | the prop's save record |
| what a TV or a radio plays | its last user; the host keeps who | the link, the play state, the position, the volume, the brightness and the playlist order |
| who is using a TV or a radio | the host | a claim when the interface opens, a release when it closes |

## Wire messages

| Kind | Direction | Carries |
|---|---|---|
| `MediaPool` | host to all; client to host; host to one client | the pool; a share, a removal or a request for a file |
| `MediaChunk` (bulk, Media lane) | either direction, to the asker | one piece of a pooled file |
| `MediaPlay` | host to all; client to host | a device's owner, holder, link, play state, position, volume, brightness and playlist order; a client's claim or release |

## Known limits

| Limit | Status |
|---|---|
| A file is fetched only while both players are in the session; a pooled file the host does not hold yet waits for its sharer | `[V]` |
| A video's position is followed to within two seconds, not frame by frame | `[V]` by design: a closer follow would seek often |
| A follower's device that changes channel by itself, without `openLink`, plays that channel on its own screen until its owner opens a link again | `[V]` `coop/media/media_playback` follows the owner's link and opens it only on a new pick |
| The radio's built-in stations (`prop_radio_local_C`) are not synced | `[V]` they play the game's own sounds, not the asset folder |

## Code map

| Concern | Files |
|---|---|
| the pool and the transfer | `coop/media/media_pool`, `ui/media_panel` |
| frames and posters | `coop/media/media_frames` |
| TVs and radios | `coop/media/media_playback`, `ue_wrap/devices/media_device` |
| the asset folder and its import | `ue_wrap/world/custom_assets` |
