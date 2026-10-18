// ue_wrap/world/custom_assets.h -- the game's custom media folder and its import.
//
// The game reads a player's own pictures, posters, radio tracks and TV videos from one folder,
// lib_C::GetAssetFolder (the gamemode's assetFolder when set, else the profile's
// AppData\Local\VotV\Assets), one subfolder per kind. mainGamemode_C::importAssets scans it once at
// the world's start -- the textures, then import_radio and import_tv -- and running it again rescans,
// so a file put there mid-session shows in the game's own pickers. A picture frame and a poster name
// their image by file name and look it up again in their refresh functions.
// Engine-wrapper layer (principle 7): no network logic. Game thread.

#pragma once

#include <string>

namespace ue_wrap::custom_assets {

// The folder, as the game resolves it, with no trailing separator; empty while no world is up.
std::wstring AssetFolder();

// Re-run the game's own import of the folder. False when the gamemode or the function does not resolve.
bool Reimport();

// A picture frame (prop_picture_h_C and its shapes) or a custom poster (prop_poster_C) looks its image
// up again by its file name. False when `actor` is neither, or the call did not dispatch.
bool RefreshImage(void* actor);

// Whether `actor` is a picture frame or a poster.
bool IsImageProp(void* actor);

}  // namespace ue_wrap::custom_assets
