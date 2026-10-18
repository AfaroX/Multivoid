// ue_wrap/devices/media_device.cpp -- see ue_wrap/devices/media_device.h.

#include "ue_wrap/devices/media_device.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstring>

namespace ue_wrap::media_device {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kTvClass    = L"prop_tv2_C";
constexpr const wchar_t* kRadioClass = L"prop_radio_internet_C";

// FTimespan counts 100 ns ticks.
constexpr int64_t kTicksPerMs = 10000;

struct FStr { wchar_t* data; int32_t num; int32_t max; };

bool HasClassInChain(void* actor, const wchar_t* name) {
    if (!actor) return false;
    for (void* cls = R::ClassOf(actor); cls; cls = R::SuperStructOf(cls))
        if (R::ToString(R::NameOf(cls)) == name) return true;
    return false;
}

// struct_tv's members, by their prefixes on the live struct: name, link, isUrl. The measured layout
// is the fallback when the struct does not resolve.
struct TvLayout { bool tried = false; int32_t name = 0, link = 16, isUrl = 32, size = 40; };
const TvLayout& Layout(void* openLinkFn) {
    static TvLayout l;
    if (l.tried || !openLinkFn) return l;
    l.tried = true;
    if (void* st = R::PropertyInnerStruct(openLinkFn, L"data")) {
        const int32_t n = R::FindPropertyOffsetByPrefix(st, L"name_");
        const int32_t k = R::FindPropertyOffsetByPrefix(st, L"link_");
        const int32_t u = R::FindPropertyOffsetByPrefix(st, L"isUrl_");
        if (n >= 0 && k >= 0 && u >= 0) { l.name = n; l.link = k; l.isUrl = u; }
    }
    const int32_t sz = R::FindParamSize(openLinkFn, L"data");
    if (sz > 0) l.size = sz;
    return l;
}

void* PlayerFn(void* player, const wchar_t* name) {
    return player ? R::FindFunction(R::ClassOf(player), name) : nullptr;
}

bool CallBool(void* player, const wchar_t* name, bool& out) {
    void* fn = PlayerFn(player, name);
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(player, f)) return false;
    out = f.Get<bool>(L"ReturnValue");
    return true;
}

std::wstring ReadStr(const uint8_t* at) {
    FStr s{};
    std::memcpy(&s, at, sizeof(s));
    if (!s.data || s.num <= 1 || s.num > 4096) return std::wstring();
    return std::wstring(s.data, static_cast<size_t>(s.num - 1));
}

}  // namespace

Kind KindOf(void* actor) {
    if (HasClassInChain(actor, kTvClass)) return Kind::Tv;
    if (HasClassInChain(actor, kRadioClass)) return Kind::Radio;
    return Kind::None;
}

void* PlayerOf(void* device) {
    static R::InstanceOffset sOff(L"mediaPlayer");
    if (!device) return nullptr;
    const int32_t off = sOff.Of(device);
    if (off < 0) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, static_cast<const uint8_t*>(device) + off, sizeof(p));
    return (p && R::IsLive(p)) ? p : nullptr;
}

bool OpenLink(void* device, const std::wstring& name, const std::wstring& link, bool isUrl, bool pause) {
    if (KindOf(device) == Kind::None) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(device), L"openLink");
    if (!fn) return false;
    ParamFrame f(fn);
    const int32_t off = f.ParamOffset(L"data");
    if (!f.valid() || off < 0) return false;
    const TvLayout& l = Layout(fn);
    // The frame's strings point at our buffers for the call's duration: the callee copies what it keeps.
    std::wstring n = name, k = link;
    FStr ns{n.empty() ? nullptr : n.data(), n.empty() ? 0 : static_cast<int32_t>(n.size() + 1),
            n.empty() ? 0 : static_cast<int32_t>(n.size() + 1)};
    FStr ks{k.empty() ? nullptr : k.data(), k.empty() ? 0 : static_cast<int32_t>(k.size() + 1),
            k.empty() ? 0 : static_cast<int32_t>(k.size() + 1)};
    auto* base = static_cast<uint8_t*>(f.data()) + off;
    if (l.name + 16 > l.size || l.link + 16 > l.size || l.isUrl + 1 > l.size) return false;
    std::memcpy(base + l.name, &ns, sizeof(ns));
    std::memcpy(base + l.link, &ks, sizeof(ks));
    base[l.isUrl] = isUrl ? 1 : 0;
    f.Set<uint8_t>(L"pausePlayer", pause ? 1 : 0);
    const bool ok = Call(device, f);
    // The strings are ours: clear the frame's headers so nothing reads them after the call.
    std::memset(base + l.name, 0, sizeof(FStr));
    std::memset(base + l.link, 0, sizeof(FStr));
    return ok;
}

