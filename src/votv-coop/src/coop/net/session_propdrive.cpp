// coop/net/session_propdrive.cpp -- the driven-prop pose batch, send and receive, for Session. Any peer
// sends the props it set moving; the host keeps a client's per origin and relays it to the others.
//
// The session_trashcarry.cpp shape. The LOCAL side is a QUEUE, not a snapshot: the publisher sends
// a prop's pose only when it moved since the last one it counted as sent, so the newest pose of
// EVERY prop has to go out on the next send -- a snapshot would drop the other props' poses
// whenever one prop published later in the tick. The queues and their rules are
// coop/net/eid_pose_lane.h; this file is the datagram's framing and the trust boundary.

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

#include "coop/net/protocol.h"  // PacketHeader / EntityPoseBatchHeader / PropPoseSnapshot / kMaxPropDriveBatchEntries

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::net {

PoseTurn Session::PropDrivePoseTurn(uint32_t eid) const {
    return propDrivePoses_.Turn(eid, kMaxPropDriveBatchEntries, /*ahead=*/false);
}

void Session::PublishPropDrivePose(const PropPoseSnapshot& pose) {
    propDrivePoses_.Publish(pose, /*ahead=*/false);
}

bool Session::TakeRemotePropDriveBatch(std::vector<PropPoseSnapshot>& out) {
    if (!connected()) return false;
    std::lock_guard<std::mutex> lk(remoteMutex_);
    std::vector<PropPoseSnapshot> one;
    bool any = false;
    for (int slot = 0; slot < kMaxPeers; ++slot) {
        one.clear();
        if (!origin_[slot].propDrive.Take(one)) continue;
        for (PropPoseSnapshot& e : one) {
            e.flags = static_cast<uint8_t>((e.flags & 0x0F) | (slot << 4));
            out.push_back(e);
        }
        any = true;
    }
    return any;
}

int Session::SerializeLocalPropDriveBatch(uint8_t* buf) {
    // One datagram = PacketHeader(20) + EntityPoseBatchHeader(4) + N*PropPoseSnapshot(64), capped at
    // kMaxPropDriveBatchEntries; the rest go out on the next send. The leading PacketHeader bytes
    // are left for the caller to stamp per-peer. Whichever peer set a prop moving publishes it.
    const int n = propDrivePoses_.Drain(buf + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                                        kMaxPropDriveBatchEntries);
    if (n == 0) return 0;
    EntityPoseBatchHeader bh{};
    bh.count = static_cast<uint8_t>(n);
    std::memcpy(buf + sizeof(PacketHeader), &bh, sizeof(bh));
    return static_cast<int>(sizeof(PacketHeader) + sizeof(bh) +
                            static_cast<size_t>(n) * sizeof(PropPoseSnapshot));
}

void Session::StoreRemotePropDriveBatch(int routeSlot, int peerSlot, const void* data, int len, uint32_t seq) {
    // A peer's driven-prop batch: on the host a client's own, kept under its slot and relayed to the
    // other clients; on a client the host's, or another client's the host relayed, under its origin.
    // The origin is the connection's on the host and the relay's stamp on a client, never a field the
    // sender writes; the FINITE check is the format's trust boundary, for the reasons session_trashcarry.cpp
    // states.
    if (routeSlot < 0 || routeSlot >= kMaxPeers) return;
    if (role() == Role::Host && routeSlot != peerSlot) return;   // a client speaks for itself alone
    if (len < static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader))) return;
    EntityPoseBatchHeader bh;
    std::memcpy(&bh, static_cast<const uint8_t*>(data) + sizeof(PacketHeader), sizeof(bh));
    const int count = bh.count;
    if (count > kMaxPropDriveBatchEntries) return;  // malformed
    const int need = static_cast<int>(sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader)) +
                     count * static_cast<int>(sizeof(PropPoseSnapshot));
    if (len < need) return;  // truncated datagram
    // A fixed array, since the count is capped by the format: no allocation per datagram.
    PropPoseSnapshot batch[kMaxPropDriveBatchEntries];
    if (count > 0)
        std::memcpy(batch,
                    static_cast<const uint8_t*>(data) + sizeof(PacketHeader) + sizeof(EntityPoseBatchHeader),
                    static_cast<size_t>(count) * sizeof(PropPoseSnapshot));
    // Strict on format: a batch carrying a non-finite pose, an over-long key or an eid of zero is
    // refused whole rather than filtered.
    for (int i = 0; i < count; ++i) {
        const PropPoseSnapshot& e = batch[i];
        if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z) ||
            !std::isfinite(e.pitch) || !std::isfinite(e.yaw) || !std::isfinite(e.roll) ||
            e.key.len > sizeof(e.key.data) || e.elementId == 0u) {
            static uint32_t sMalformed = 0;
            if (++sMalformed <= 3 || (sMalformed % 100) == 0)
                UE_LOGW("propdrive: malformed entry in a %d-entry batch -- dropping the batch (#%u)",
                        count, sMalformed);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(remoteMutex_);
        origin_[routeSlot].propDrive.Merge(batch, count, seq);
    }
    if (role() == Role::Host) RelayUnreliableToOtherClients(peerSlot, data, len);
}

}  // namespace coop::net
