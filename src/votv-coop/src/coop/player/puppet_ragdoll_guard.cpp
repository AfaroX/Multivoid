// coop/player/puppet_ragdoll_guard.cpp -- see coop/player/puppet_ragdoll_guard.h.

#include "coop/player/puppet_ragdoll_guard.h"

#include "coop/player/players_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>

namespace coop::puppet_ragdoll_guard {
namespace {

namespace sg = ue_wrap::script_gate;

// Blueprint-internal on every route that matters -- the husk's punch reaches it with
// EX_LocalVirtualFunction -- so the seam is the script gate's body loop, not a dispatch detour.
UE_FACT_FN(kRowRagdollMode, "puppet_ragdoll_guard", L"mainPlayer_C", L"ragdollMode", Declared);

constexpr int kTag = 0x50524744;  // 'PRGD'

bool                  g_watchDone = false;
std::atomic<uint32_t> g_refused{0};

// IsPuppet alone, the shape coop/player/player_damage's refusals settled on: the local pawn reads
// null for a whole death or travel window, and "not the local pawn" would make this machine's own
// player immune to its own death for as long as it did. A puppet is refused whoever asks -- the
// origin flag spans every Blueprint call nested inside one of ours, so it proves nothing about
// which body the nested ragdoll was aimed at, and our own layer asks this verb of the local pawn
// alone anyway.
sg::Verdict OnRagdollModePre(const sg::Call& c) {
    if (!c.object || !coop::players::Registry::Get().IsPuppet(c.object)) return sg::Verdict::Run;
    const uint32_t n = g_refused.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || (n % 100) == 0) {
        UE_LOGI("puppet_ragdoll_guard: refused ragdollMode on a peer's puppet %p (#%u) -- its "
                "ragdoll is its owner's, and fallen() would write this machine's own death",
                c.object, n);
    }
    return sg::Verdict::Cancel;
}

}  // namespace

void Tick() {
    if (g_watchDone) return;
    g_watchDone = true;
    if (!sg::WatchName(kRowRagdollMode, kTag, &OnRagdollModePre, nullptr)) {
        UE_LOGE("puppet_ragdoll_guard: the ragdollMode watch did not register -- a peer's slip, "
                "fall or blast can run the death chain against this machine");
    }
}

}  // namespace coop::puppet_ragdoll_guard
