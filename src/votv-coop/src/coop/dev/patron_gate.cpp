// coop/dev/patron_gate.cpp -- see the header.
#include "coop/dev/patron_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/facts/function_row.h"

#include <cstdint>

namespace coop::dev::patron_gate {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace UH = ue_wrap::ufunction_hook;

UE_FACT_FN(kRowGetFilesFromDirectory, "dev.patron_gate", L"FileSDKBPLibrary", L"GetFilesFromDirectory", Declared);

ue_wrap::facts::Resolved g_fn;   // UFileSDKBPLibrary::GetFilesFromDirectory, once resolved
bool  g_done = false;     // armed, or given up for a named reason -- either way, stop trying
bool  g_off = false, g_offRead = false;
int   g_diagLines = 0;
constexpr int kMaxDiagLines = 6;

// Post-hook on GetFilesFromDirectory: fires after the original filled the returned TArray<FString>.
// When the call came from ExecuteUbergraph_mainGamemode -- the sole ubergraph caller, and the
// anti-mod scan -- empty the list so the gate's loops see no file. The name is class-suffixed, so it
// uniquely identifies that function without an address compare (robust across a world reload).
// Game thread, deep in native dispatch: no allocation, no engine call, SEH-guarded by the facility.
void OnGetFilesFromDirectory(void* /*context*/, void* /*sourceObject*/, void* /*resultDeref*/) {
    const UH::CallerFrame frame = UH::CurrentCallerFrame();
    if (!frame.function || !R::NameEquals(R::NameOf(frame.function), L"ExecuteUbergraph_mainGamemode"))
        return;  // a legitimate GetFilesFromDirectory (photos, printer meshes, textures) -- left alone
    void* result = UH::CurrentResult();   // &ReturnValue, the TArray<FString>
    if (!result) return;
    int32_t& num = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(result) + P::off::TArray_Num);
    if (g_diagLines < kMaxDiagLines) {
        ++g_diagLines;
        UE_LOGW("patron_gate: mod scan in mainGamemode -- GetFilesFromDirectory returned %d file(s); "
                "emptying the list so the Shadow-Test-Build mod gate finds nothing", num);
    }
    // ArrayNum = 0: every scan loop reads Length 0. The element buffers of this one call leak (a
    // handful of FStrings, a few times per BeginPlay) -- acceptable for a local dev-only instrument.
    num = 0;
}

}  // namespace

void EnsureArmed() {
    if (!g_offRead) {
        g_off = !coop::config::ResolveFlag(::coop::config_registry::rows::patron_mod_gate_bypass);
        g_offRead = true;
    }
    if (g_off || g_done) return;

    if (!g_fn) {
        void* cls = R::FindClass(kRowGetFilesFromDirectory.Class());
        if (!cls) return;  // the FileSDK plugin's class is not registered yet -- retried on a later call
        g_fn = ue_wrap::facts::Resolve(kRowGetFilesFromDirectory);
        if (!g_fn) {
            UE_LOGE("patron_gate: FileSDKBPLibrary resolved but GetFilesFromDirectory did not -- the "
                    "Shadow-Test-Build mod gate CANNOT be neutralised (the dll/pak scan stands)");
            g_done = true;   // the name is wrong for this build; do not spin
            return;
        }
    }
    if (UH::InstallPostHook(g_fn, &OnGetFilesFromDirectory)) {
        g_done = true;
        UE_LOGW("patron_gate: ARMED -- UFileSDKBPLibrary::GetFilesFromDirectory post-hook installed; the "
                "patron Shadow-Test-Build 'Modded client detected!' gate is neutralised. LOCAL port-prep "
                "instrument ([dev] patron_mod_gate_bypass); it never ships and edits no game file.");
    } else {
        UE_LOGE("patron_gate: the GetFilesFromDirectory Func patch did not install (fn=%p) -- gate stands", g_fn.Fn());
        g_done = true;
    }
}

}  // namespace coop::dev::patron_gate
