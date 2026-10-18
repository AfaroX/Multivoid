# Shared media: a pool of custom files, and the screens and frames that show them

Status: plan, not built. Two open decisions are at the end.

## 1. What the game does today

- **Where custom media lives.** Every player's own profile folder, `%LOCALAPPDATA%\VotV\Assets\`
  (`lib_C::GetAssetFolder`), in one subfolder per kind:

  | Folder | Used by |
  |---|---|
  | `paintings\h`, `paintings\s`, `paintings\t`, `paintings\v` | picture frames, one folder per frame shape (`prop_picture_h/s/t/v_C`) |
  | `posters` | the custom poster (`prop_poster_c_C`) |
  | `radio` | mp3 files, plus `online.txt` with stream links |
  | `tv` | video files for the TVs (`prop_tv2_C` and its variants) |
  | `pcvid`, `rugs`, `stickers` | the PC's videos, rugs, stickers (later, same mechanism) |

- **How it is loaded.** `mainGamemode_C::importAssets` scans those folders once: pictures become
  textures (`Victory_LoadTexture2D_FromFile`), and it calls `import_radio` and `import_tv`, which build
  the radio and TV lists (`gamemode.tvVids` for the TV). Calling it again re-scans.
- **How a device refers to media.**
  - A picture frame saves only a **file name** (`filename`, in its getData record), and `upd` looks the
    texture up by that name (`lib_C::Get Custom Pic`). A change comes through
    `intComs_texturePickerApply`.
  - A TV plays a **link**: `openLink(struct_tv {name, link, isUrl})` opens a file path or a URL on its
    media player, and `fin` picks the next video at random (`RandomIntegerInRange`). TVs take
    gamemode-wide **media channels** (`Take Media Channel`), so several TVs can show one channel.
  - The radio's custom tracks and stations come from `import_radio`; which radio class plays them, and
    how it picks a track, is still to read.
- **What the mod carries today:** none of the files, and no lane for any of these devices. Skins send a
  name only. Since step 1 of SYNC-ROOT-CAUSES (`coop/props/record_sync`), a frame's or poster's record
  -- and so its file name -- crosses when the host changes it, or when a client changes one it just held.

So once both players have the **same file under the same name**, most of the sync is a name, plus a
position for what plays over time.

## 2. The plan, in three phases

### Phase 1 -- the shared pool

- **F1 > Shared media**, an ImGui panel beside Skins (`ui/skins_panel` is the model): this player's
  files by kind, read from the Assets folders, each with a share toggle; the pool, with who added each
  file and its transfer progress.
- **The pool is the host's**: a client's share is an intent with the file's manifest
  `{kind, name, size, hash}`; the host takes it and announces the pool. A peer that lacks a pooled file,
  by size and hash, asks for it; one that has it already sends nothing.
- **Transfer**: chunked on its own lane, the shape of `coop/save/save_transfer` (the host's save to a
  joiner), with a per-file cap, a rate bound, and a resumable index per chunk. A received file is
  written to a temporary name and renamed into the same Assets subfolder when its hash matches. A name
  collision with a different file is resolved by suffixing the incoming one, never by overwriting.
- **Rescan**: after a file lands, call `mainGamemode_C::importAssets` on the game thread, so it shows
  in the game's own pickers without a restart. To confirm: that the rescan does not reset a TV or radio
  already playing.
- **Late join**: a joiner gets the pool manifest at world-ready and fetches what it lacks.
- **Trust**: kinds are a fixed list, names are sanitised to a bare file name (no path), extensions are
  limited per kind, and sizes are capped per kind.

### Phase 2 -- picture frames and posters

- A client's pick (`intComs_texturePickerApply`, post) marks the prop touched in `record_sync`, so the
  client's change crosses as an intent the host takes and re-publishes, like a held prop's.
- A record naming a file this peer does not have yet: keep the name and re-run the prop's `upd` when the
  file lands (phase 1's rescan names the file).
- Covers every class with a `filename` record field: the four frames, the custom poster, and later the
  rugs and stickers.

### Phase 3 -- radio and TV

- **TV**: a lane per TV, by key, carrying `{link, isUrl, playing, position, volume, brightness}`. The
  host's copy is the reference: a client's `openLink`, pause or channel change is an intent the host
  runs; the host sends the state on each change and a position every few seconds; a receiver seeks
  when it drifts past about a second. `fin`'s random pick runs on the host only (the client's is
  refused at the script gate, as the dish hash codes are). A URL needs no file. Media channels: carry
  the channel a TV took, so TVs that share one stay together.
- **Radio**: the same shape with `{station or track, playing, position, volume}`, once its class and
  track selection are read.
- **Late join**: the current state and position of every playing device at world-ready.

## 3. Order, size, tests

- Phases 1 and 2 first, then a build to test; phase 3 after. Phase 1 is the largest (transfer, panel,
  rescan); phase 2 is small; phase 3 is about the size of phase 1.
- Tests per phase: share a picture and a video each way, rejoin mid-transfer, set a frame on each side,
  play a video and pause and seek it on each side, join late into a playing TV.

## 4. Open decisions

1. **Transfer path**: the mod's own connection, rate-bounded so play does not stutter (works wherever
   co-op works, no firewall change, big videos take tens of seconds to minutes), or a separate direct
   LAN connection for files (much faster, LAN only, the host's firewall asks once). Recommended: the
   mod's own connection.
2. **Scope of the first build**: phases 1 and 2 (recommended), all three, or the pool alone.
