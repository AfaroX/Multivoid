// ue_wrap/devices/media_device.h -- a TV or a radio that plays a link, and its media player.
//
// prop_tv2_C (every TV: the plasma, the floor and mini TVs, Erie's, the kerfur's) and
// prop_radio_internet_C (the radio that plays the Assets\radio tracks and online.txt's streams) both
// play through openLink(data: struct_tv {name, link, isUrl}, pausePlayer), which opens the link on the
// device's `mediaPlayer` (a TV's is the gamemode media channel it took). The link of a local file is
// an absolute path under that machine's own asset folder, so it differs between machines. A track or
// a video ending calls the device's `fin`, which picks what plays next.
// Engine-wrapper layer (principle 7): no network logic. Game thread.

#pragma once

#include <cstdint>
#include <string>

namespace ue_wrap::media_device {

enum class Kind : uint8_t { None = 0, Tv = 1, Radio = 2 };

// What `actor` is: a TV, a radio, or neither.
Kind KindOf(void* actor);

// The device's media player (a UMediaPlayer), or null.
void* PlayerOf(void* device);

// Open a link on the device through its own openLink, as its interface does.
bool OpenLink(void* device, const std::wstring& name, const std::wstring& link, bool isUrl, bool pause);

// What openLink was called with, read from a call's parameter frame (a script-gate call on openLink).
// False when the frame does not read.
bool ReadOpenLinkArgs(void* function, const uint8_t* locals, std::wstring& name, std::wstring& link,
                      bool& isUrl, bool& pause);

// The player's state. False when the player is null or the call did not dispatch.
bool Url(void* player, std::wstring& out);
bool IsPlaying(void* player, bool& out);
bool IsPreparing(void* player, bool& out);
bool TimeMs(void* player, int64_t& out);
bool Seek(void* player, int64_t ms);
bool Play(void* player);
bool Pause(void* player);

}  // namespace ue_wrap::media_device
