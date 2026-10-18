// ue_wrap/world/custom_assets.cpp -- see ue_wrap/world/custom_assets.h.

#include "ue_wrap/world/custom_assets.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::custom_assets {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kPictureClass = L"prop_picture_h_C";
constexpr const wchar_t* kPosterClass  = L"prop_poster_C";

bool HasClassInChain(void* actor, const wchar_t* name) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == name) return true;
    return false;
}

}  // namespace

std::wstring AssetFolder() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* lib = R::FindClassDefaultObject(L"lib_C");
    void* cls = R::FindClass(L"lib_C");
    void* fn = cls ? R::FindFunction(cls, L"GetAssetFolder") : nullptr;
    if (!gm || !lib || !fn) return std::wstring();
    ParamFrame f(fn);
    if (!f.valid()) return std::wstring();
    f.Set<uint8_t>(L"skipCreation", 1);
    f.Set<void*>(L"__WorldContext", gm);
    if (!Call(lib, f)) return std::wstring();
    struct FStr { wchar_t* data; int32_t num; int32_t max; } out{};
    if (!f.GetRaw(L"path", &out, sizeof(out)) || !out.data || out.num <= 1) return std::wstring();
    std::wstring path(out.data, static_cast<size_t>(out.num - 1));
    R::EngineFree(out.data);   // the call minted it; the frame has no destructor
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

bool Reimport() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    void* fn = gm ? R::FindFunction(R::ClassOf(gm), L"importAssets") : nullptr;
    if (!fn) return false;
    return R::CallFunction(gm, fn, nullptr);
}

bool IsImageProp(void* actor) {
    return HasClassInChain(actor, kPictureClass) || HasClassInChain(actor, kPosterClass);
}

bool RefreshImage(void* actor) {
    const wchar_t* fnName = HasClassInChain(actor, kPictureClass) ? L"upd"
                          : HasClassInChain(actor, kPosterClass)  ? L"settex"
                                                                  : nullptr;
    if (!fnName) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), fnName);
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(actor, f);
}

}  // namespace ue_wrap::custom_assets