bool ReadOpenLinkArgs(void* function, const uint8_t* locals, std::wstring& name, std::wstring& link,
                      bool& isUrl, bool& pause) {
    if (!function || !locals) return false;
    const int32_t off = R::FindParamOffset(function, L"data");
    const int32_t poff = R::FindParamOffset(function, L"pausePlayer");
    if (off < 0) return false;
    const TvLayout& l = Layout(function);
    name = ReadStr(locals + off + l.name);
    link = ReadStr(locals + off + l.link);
    isUrl = locals[off + l.isUrl] != 0;
    pause = poff >= 0 && locals[poff] != 0;
    return true;
}

bool Url(void* player, std::wstring& out) {
    void* fn = PlayerFn(player, L"GetUrl");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(player, f)) return false;
    FStr r{};
    if (!f.GetRaw(L"ReturnValue", &r, sizeof(r))) return false;
    out = (r.data && r.num > 1 && r.num <= 4096) ? std::wstring(r.data, static_cast<size_t>(r.num - 1)) : std::wstring();
    if (r.data) R::EngineFree(r.data);   // the call minted it; the frame has no destructor
    return true;
}

bool IsPlaying(void* player, bool& out) { return CallBool(player, L"IsPlaying", out); }
bool IsPreparing(void* player, bool& out) { return CallBool(player, L"IsPreparing", out); }

bool TimeMs(void* player, int64_t& out) {
    void* fn = PlayerFn(player, L"GetTime");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(player, f)) return false;
    out = f.Get<int64_t>(L"ReturnValue") / kTicksPerMs;
    return true;
}

bool Seek(void* player, int64_t ms) {
    void* fn = PlayerFn(player, L"Seek");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<int64_t>(L"Time", ms * kTicksPerMs);
    return Call(player, f);
}

bool Play(void* player) { bool ok = false; return CallBool(player, L"Play", ok) && ok; }

namespace {

void* ObjectAt(void* obj, R::InstanceOffset& off) {
    if (!obj) return nullptr;
    const int32_t o = off.Of(obj);
    if (o < 0) return nullptr;
    void* p = nullptr;
    std::memcpy(&p, static_cast<const uint8_t*>(obj) + o, sizeof(p));
    return (p && R::IsLive(p)) ? p : nullptr;
}

void* SoundOf(void* device) {
    static R::InstanceOffset sOff(L"MediaSound");
    return ObjectAt(device, sOff);
}

// A TV re-applies its own fields (volume, brightness, squeeze) through updProperties.
bool UpdProperties(void* device) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(device), L"updProperties");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(device, f);
}

}  // namespace

bool ReadVolume(void* device, float& out) {
    if (KindOf(device) == Kind::Tv) {
        static R::InstanceOffset sOff(L"audioVolume");
        const int32_t o = sOff.Of(device);
        if (o < 0) return false;
        int32_t v = 0;
        std::memcpy(&v, static_cast<const uint8_t*>(device) + o, sizeof(v));
        out = static_cast<float>(v) / 100.0f;
        return true;
    }
    void* snd = SoundOf(device);
    static R::InstanceOffset sMul(L"VolumeMultiplier");
    const int32_t o = snd ? sMul.Of(snd) : -1;
    if (o < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(snd) + o, sizeof(out));
    return true;
}

bool WriteVolume(void* device, float v) {
    if (v < 0.f) v = 0.f;
    if (KindOf(device) == Kind::Tv) {
        static R::InstanceOffset sOff(L"audioVolume");
        const int32_t o = sOff.Of(device);
        if (o < 0) return false;
        const int32_t iv = static_cast<int32_t>(v * 100.0f + 0.5f);
        std::memcpy(static_cast<uint8_t*>(device) + o, &iv, sizeof(iv));
        return UpdProperties(device);
    }
    void* snd = SoundOf(device);
    void* fn = snd ? R::FindFunction(R::ClassOf(snd), L"SetVolumeMultiplier") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<float>(L"NewVolumeMultiplier", v) && Call(snd, f);
}

bool ReadBrightness(void* device, float& out) {
    if (KindOf(device) != Kind::Tv) return false;
    static R::InstanceOffset sOff(L"brightness");
    const int32_t o = sOff.Of(device);
    if (o < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(device) + o, sizeof(out));
    return true;
}

bool WriteBrightness(void* device, float v) {
    if (KindOf(device) != Kind::Tv) return false;
    static R::InstanceOffset sOff(L"brightness");
    const int32_t o = sOff.Of(device);
    if (o < 0) return false;
    std::memcpy(static_cast<uint8_t*>(device) + o, &v, sizeof(v));
    return UpdProperties(device);
}

void* DeviceOfInterface(void* widget) {
    static R::InstanceOffset sOff(L"radio");
    void* d = ObjectAt(widget, sOff);
    return KindOf(d) == Kind::None ? nullptr : d;
}
bool Pause(void* player) { bool ok = false; return CallBool(player, L"Pause", ok) && ok; }

}  // namespace ue_wrap::media_device
