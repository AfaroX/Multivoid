// coop/media/media_frames.cpp -- see coop/media/media_frames.h.

#include "coop/media/media_frames.h"

#include "coop/net/session.h"
#include "coop/props/record_sync.h"   // NoteTouched: this client authors the frame's record for a while

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/custom_assets.h"

#include <atomic>

namespace coop::media_frames {
namespace {

namespace sg = ue_wrap::script_gate;

// The texture picker's apply on each image prop; the frame shapes inherit the wide frame's.
UE_FACT_FN(kRowPictureApply, "media_frames", L"prop_picture_h_C", L"intComs_texturePickerApply", Declared);
UE_FACT_FN(kRowPosterApply, "media_frames", L"prop_poster_C", L"intComs_texturePickerApply", Declared);
constexpr int kTagPicture = 0x4D465049, kTagPoster = 0x4D46504F;  // 'MFPI' 'MFPO'

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watched[2] = {}, g_saidLive[2] = {};

// CLIENT: before the pick changes the frame, so the change is this client's to send.
sg::Verdict OnApplyPre(const sg::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Client || c.fromOurCode || !c.object)
        return sg::Verdict::Run;
    coop::props::record_sync::NoteTouched(c.object);
    return sg::Verdict::Run;
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    const ue_wrap::facts::Function* rows[2] = {&kRowPictureApply, &kRowPosterApply};
    const int tags[2] = {kTagPicture, kTagPoster};
    bool pending = false;
    for (int i = 0; i < 2; ++i) {
        if (!g_watched[i]) g_watched[i] = sg::WatchName(*rows[i], tags[i], &OnApplyPre, nullptr);
        if (g_watched[i] && !g_saidLive[i]) pending = true;
    }
    if (!pending) return;
    sg::ResolvePendingNames();
    for (int i = 0; i < 2; ++i) {
        if (!g_watched[i] || g_saidLive[i] || !sg::NameWatchSettled(*rows[i], tags[i])) continue;
        g_saidLive[i] = true;
        if (sg::NameWatchLive(*rows[i], tags[i])) UE_LOGI("media_frames: the %ls picker watch is live", rows[i]->Class());
        else UE_LOGE("media_frames: the %ls picker watch settled dead -- a client's pick stays on its screen", rows[i]->Class());
    }
}

void OnRecordApplied(void* actor) {
    if (actor && ue_wrap::custom_assets::IsImageProp(actor)) ue_wrap::custom_assets::RefreshImage(actor);
}

}  // namespace coop::media_frames
