// coop/items/shovel_scrap.cpp -- see coop/items/shovel_scrap.h.

#include "coop/items/shovel_scrap.h"

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>

namespace coop::shovel_scrap {
namespace {

namespace sg = ue_wrap::script_gate;

// The two bodies a shovel's births happen inside, both reached Blueprint to Blueprint from the swing's
// montage notify, so the seam is the script gate's body loop.
UE_FACT_FN(kRowTurnIntoScrap, "shovel_scrap", L"actorChipPile_C", L"turnIntoScrap", Declared);
UE_FACT_FN(kRowThrowShovelItem, "shovel_scrap", L"mainPlayer_C", L"throwShovelItem", Declared);
constexpr int kTagScoop = 0x53484F31;  // 'SHO1'
constexpr int kTagThrow = 0x53484F32;  // 'SHO2'

// Deeper than the two bodies ever nest is a lost POST (a body some other watch cancelled), not a
// shovel: the depth is reset rather than left marking every later birth.
constexpr int kMaxDepth = 4;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_scoopWatched = false, g_throwWatched = false, g_saidLive = false;
int  g_depth = 0;
uint64_t g_bodies = 0;

bool OnClient() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

sg::Verdict OnBodyPre(const sg::Call&) {
    if (!OnClient()) return sg::Verdict::Run;
    if (++g_depth > kMaxDepth) {
        UE_LOGW("shovel_scrap: body depth %d past %d -- a POST was lost; reset", g_depth, kMaxDepth);
        g_depth = 1;
    }
    ++g_bodies;
    return sg::Verdict::Run;
}

void OnBodyPost(const sg::Call&) {
    if (g_depth > 0) --g_depth;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!g_scoopWatched)
        g_scoopWatched = sg::WatchName(kRowTurnIntoScrap, kTagScoop, &OnBodyPre, &OnBodyPost);
    if (!g_throwWatched)
        g_throwWatched = sg::WatchName(kRowThrowShovelItem, kTagThrow, &OnBodyPre, &OnBodyPost);
    if (g_saidLive || !g_scoopWatched || !g_throwWatched) return;
    sg::ResolvePendingNames();
    if (!sg::NameWatchSettled(kRowTurnIntoScrap, kTagScoop) || !sg::NameWatchSettled(kRowThrowShovelItem, kTagThrow))
        return;
    g_saidLive = true;
    const bool scoop = sg::NameWatchLive(kRowTurnIntoScrap, kTagScoop);
    const bool thrw  = sg::NameWatchLive(kRowThrowShovelItem, kTagThrow);
    if (scoop && thrw) UE_LOGI("shovel_scrap: the scoop and throw watches are live");
    else UE_LOGE("shovel_scrap: a watch settled dead (scoop=%d throw=%d) -- a client's shovel scrap stays on "
                 "its own screen", scoop ? 1 : 0, thrw ? 1 : 0);
}

bool InFlight() { return g_depth > 0; }

void OnDisconnect() {
    if (g_bodies) UE_LOGI("shovel_scrap: session end -- %llu shovel bodies ran on this client",
                          static_cast<unsigned long long>(g_bodies));
    g_depth = 0;
    g_bodies = 0;
}

}  // namespace coop::shovel_scrap
