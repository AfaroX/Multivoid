// coop/props/prop_tool_flags.h -- a toolgun's freeze holds a prop still in the same place on every screen.
//
// The toolgun's prop-properties tool (tool_propProps_C) runs the prop's own setPropProps(static, frozen,
// active, sleeping) on what its beam hit, which writes the flags and runs init(), and init() turns the
// body's physics off for a frozen or static prop. It ran on the tool user's machine only: everywhere else
// the prop kept falling, or stayed loose where it had been. Here that call, made from the tool, crosses
// with the four flags and the prop's pose at that moment; every other peer puts its copy at that pose and
// runs the same setter, so the prop is frozen everywhere at exactly the same place. A client's goes to the
// host, which applies it and sends it on. Wire: PropToolFlags. Gameplay/network layer. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropToolFlagsPayload;
}  // namespace coop::net

namespace coop::prop_tool_flags {

// Cache the session; the watch registers from Tick. Idempotent.
void Install(coop::net::Session* session);

// Register and settle the watch. Game thread, per pump tick.
void Tick();

// A peer's toolgun change: applied to this copy; the host relays it. Game thread.
void OnToolFlags(coop::net::Session& session, const coop::net::PropToolFlagsPayload& payload, uint8_t senderSlot);

}  // namespace coop::prop_tool_flags
