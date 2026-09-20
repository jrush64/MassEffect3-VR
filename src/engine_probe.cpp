#include "engine_probe.h"
#include "calcview_hook.h"
#include "convo_fp.h"

#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <MinHook.h>

namespace
{
// --- LE3 engine globals / struct offsets ----------------------------------
// RVAs are from the ME3Tweaks unified SDK (LExSDKv2 Init.hpp, SDK_TARGET_LE3 block).
// If GEngine derefs to garbage on this build, the installed exe differs from the
// SDK's target build and the mod falls back to a pattern scan -- the log will say so.
constexpr std::uintptr_t kGEngineRva = 0x18b31d0;   // UEngine* GEngine (LE3)
constexpr std::uintptr_t kGObjectsRva = 0x1887e40;  // (catalogued for later)

// Offsets are confirmed byte-identical to LE1/LE3 from the SDK class layouts.
constexpr std::uintptr_t kEngineGamePlayers = 0x498;   // UGameEngine: TArray<ULocalPlayer*>
constexpr std::uintptr_t kLpViewState = 0x46C;
constexpr std::uintptr_t kLpViewportClient = 0x594;
constexpr std::uintptr_t kLpOrigin = 0x59C;   // FVector2D (2 floats)
constexpr std::uintptr_t kLpSize = 0x5A4;     // FVector2D (2 floats)
constexpr std::uintptr_t kLpControllerId = 0x5B4;   // int
constexpr std::uintptr_t kVpcFViewportClientVtable = 0x60;   // UGameViewportClient: FViewportClient vtable
constexpr int kDrawVtableSlot = 2;            // FViewportClient::Draw(FViewport*, FCanvas*)

// UE3 TArray<T> = { T* Data; int Num; int Max; }
constexpr std::uintptr_t kTArrayData = 0x0;
constexpr std::uintptr_t kTArrayNum = 0x8;

std::atomic_bool g_dumped{false};
std::atomic<unsigned long long> g_waitLogs{0};

// All raw reads of possibly-invalid game pointers go through these SEH-guarded helpers.
// Keep them free of C++ objects so the compiler accepts __try here.
bool SafeReadPtr(std::uintptr_t addr, std::uintptr_t* out) noexcept
{
    __try
    {
        *out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadInt(std::uintptr_t addr, int* out) noexcept
{
    __try
    {
        *out = *reinterpret_cast<volatile int*>(addr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SafeReadFloat2(std::uintptr_t addr, float* out2) noexcept
{
    __try
    {
        out2[0] = *reinterpret_cast<volatile float*>(addr);
        out2[1] = *reinterpret_cast<volatile float*>(addr + 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

std::string Hex(std::uintptr_t v)
{
    char buf[32] = {};
    sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// --- UObject reflection (LE3): resolve an object's class name from memory ------------------
// SFXName name pool (GBioNamePools) at MassEffect3.exe + 0x17B33D0 (array of chunk base pointers).
// SFXName(8B): first DWORD packed = Offset:29 | Chunk:3. Entry = chunk[Offset]; ANSI string at +12
// (SFXPackedIndex 4 + HashNext 8). UObject.Name@0x48, UObject.Class@0x50, UClass.Name@0x48.
constexpr std::uintptr_t kNamePoolsRva = 0x17B33D0;
constexpr std::uintptr_t kObjName = 0x48;
constexpr std::uintptr_t kObjClass = 0x50;

bool ReadSfxName(unsigned long long sfxname, char* out, int outSize) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned int packed = static_cast<unsigned int>(sfxname & 0xFFFFFFFFull);
        const unsigned int offset = packed & 0x1FFFFFFFu;
        const unsigned int chunk = (packed >> 29) & 0x7u;
        auto* pools = reinterpret_cast<unsigned char* volatile*>(base + kNamePoolsRva);
        unsigned char* pool = pools[chunk];
        if (pool == nullptr) return false;
        const char* ansi = reinterpret_cast<const char*>(pool + offset + 12);   // Index(4)+HashNext(8)
        int i = 0;
        for (; i < outSize - 1 && ansi[i] >= 32 && ansi[i] < 127; ++i) out[i] = ansi[i];
        out[i] = 0;
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Resolve obj->Class->Name into out. Returns false on any bad read.
bool ClassNameOf(std::uintptr_t obj, char* out, int outSize) noexcept
{
    if (obj < 0x10000) return false;
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(obj + kObjClass, &cls) || cls < 0x10000) return false;
    unsigned long long nm = 0;
    __try { nm = *reinterpret_cast<unsigned long long volatile*>(cls + kObjName); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ReadSfxName(nm, out, outSize);
}

// True if an AActor is being destroyed (LE3: bDeleteMe 0x25C bit8 / bPendingDelete 0x260 bit 0x800000).
bool ActorIsDying(std::uintptr_t actor) noexcept
{
    if (actor < 0x10000) return true;
    __try
    {
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x25C) & 0x00000008u) return true;
        if (*reinterpret_cast<std::uint32_t volatile*>(actor + 0x260) & 0x00800000u) return true;
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// ===== First-person (ported from ME2, LE3 offsets) ==========================================
std::atomic_bool g_fpEnabled{false};
std::atomic_bool g_meshHidden{false};
// MEASURED, not ported: the full PlayerController census ([PCSLOTS]) reported +E28=SFXPlayerCamera and
// +C38=SFXGameModeManager. The previous 0x684 here was an untested guess carried over from LE2-shaped
// thinking, and it pointed at nothing.
constexpr std::uintptr_t kPcPlayerCamera     = 0xE28;   // APlayerController.PlayerCamera -> SFXPlayerCamera
constexpr std::uintptr_t kPcGameModeManager  = 0xC38;   // -> SFXGameModeManager (the EGameModes chain)
constexpr std::uintptr_t kGmmCurrentMode     = 0x0A8;   // USFXGameModeManager.CurrentMode (byte)
constexpr std::uintptr_t kCamCurrentMode = 0x558;   // ASFXCameraNativeBase.CurrentCameraMode
constexpr std::uintptr_t kModeOffset     = 0xB4;    // USFXCameraMode.Offset (FVector)
constexpr std::uintptr_t kModeCollision  = 0x11C;   // USFXCameraMode flag dword
constexpr std::uintptr_t kPcIgnoreMove = 0x781;   // APlayerController.bIgnoreMoveInput (LE3 SDK)
constexpr std::uintptr_t kPcIgnoreLook = 0x782;   // APlayerController.bIgnoreLookInput (LE3 SDK)
constexpr std::uint32_t  kModeCollisionEnabled = 0x02u;   // bCollisionEnabled (gates DoCameraCollision)
constexpr std::uintptr_t kPcPawn  = 0x3BC;          // AController.Pawn
constexpr std::uintptr_t kPawnMesh = 0x3F4;         // APawn.Mesh (body)

struct FpStateDef { const char* cls; const char* label; ME2VR::EngineProbe::FpStateCfg def; };
const FpStateDef g_fpDefs[] = {
    { "SFXCameraMode_Explore",      "Explore (unarmed)",   { true, true, false, false, 35.0f, 0.0f, 65.0f } },
    { "SFXCameraMode_ExploreStorm", "Explore sprint",      { true, true, false, false, 35.0f, 0.0f, 65.0f } },
    { "SFXCameraMode_Combat",       "Combat (weapon out)", { true, true, false, false, 35.0f, 0.0f, 65.0f } },
    { "SFXCameraMode_CombatStorm",  "Combat sprint",       { true, true, false, false, 35.0f, 0.0f, 65.0f } },
    // Aim down sights. ME2 has this state and ME3 never got it, which is why there were no ADS
    // controls in the menu at all. LE3 keeps the mode name Combat while aiming (ADS is a zoom, not a
    // separate camera), so this entry exists for the transition target and for its own sliders.
    { "SFXCameraMode_TightAim",     "Aim down sights",     { true, true, false, false, 30.0f, 5.0f, 60.0f } },
};
constexpr int kFpCount = static_cast<int>(sizeof(g_fpDefs) / sizeof(g_fpDefs[0]));
ME2VR::EngineProbe::FpStateCfg g_fpStates[kFpCount] = {};
bool g_fpStatesInit = false;
void FpEnsureInit() noexcept { if (g_fpStatesInit) return; for (int i = 0; i < kFpCount; ++i) g_fpStates[i] = g_fpDefs[i].def; g_fpStatesInit = true; }
int FpStateIndexForMode(const char* cls) noexcept
{
    if (cls == nullptr) return -1;
    for (int i = 0; i < kFpCount; ++i) if (std::strcmp(cls, g_fpDefs[i].cls) == 0) return i;
    return -1;
}

// Class-keyed vanilla-offset cache (load-safe: no pointers). Captured before first FP write; restored on off.
// lx/ly/lz = the last value FIRST PERSON wrote into this class, so the release path can tell whether
// the field is still the mod's or the game has repositioned this camera since ([COVEROFFSET]).
struct ClassVanilla { char cls[64]; float x, y, z; float lx, ly, lz; bool written; };
ClassVanilla g_vanilla[16] = {};
int g_vanillaCount = 0;
void CaptureVanilla(const char* cls, float x, float y, float z) noexcept
{
    if (cls == nullptr || cls[0] == '\0') return;
    for (int i = 0; i < g_vanillaCount; ++i) if (std::strcmp(g_vanilla[i].cls, cls) == 0) return;
    if (g_vanillaCount >= static_cast<int>(sizeof(g_vanilla) / sizeof(g_vanilla[0]))) return;
    ClassVanilla& v = g_vanilla[g_vanillaCount++];
    std::strncpy(v.cls, cls, sizeof(v.cls) - 1); v.cls[sizeof(v.cls) - 1] = '\0'; v.x = x; v.y = y; v.z = z;
}
bool LookupVanilla(const char* cls, float* x, float* y, float* z) noexcept
{
    if (cls == nullptr) return false;
    for (int i = 0; i < g_vanillaCount; ++i)
        if (std::strcmp(g_vanilla[i].cls, cls) == 0) { *x = g_vanilla[i].x; *y = g_vanilla[i].y; *z = g_vanilla[i].z; return true; }
    return false;
}
void MarkLastWrite(const char* cls, float x, float y, float z) noexcept
{
    if (cls == nullptr) return;
    for (int i = 0; i < g_vanillaCount; ++i)
        if (std::strcmp(g_vanilla[i].cls, cls) == 0)
        { g_vanilla[i].lx = x; g_vanilla[i].ly = y; g_vanilla[i].lz = z; g_vanilla[i].written = true; return; }
}
bool LookupLastWrite(const char* cls, float* x, float* y, float* z) noexcept
{
    if (cls == nullptr) return false;
    for (int i = 0; i < g_vanillaCount; ++i)
        if (std::strcmp(g_vanilla[i].cls, cls) == 0 && g_vanilla[i].written)
        { *x = g_vanilla[i].lx; *y = g_vanilla[i].ly; *z = g_vanilla[i].lz; return true; }
    return false;
}
inline float AbsF(float v) noexcept { return v < 0.0f ? -v : v; }

bool FpApplyOffset(std::uintptr_t mode, bool on, float x, float y, float z, const char* cls) noexcept
{
    // This layout is valid only for USFXCameraMode-derived objects. In particular,
    // BioCameraBehaviorGalaxy also appears in PlayerCamera.CurrentCameraMode, but +0xB4/+0x11C
    // are not its Offset/collision fields. Refuse the wrong camera family here even if a caller
    // accidentally lets a menu/cinematic mode through.
    if (mode < 0x10000 || cls == nullptr || std::strncmp(cls, "SFXCameraMode_", 14) != 0) return false;
    __try
    {
        volatile float* off = reinterpret_cast<volatile float*>(mode + kModeOffset);
        volatile std::uint32_t* coll = reinterpret_cast<volatile std::uint32_t*>(mode + kModeCollision);
        if (on) { CaptureVanilla(cls, off[0], off[1], off[2]); off[0] = x; off[1] = y; off[2] = z; MarkLastWrite(cls, x, y, z); *coll &= ~kModeCollisionEnabled; }
        else { float vx = 0, vy = 0, vz = 0; if (LookupVanilla(cls, &vx, &vy, &vz)) { off[0] = vx; off[1] = vy; off[2] = vz; *coll |= kModeCollisionEnabled; } }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [COVEROFFSET 2026-08-21] Hand the camera back to the game on a cover handover.
// Cover MUST end up in third person - that is the whole point of the feature, and it is what ME2
// does. The first cut of this function only re-enabled collision and left Offset alone, which left
// THE MOD'S first-person eye offset sitting in SFXCameraMode_Combat for the entire time the player was in
// cover: first person in cover, camera above Shepard, exactly what it was supposed to fix.
// The real hazard the ordinary restore has is different and narrower: LE3 has no cover camera class
// (ME2 does - it never touches ME2's cover modes at all), so cover and standing SHARE
// SFXCameraMode_Combat. If the game has already repositioned that Offset for the lean/peek, writing
// the mod's standing snapshot over it teleports the camera.
// So: check ownership. If Offset still holds exactly what first person last wrote, it is still the mod's
// and vanilla goes back (third person, correct). If it has changed, the game has taken the field
// over - leave it alone. Either way collision comes back on, because that flag is only ever the mod's.
bool FpReleaseToGame(std::uintptr_t mode, const char* cls) noexcept
{
    if (mode < 0x10000 || cls == nullptr || std::strncmp(cls, "SFXCameraMode_", 14) != 0) return false;
    __try
    {
        volatile float* off = reinterpret_cast<volatile float*>(mode + kModeOffset);
        volatile std::uint32_t* coll = reinterpret_cast<volatile std::uint32_t*>(mode + kModeCollision);
        *coll |= kModeCollisionEnabled;
        float vx = 0, vy = 0, vz = 0, lx = 0, ly = 0, lz = 0;
        if (!LookupVanilla(cls, &vx, &vy, &vz) || !LookupLastWrite(cls, &lx, &ly, &lz)) return true;
        const bool stillOurs = AbsF(off[0] - lx) < 0.05f && AbsF(off[1] - ly) < 0.05f && AbsF(off[2] - lz) < 0.05f;
        if (stillOurs) { off[0] = vx; off[1] = vy; off[2] = vz; }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ProcessEvent (LE3 +0xF4F80) + GObjects (LE3 +0x1887E40) -> call native SetOwnerNoSee (marks proxy dirty).
using tProcessEvent = void(__fastcall*)(void*, void*, void*, void*);
void* g_setOwnerNoSeeFn = nullptr;
bool g_fnSearched = false;
// [MARKERVR] Exact native hook only -- never detour global ProcessEvent. The LE3 SDK identifies
// UFunction::Func@+0xD8 and FFrame::Locals@+0x30. Locals points at this invocation's parameter block,
// whose first 0x50 bytes are FSFXGUISceneView for SFXGUIValue_MarkerObjective.Update. Hook Update,
// because the native implementation calls CalculateMarkerPosition directly and bypasses its UFunction.
std::atomic<std::uintptr_t> g_markerObjectiveUpdateUFunction{0};
std::atomic<std::uintptr_t> g_markerCalculateUFunction{0};
std::atomic<std::uintptr_t> g_markerGetGuiSceneViewUFunction{0};
std::atomic<std::uintptr_t> g_markerWorldToScreenUFunction{0};
std::atomic<std::uintptr_t> g_markerWorldToSafeUFunction{0};
std::atomic_bool g_markerCalculateDumped{false};
std::atomic_bool g_markerUpdateDumped{false};
std::atomic_bool g_markerGetViewDumped{false};
std::atomic_bool g_markerWorldToScreenDumped{false};
std::atomic_bool g_markerWorldToSafeDumped{false};

using MarkerNativeFn = void(__fastcall*)(void*, void*, void*);
MarkerNativeFn g_origMarkerNative = nullptr;
std::atomic_bool g_markerNativeHookAttempted{false};
std::atomic_bool g_markerUpdateEnteredLogged{false};
std::atomic_bool g_markerCorrectionLogged{false};

void __fastcall MarkerObjectiveUpdateNativeDetour(void* context, void* stack, void* result) noexcept
{
    std::uintptr_t locals = 0;
    const bool haveLocals = stack != nullptr &&
        SafeReadPtr(reinterpret_cast<std::uintptr_t>(stack) + 0x30, &locals) && locals >= 0x10000;
    if (!g_markerUpdateEnteredLogged.exchange(true, std::memory_order_acq_rel))
        ME2VR::Log::Line(haveLocals ? "[MARKERVR] objective Update native entered with parameter locals"
                                    : "[MARKERVR] objective Update native entered but locals unreadable");
    if (haveLocals && ME2VR::CalcViewHook::CorrectWorldMarkerSceneView(reinterpret_cast<void*>(locals)) &&
        !g_markerCorrectionLogged.exchange(true, std::memory_order_acq_rel))
    {
        ME2VR::Log::Line("[MARKERVR] corrected objective Update call-local scene view");
    }
    if (g_origMarkerNative != nullptr) g_origMarkerNative(context, stack, result);
}

void InstallMarkerNativeHookIfReady() noexcept
{
    const std::uintptr_t ufn = g_markerObjectiveUpdateUFunction.load(std::memory_order_acquire);
    if (ufn < 0x10000 || g_markerNativeHookAttempted.exchange(true, std::memory_order_acq_rel)) return;

    std::uintptr_t target = 0;
    if (!SafeReadPtr(ufn + 0xD8, &target) || target < 0x10000)
    {
        ME2VR::Log::Line("[MARKERVR] objective Update UFunction target unreadable; hook inactive");
        return;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    const SIZE_T queried = VirtualQuery(reinterpret_cast<void*>(target), &mbi, sizeof(mbi));
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (queried == 0 || mbi.State != MEM_COMMIT || (mbi.Protect & execMask) == 0)
    {
        ME2VR::Log::Line("[MARKERVR] rejected non-executable native target; hook inactive");
        return;
    }
    MH_Initialize();
    const bool ok = MH_CreateHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(&MarkerObjectiveUpdateNativeDetour),
                                  reinterpret_cast<void**>(&g_origMarkerNative)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK;
    ME2VR::Log::Line(ok ? "[MARKERVR] exact objective Update native hook installed"
                        : "[MARKERVR] exact native hook install FAILED");
    if (!ok) g_origMarkerNative = nullptr;
}

bool ReadNativeBytes(std::uintptr_t target, unsigned char* out, int count) noexcept
{
    __try
    {
        for (int i = 0; i < count; ++i) out[i] = *reinterpret_cast<unsigned char volatile*>(target + i);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void DumpMarkerNativeOne(const char* label, std::atomic<std::uintptr_t>& slot,
                         std::atomic_bool& dumped) noexcept
{
    const std::uintptr_t ufn = slot.load(std::memory_order_acquire);
    if (ufn < 0x10000 || dumped.exchange(true, std::memory_order_acq_rel)) return;
    std::uintptr_t target = 0;
    if (!SafeReadPtr(ufn + 0xD8, &target) || target < 0x10000) return;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    int flags = 0, packed = 0;
    SafeReadInt(ufn + 0xE0, &flags);
    SafeReadInt(ufn + 0xE4, &packed);
    char head[260] = {};
    sprintf_s(head, "[MARKERNATIVE] %s ufn=0x%llX target=exe+0x%llX flags=0x%08X native_parms=0x%08X",
              label, static_cast<unsigned long long>(ufn),
              static_cast<unsigned long long>(target - base), flags, packed);
    ME2VR::Log::Line(head);
    unsigned char bytes[256] = {};
    if (!ReadNativeBytes(target, bytes, sizeof(bytes))) return;
    for (int row = 0; row < 4; ++row)
    {
        char line[420] = {};
        int used = sprintf_s(line, "[MARKERNATIVE] %s +%03X:", label, row * 64);
        for (int i = 0; i < 64; ++i)
            used += sprintf_s(line + used, sizeof(line) - used, "%02X", bytes[row * 64 + i]);
        ME2VR::Log::Line(line);
    }
}

void DumpMarkerNativeTargets() noexcept
{
    DumpMarkerNativeOne("Marker.CalculateMarkerPosition", g_markerCalculateUFunction, g_markerCalculateDumped);
    DumpMarkerNativeOne("MarkerObjective.Update", g_markerObjectiveUpdateUFunction, g_markerUpdateDumped);
    DumpMarkerNativeOne("SFXGUIMovie.GetGUISceneView", g_markerGetGuiSceneViewUFunction, g_markerGetViewDumped);
    DumpMarkerNativeOne("SFXGUIMovie.WorldToScreenFast", g_markerWorldToScreenUFunction, g_markerWorldToScreenDumped);
    DumpMarkerNativeOne("SFXGUIMovie.WorldToSafePostProjectionFast", g_markerWorldToSafeUFunction, g_markerWorldToSafeDumped);
}

// [MARKERVR] The captured MarkerObjective.Update wrapper ends in the class-specific virtual call
// `this->vtable[0x2B8](this, SceneView)`. Native marker management uses that same virtual directly,
// which is why both UFunction thunk hooks stayed silent. Resolve the slot from a live SP objective
// marker and gate the detour back to that exact UClass, so no other gameplay HUD scene view changes.
constexpr std::uintptr_t kMarkerUpdateVtableOffset = 0x2B8;
std::atomic<std::uintptr_t> g_markerObjectiveInstance{0};
std::atomic<std::uintptr_t> g_markerObjectiveClass{0};
using MarkerObjectiveUpdateDirectFn = void(__fastcall*)(void*, void*);
MarkerObjectiveUpdateDirectFn g_origMarkerObjectiveUpdateDirect = nullptr;
std::atomic_bool g_markerObjectiveDirectHookAttempted{false};
std::atomic_bool g_markerObjectiveDirectEnteredLogged{false};
std::atomic_bool g_markerObjectiveBytesDumped{false};
std::atomic_bool g_markerObjectiveMathDumped{false};
std::atomic<unsigned long long> g_markerObjectiveUpdateCalls{0};

void DumpMarkerObjectiveDirectBytes(std::uintptr_t target) noexcept
{
    if (target < 0x10000 || g_markerObjectiveBytesDumped.exchange(true, std::memory_order_acq_rel)) return;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char head[128] = {};
    sprintf_s(head, "[MARKERIMPL] Objective.Update direct target=exe+0x%llX bytes=2048",
              static_cast<unsigned long long>(target - base));
    ME2VR::Log::Line(head);
    unsigned char bytes[2048] = {};
    if (!ReadNativeBytes(target, bytes, sizeof(bytes))) return;
    for (int row = 0; row < 32; ++row)
    {
        char line[420] = {};
        int used = sprintf_s(line, "[MARKERIMPL] +%03X:", row * 64);
        for (int i = 0; i < 64; ++i)
            used += sprintf_s(line + used, sizeof(line) - used, "%02X", bytes[row * 64 + i]);
        ME2VR::Log::Line(line);
    }
}

bool CopyMarkerFloats(std::uintptr_t address, float* out, int count) noexcept
{
    __try
    {
        for (int i = 0; i < count; ++i) out[i] = *reinterpret_cast<float volatile*>(address + i * 4);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsExecutableAddress(std::uintptr_t target) noexcept
{
    MEMORY_BASIC_INFORMATION mbi = {};
    const SIZE_T queried = VirtualQuery(reinterpret_cast<void*>(target), &mbi, sizeof(mbi));
    const DWORD execMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return queried != 0 && mbi.State == MEM_COMMIT && (mbi.Protect & execMask) != 0;
}

void __fastcall MarkerObjectiveUpdateDirectDetour(void* marker, void* sceneView) noexcept
{
    std::uintptr_t cls = 0;
    const bool exactObjectiveClass = marker != nullptr &&
        SafeReadPtr(reinterpret_cast<std::uintptr_t>(marker) + kObjClass, &cls) &&
        cls == g_markerObjectiveClass.load(std::memory_order_acquire);
    const int yawUU = ME2VR::CalcViewHook::HeadLookYawUU();
    const int pitchUU = ME2VR::CalcViewHook::HeadLookPitchUU();
    const bool movedHead = yawUU > 700 || yawUU < -700 || pitchUU > 700 || pitchUU < -700;
    const bool captureMath = exactObjectiveClass && movedHead &&
        !g_markerObjectiveMathDumped.exchange(true, std::memory_order_acq_rel);
    alignas(16) float privateSceneView[20] = {};
    const bool havePrivateView = sceneView != nullptr &&
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(sceneView), privateSceneView, 20);
    void* objectiveSceneView = havePrivateView ? privateSceneView : sceneView;
    float matrixBefore[16] = {}, matrixAfter[16] = {}, viewLocation[3] = {};
    float diBefore[2] = {}, diAfter[2] = {};
    if (captureMath)
    {
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(sceneView), matrixBefore, 16);
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(sceneView) + 0x40, viewLocation, 3);
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(marker) + 0xA0, diBefore, 2);
    }
    if (exactObjectiveClass)
    {
        if (!g_markerObjectiveDirectEnteredLogged.exchange(true, std::memory_order_acq_rel))
            ME2VR::Log::Line("[MARKERVR] direct SP objective Update entered");
        if (ME2VR::CalcViewHook::CorrectWorldMarkerSceneView(objectiveSceneView) &&
            !g_markerCorrectionLogged.exchange(true, std::memory_order_acq_rel))
        {
            ME2VR::Log::Line("[MARKERVR] corrected SP objective marker scene view");
        }
        if (captureMath)
            CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(objectiveSceneView), matrixAfter, 16);
    }
    if (g_origMarkerObjectiveUpdateDirect != nullptr)
        g_origMarkerObjectiveUpdateDirect(marker, objectiveSceneView);

    // Cadence proof: if this is not called continuously, changing its scene view can only seed the
    // marker once and Scaleform will subsequently carry that screen-space position with the head.
    // If it is called every frame and these final DI coordinates move with the HMD, the remaining
    // owner is the later GFx display-info write rather than marker projection.
    const unsigned long long updateCalls =
        g_markerObjectiveUpdateCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    static std::atomic<unsigned long long> s_markerRateLastMs{0};
    static std::atomic<unsigned long long> s_markerRateLastCalls{0};
    const unsigned long long nowMs = GetTickCount64();
    unsigned long long lastMs = s_markerRateLastMs.load(std::memory_order_relaxed);
    if (nowMs - lastMs >= 2000 &&
        s_markerRateLastMs.compare_exchange_strong(lastMs, nowMs, std::memory_order_acq_rel))
    {
        const unsigned long long previousCalls =
            s_markerRateLastCalls.exchange(updateCalls, std::memory_order_acq_rel);
        float liveDi[2] = {};
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(marker) + 0xA0, liveDi, 2);
        char cadence[224] = {};
        sprintf_s(cadence,
                  "[MARKERRATE] calls=%llu/2s total=%llu yawUU=%d pitchUU=%d finalDI=(%.3f,%.3f) exact=%d",
                  updateCalls - previousCalls, updateCalls, yawUU, pitchUU,
                  static_cast<double>(liveDi[0]), static_cast<double>(liveDi[1]),
                  exactObjectiveClass ? 1 : 0);
        ME2VR::Log::Line(cadence);
    }
    if (captureMath)
    {
        CopyMarkerFloats(reinterpret_cast<std::uintptr_t>(marker) + 0xA0, diAfter, 2);
        std::uintptr_t actor = 0;
        SafeReadPtr(reinterpret_cast<std::uintptr_t>(marker) + 0x18C, &actor);
        char line[512] = {};
        sprintf_s(line, "[MARKERMATH] yawUU=%d pitchUU=%d marker=0x%llX actor=0x%llX scene=0x%llX viewLoc=(%.3f,%.3f,%.3f) DI=(%.3f,%.3f)->(%.3f,%.3f)",
                  yawUU, pitchUU, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(marker)),
                  static_cast<unsigned long long>(actor),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sceneView)),
                  viewLocation[0], viewLocation[1], viewLocation[2],
                  diBefore[0], diBefore[1], diAfter[0], diAfter[1]);
        ME2VR::Log::Line(line);
        for (int row = 0; row < 4; ++row)
        {
            sprintf_s(line, "[MARKERMATH] row%d before=(%.6f,%.6f,%.6f,%.6f) after=(%.6f,%.6f,%.6f,%.6f)", row,
                      matrixBefore[row*4+0], matrixBefore[row*4+1], matrixBefore[row*4+2], matrixBefore[row*4+3],
                      matrixAfter[row*4+0], matrixAfter[row*4+1], matrixAfter[row*4+2], matrixAfter[row*4+3]);
            ME2VR::Log::Line(line);
        }
    }
}

void InstallMarkerObjectiveDirectHookIfReady() noexcept
{
    const std::uintptr_t instance = g_markerObjectiveInstance.load(std::memory_order_acquire);
    const std::uintptr_t expectedClass = g_markerObjectiveClass.load(std::memory_order_acquire);
    if (instance < 0x10000 || expectedClass < 0x10000 ||
        g_markerObjectiveDirectHookAttempted.load(std::memory_order_acquire)) return;

    std::uintptr_t actualClass = 0, vtable = 0, target = 0;
    if (!SafeReadPtr(instance + kObjClass, &actualClass) || actualClass != expectedClass ||
        !SafeReadPtr(instance, &vtable) || vtable < 0x10000 ||
        !SafeReadPtr(vtable + kMarkerUpdateVtableOffset, &target) || !IsExecutableAddress(target))
        return; // object may have died between census and install; retry on the next scan

    DumpMarkerObjectiveDirectBytes(target);
    if (g_markerObjectiveDirectHookAttempted.exchange(true, std::memory_order_acq_rel)) return;
    MH_Initialize();
    const bool ok = MH_CreateHook(reinterpret_cast<void*>(target),
                                  reinterpret_cast<void*>(&MarkerObjectiveUpdateDirectDetour),
                                  reinterpret_cast<void**>(&g_origMarkerObjectiveUpdateDirect)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    char line[160] = {};
    sprintf_s(line, ok ? "[MARKERVR] SP objective-only Update hook installed at exe+0x%llX"
                       : "[MARKERVR] SP objective-only Update hook install FAILED at exe+0x%llX",
              static_cast<unsigned long long>(target - base));
    ME2VR::Log::Line(line);
    if (!ok) g_origMarkerObjectiveUpdateDirect = nullptr;
}

void* FindSetOwnerNoSeeFn() noexcept
{
    if (g_fnSearched) return g_setOwnerNoSeeFn;
    g_fnSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x1887E40);
        const int num = *reinterpret_cast<int volatile*>(base + 0x1887E40 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return nullptr;
        char nm[64] = {}, onm[64] = {};
        for (int i = 0; i < num; ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName);
            if (!ReadSfxName(n, nm, sizeof(nm)) || std::strcmp(nm, "SetOwnerNoSee") != 0) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjName);
            if (ReadSfxName(on, onm, sizeof(onm)) && std::strcmp(onm, "PrimitiveComponent") == 0) { g_setOwnerNoSeeFn = reinterpret_cast<void*>(obj); break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return g_setOwnerNoSeeFn;
}
void FpSetOwnerNoSee(std::uintptr_t comp, bool on) noexcept
{
    void* fn = FindSetOwnerNoSeeFn();
    if (fn == nullptr || comp < 0x10000) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xF4F80);
        unsigned long parm = on ? 1ul : 0ul;
        pe(reinterpret_cast<void*>(comp), fn, &parm, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void* g_primSetHiddenFn = nullptr;
void* g_disableDofFn = nullptr;
bool g_convoFnsSearched = false;
void FindConvoFunctions() noexcept
{
    if (g_convoFnsSearched) return;
    g_convoFnsSearched = true;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjectsRva);
        const int num = *reinterpret_cast<int volatile*>(base + kGObjectsRva + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return;
        for (int i = 0; i < num && (!g_primSetHiddenFn || !g_disableDofFn); ++i)
        {
            const std::uintptr_t obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (obj < 0x10000) continue;
            char name[64] = {}, owner[64] = {};
            const unsigned long long n = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName);
            if (!ReadSfxName(n, name, sizeof(name))) continue;
            const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
            if (outer < 0x10000) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(outer + kObjName);
            if (!ReadSfxName(on, owner, sizeof(owner))) continue;
            if (!g_primSetHiddenFn && std::strcmp(name, "SetHidden") == 0 && std::strcmp(owner, "PrimitiveComponent") == 0)
                g_primSetHiddenFn = reinterpret_cast<void*>(obj);
            else if (!g_disableDofFn && std::strcmp(name, "DisableDOF") == 0 && std::strcmp(owner, "BioPlayerController") == 0)
                g_disableDofFn = reinterpret_cast<void*>(obj);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void CallPrimSetHidden(std::uintptr_t comp, bool on) noexcept
{
    FindConvoFunctions();
    if (comp < 0x10000 || !g_primSetHiddenFn) return;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xF4F80);
        unsigned long parm = on ? 1ul : 0ul;
        pe(reinterpret_cast<void*>(comp), g_primSetHiddenFn, &parm, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
int FpHideMeshes(std::uintptr_t pawn, bool hideHead, bool hideBody) noexcept
{
    int count = 0;
    __try
    {
        std::uintptr_t body = *reinterpret_cast<std::uintptr_t volatile*>(pawn + kPawnMesh);
        if (body >= 0x10000) { FpSetOwnerNoSee(body, hideBody); ++count; }
        const std::uintptr_t headOffs[5] = { 0xB38, 0xB48, 0xB50, 0xB58, 0xB60 };  // Head, Hair, HeadGear, Visor, FacePlate
        for (int i = 0; i < 5; ++i) { std::uintptr_t c = *reinterpret_cast<std::uintptr_t volatile*>(pawn + headOffs[i]); if (c >= 0x10000) { FpSetOwnerNoSee(c, hideHead); ++count; } }
        std::uintptr_t adata = *reinterpret_cast<std::uintptr_t volatile*>(pawn + 0x6FC);   // m_aoAccessories (headgear/breather)
        const int anum = *reinterpret_cast<int volatile*>(pawn + 0x6FC + 8);
        if (adata >= 0x10000 && anum > 0 && anum <= 32)
            for (int i = 0; i < anum; ++i) { std::uintptr_t c = *reinterpret_cast<std::uintptr_t volatile*>(adata + static_cast<std::uintptr_t>(i) * 8); if (c >= 0x10000) { FpSetOwnerNoSee(c, hideHead); ++count; } }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}
}


// ===== [HEADAIM] Head drives the aim: HMD yaw/pitch -> PlayerController rotation. Ported from ME2
// 2026-07-28, with LE3's offset discovered by [AIMPROBE] the same day: slots +0x114/+0x118 of the
// PlayerController moved together during look-around with yaw (+0x118) dominating a left/right sweep,
// which matches an FRotator {Pitch,Yaw,Roll} based at 0x114 (LE2 had it at 0x124). If the world does
// not turn with the head, that identification is wrong - re-run [AIMPROBE] and sweep pitch alone.
//
// Injection model is ME2's, unchanged: the head ADDS a delta on top of the stick aim; last frame's
// delta is stripped each frame so the stick keeps working; pitch clamps at +/-80deg; a ramp (~25% of
// the remainder per frame) transfers the head-look offset in when aim engages, because stepping the
// rotation in one frame is the [FPSTORM] violent-snap bug ME1 already solved.
//
// DIFFERENCE from ME2, deliberate: ME2 only aims while the weapon is out (g_weaponOut, read through
// LE2 camera-mode offsets). LE3's camera-mode offsets are not discovered yet, so there is no weapon
// state to gate on - aim runs whenever the toggle is on and gameplay is live. Revisit when the
// camera-mode discovery happens.
constexpr std::uintptr_t kCtrlRotation = 0x114;   // [AIMPROBE] pitch +0x114, yaw +0x118, roll +0x11C
std::atomic_bool g_headAimOn{true};
std::atomic_bool g_headAimInvYaw{false};
std::atomic_bool g_headAimInvPitch{false};
// [AIMSCOPE] Whether head aim is restricted to weapon-drawn only. DEFAULT OFF, and the default is
// load-bearing: head aim is what drives APlayerController.ControlRotation, i.e. the direction the
// CHARACTER faces, not merely where a reticle points. LE3 targets world interactions off that
// facing, so with aim disabled the player can look straight at a shop or console and the game does
// not consider them facing it. Measured 2026-08-14: aim engaged only while SFXCameraMode_Combat was
// live, and the entire holstered session that followed (SFXPawn_PlayerVanguardNonCombat /
// SFXCameraMode_Explore, where every shop lives) could not interact with vendors at all.
// DEFAULT ON as of 2026-08-14. Head aim writes ControlRotation, which in THIRD PERSON swings the
// camera boom around the player: looking around with the head orbited the camera instead of simply
// turning the view, which is wrong everywhere outside combat. Restricting aim to weapon-drawn is
// what the player asked for and is the correct default. The known cost is that world interactions
// resolve against ControlRotation, so while holstered the character may need to be turned with the
// stick to face a vendor - if that recurs, fix INTERACTION, do not re-couple exploration aim.
std::atomic_bool g_headAimWeaponOnly{true};
std::atomic_bool g_headAimCoverOff{true};   // [AIMCOVER] default ON = fix active
float g_aimRefYawDeg = 0.0f, g_aimRefPitchDeg = 0.0f;   // HMD orientation at the aim center
int g_aimAppliedYawUU = 0, g_aimAppliedPitchUU = 0;     // last frame's injected delta (to strip it)
int g_seedRemYawUU = 0, g_seedRemPitchUU = 0;           // ramp remainder (head-look -> aim handoff)
int g_seedDoneYawUU = 0, g_seedDonePitchUU = 0;
bool g_aimActive = false;

int NormUU(int v) noexcept { while (v > 32768) v -= 65536; while (v < -32768) v += 65536; return v; }
float WrapDegF(float d) noexcept { while (d > 180.0f) d -= 360.0f; while (d < -180.0f) d += 360.0f; return d; }
bool ReadCtrlRot(std::uintptr_t pc, int* p, int* y, int* r) noexcept
{
    __try { *p = *reinterpret_cast<int volatile*>(pc + kCtrlRotation); *y = *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 4); *r = *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 8); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteCtrlRot(std::uintptr_t pc, int p, int y, int r) noexcept
{
    __try { *reinterpret_cast<int volatile*>(pc + kCtrlRotation) = p; *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 4) = y; *reinterpret_cast<int volatile*>(pc + kCtrlRotation + 8) = r; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


// [CAMPROBE] helpers: class name of a UObject, and a tiny hex formatter (no std::string inside __try).
// The object's OWN name, as opposed to its class name. This is what separates a live actor from the
// UClass that describes it: the camera scan kept reporting class objects (their slot census came back
// full of NameProperty/Function/ObjectProperty entries - class metadata, not an actor), and without the
// object name there was no way to see that from the log.
bool ObjNameOf(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    unsigned long long nm = 0;
    __try { nm = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ReadSfxName(nm, out, static_cast<int>(cap));
}
bool SafeReadByte(std::uintptr_t addr, unsigned char* out) noexcept
{
    __try { *out = *reinterpret_cast<unsigned char volatile*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
bool ClassNameOf(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    std::uintptr_t cls = 0;
    if (!SafeReadPtr(obj + kObjClass, &cls) || cls < 0x10000) return false;
    unsigned long long nm = 0;
    __try { nm = *reinterpret_cast<unsigned long long volatile*>(cls + kObjName); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return ReadSfxName(nm, out, cap);
}
std::string HexOff(std::uintptr_t v) noexcept
{
    char b[24] = {};
    sprintf_s(b, "%03llX", static_cast<unsigned long long>(v));
    return std::string(b);
}

// [CAMPROBE2] POD-only so it can hold __try: a function that unwinds C++ objects cannot also use SEH
// (C2712), and the caller logs with std::string. Fills up to cap entries and returns the count.
int ScanCameraInstances(char names[][64], std::uintptr_t* addrs, int cap) noexcept
{
    int n = 0;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x1887E40);
        const int num = *reinterpret_cast<int volatile*>(base + 0x1887E40 + 8);
        if (data >= 0x10000 && num > 0 && num <= 5000000)
        {
            for (int k = 0; k < num && n < cap; ++k)
            {
                const std::uintptr_t obj =
                    *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(k) * 8);
                if (obj < 0x10000) continue;
                const std::uintptr_t cls = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClass);
                if (cls < 0x10000) continue;
                const unsigned long long cn = *reinterpret_cast<unsigned long long volatile*>(cls + kObjName);
                char nm[64] = {};
                if (!ReadSfxName(cn, nm, sizeof(nm))) continue;
                if (std::strstr(nm, "Camera") == nullptr) continue;
                if (std::strcmp(nm, "Class") == 0) continue;
                // Reject the decoys. The first pass capped at six matches and filled up on debug and
                // shake helpers before reaching the real camera, then censused a class object (its
                // slots read Package/Class/Enum - metadata, not a live camera). These substrings are
                // every camera-adjacent class that is NOT the gameplay camera.
                if (std::strstr(nm, "Shake")  != nullptr) continue;
                if (std::strstr(nm, "Input")  != nullptr) continue;
                if (std::strstr(nm, "HUD")    != nullptr) continue;
                if (std::strstr(nm, "Debug")  != nullptr) continue;
                if (std::strstr(nm, "Anim")   != nullptr) continue;
                if (std::strstr(nm, "Blur")   != nullptr) continue;
                if (std::strstr(nm, "Mode")   != nullptr) continue;   // modes are found via the camera
                // de-dup by class name so one prolific class cannot fill the whole result set
                bool dup = false;
                for (int q = 0; q < n; ++q) if (std::strcmp(names[q], nm) == 0) { dup = true; break; }
                if (dup) continue;
                strncpy_s(names[n], 64, nm, 63);
                addrs[n] = obj;
                ++n;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    return n;
}

// [GUICEN] Census of live GFx/GUI objects from the global object array (same walk as
// ScanCameraInstances, which found the camera chain). Purpose: the squad/outfit-class screens are
// invisible to every engine signal tried so far (gm 0, gameplay camera, unpaused, and the
// scene-sampling draw is the game's own composite firing every frame) - but the UI system itself
// must hold an object PER OPEN SCREEN, named after it. This logs the (class:name) set whenever it
// CHANGES, so one short session of opening/closing the squad menu names the objects that appear
// and disappear with it. That named signal is what GetMenuMode gets wired to next - measured, not
// inferred. Runs on its own thread (the walk is ~5M reads; never on the present thread).
// Delta logging (v2): the first field run proved the signal live - the object set visibly grew
// the moment a menu opened (139 -> 314) and shrank on close - but a fixed top-24 print always
// showed the same early objects; the menu's own objects live deeper in the table. So keep the
// whole set (hashed) between scans and print only what APPEARED/DISAPPEARED - the menu names
// themselves, timestamped against the open/close.
struct GuiCenEntry { char cls[48]; char name[48]; unsigned long long hash; std::uintptr_t clsPtr; };
int GuiCensusScan(GuiCenEntry* out, int cap, int* totalMatches) noexcept
{
    int n = 0, total = 0;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x1887E40);
        const int num = *reinterpret_cast<int volatile*>(base + 0x1887E40 + 8);
        if (data >= 0x10000 && num > 0 && num <= 5000000)
        {
            for (int k = 0; k < num; ++k)
            {
                const std::uintptr_t obj =
                    *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(k) * 8);
                if (obj < 0x10000) continue;
                const std::uintptr_t cls = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClass);
                if (cls < 0x10000) continue;
                const unsigned long long cn = *reinterpret_cast<unsigned long long volatile*>(cls + kObjName);
                char cnm[48] = {};
                if (!ReadSfxName(cn, cnm, sizeof(cnm))) continue;
                if (std::strcmp(cnm, "SFXGUIValue_MarkerObjectiveSP") == 0)
                {
                    g_markerObjectiveClass.store(cls, std::memory_order_release);
                    g_markerObjectiveInstance.store(obj, std::memory_order_release);
                }
                // Harvest every native boundary in the marker projection pipeline while the existing
                // background census is already walking GObjects. Runtime bytes are dumped outside SEH.
                if (std::strcmp(cnm, "Function") == 0)
                {
                    const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName);
                    char onm[48] = {};
                    if (ReadSfxName(on, onm, sizeof(onm)))
                    {
                        const std::uintptr_t outer = *reinterpret_cast<std::uintptr_t volatile*>(obj + 0x40);
                        if (outer >= 0x10000)
                        {
                            const unsigned long long ownerName =
                                *reinterpret_cast<unsigned long long volatile*>(outer + kObjName);
                            char owner[48] = {};
                            if (ReadSfxName(ownerName, owner, sizeof(owner)))
                            {
                                if (std::strcmp(owner, "SFXGUIValue_Marker") == 0 &&
                                    std::strcmp(onm, "CalculateMarkerPosition") == 0)
                                    g_markerCalculateUFunction.store(obj, std::memory_order_release);
                                else if (std::strcmp(owner, "SFXGUIValue_MarkerObjective") == 0 &&
                                         std::strcmp(onm, "Update") == 0)
                                    g_markerObjectiveUpdateUFunction.store(obj, std::memory_order_release);
                                else if (std::strcmp(owner, "SFXGUIMovie") == 0)
                                {
                                    if (std::strcmp(onm, "GetGUISceneView") == 0)
                                        g_markerGetGuiSceneViewUFunction.store(obj, std::memory_order_release);
                                    else if (std::strcmp(onm, "WorldToScreenFast") == 0)
                                        g_markerWorldToScreenUFunction.store(obj, std::memory_order_release);
                                    else if (std::strcmp(onm, "WorldToSafePostProjectionFast") == 0)
                                        g_markerWorldToSafeUFunction.store(obj, std::memory_order_release);
                                }
                            }
                        }
                    }
                }

                if (std::strstr(cnm, "GFx") == nullptr && std::strstr(cnm, "GUI") == nullptr) continue;
                if (std::strcmp(cnm, "Class") == 0) continue;
                ++total;
                if (n < cap)
                {
                    const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName);
                    char onm[48] = {};
                    if (!ReadSfxName(on, onm, sizeof(onm))) onm[0] = 0;
                    memcpy(out[n].cls, cnm, sizeof(cnm));
                    memcpy(out[n].name, onm, sizeof(onm));
                    out[n].clsPtr = cls;
                    unsigned long long hh = 1469598103934665603ull;
                    for (const char* s = out[n].cls; *s; ++s) { hh ^= static_cast<unsigned char>(*s); hh *= 1099511628211ull; }
                    for (const char* s = out[n].name; *s; ++s) { hh ^= static_cast<unsigned char>(*s); hh *= 1099511628211ull; }
                    out[n].hash = hh;
                    ++n;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; total = 0; }
    *totalMatches = total;
    return n;
}

// [GUINAME] The payoff of the census hunt (2026-08-14 field run): the 3D-model menus exist as
// live, NAMED UObjects created on open and collected on close - measured three clean open/close
// cycles for the squad menu:
//   SFXGUI_PCSquadRecord            squad menu
//   SFXGUI_WeaponSelection /
//   SFXGUIExt_WeaponMods            weapon bench
//   GFxMovieInfo "Personalization"  armor/outfit customization
// Presence of the object IS the menu being open: it cannot flap (no per-frame heuristics) and it
// cannot fire in gameplay (the object does not exist there). Substring match against class AND
// object names; names starting "Default__" are class-default objects that exist forever and never
// count. Scan every 250ms so a menu is caught within a frame or three of opening.
// EXACT class names only. Substring matching latched flat AT BOOT: levels permanently contain
// kismet and data objects with GUI-ish names (SFXSeqAct_StoreGUI, SFXGUIData_Weapons,
// GFxMovieInfo:PersonalTerminalEGM...), so "WeaponMods"/"SquadRecord" as substrings matched
// level furniture that exists whenever the map is loaded. The classes below are the measured
// menu INSTANCES (created on open, collected on close). GFxMovieInfo:Personalization was also
// dropped: it appeared on AREA load (10:12:02, alongside the weapon-bench InterpActors), not on
// menu open - the armor locker's real object gets added from its own census line when measured.
// Authoritative raw screen objects only. Guessed class names made the presentation react to
// short-lived/package objects instead of the actual screen lifetime.
const char* kMonoGuiClasses[] = {
    "SFXGUI_PCSquadRecord", "SFXGUI_WeaponSelection", "SFXGUIExt_WeaponMods",
    "SFXGUI_TeamSelect", "SFXGUI_PCSplashScreen", "SFXGUI_MainMenu_RightComputer",
    // Added from a [GUICEN] census pass 2026-08-14.
    // Each was observed as a clean create-on-open / destroy-on-close pair with a real instance name
    // (never Default__) around a measured MENU_MONO episode:
    //   SFXGUI_Terminal   06:54:42.6 -> 52.1   Normandy terminal / datapad
    //   SFXGUI_Mail       06:54:44.2 -> 52.1, again 06:57:30.4 -> 38.3   private terminal mail
    //   SFXGUI_WarAssets  06:54:48.9 -> 52.1   war assets screen
    // These screens already present correctly through the input-lock owner; the census entry is what
    // keeps them correct in the two cases where locks alone are not trusted - it cancels the
    // cinematic-tail grace immediately, and it exempts them from the gameplay-family suppression
    // that stops Atlas-style scripted locks. Supporting disambiguator only, never an owner.
    // DELIBERATELY NOT ADDED, though they appeared in the same census: SFXGUI_Elevator (in-world
    // panel, alive during normal gameplay), SFXGUI_Markers, SFXGUI_CrosshairReticle,
    // SFXGUI_AtlasHUD, SFXGUI_WeaponReticle*, SFXGUIValue_* (HUD/world objects), and
    // GFxMovieInfo:Personalization (package metadata that loads with the AREA, not the locker).
    "SFXGUI_Terminal", "SFXGUI_Mail", "SFXGUI_WarAssets",
};
constexpr int kMonoGuiClassCount = static_cast<int>(sizeof(kMonoGuiClasses) / sizeof(kMonoGuiClasses[0]));
std::atomic_bool g_namedGuiActive{false};

bool GuiEntryIsMonoName(const GuiCenEntry& e) noexcept
{
    if (std::strncmp(e.name, "Default__", 9) == 0) return false;
    if (std::strcmp(e.cls, "SFXGUI_MainMenu_RightComputer") == 0)
        return std::strcmp(e.name, "SFXGUI_MainMenu_RightComputer") == 0;
    for (const char* want : kMonoGuiClasses)
        if (std::strcmp(e.cls, want) == 0) return true;
    return false;
}

// [GUINAME FAST] Presence by CLASS POINTER, no name resolution: the slow census resolves every
// object's class name (FName chase x 5M objects, most of a core at high cadence). The target
// UClass pointers are harvested from the slow scan (the Default__ class-default objects exist
// from class load, so the pointers are known long before any menu opens); this walk is 2 reads
// per object and runs every 100ms, so a menu is caught before its open animation finishes.
// Names are resolved only on a pointer match (0-2 objects) to reject the Default__ object.
// [FRAMETIME] census cost, read and reset by the present-hook telemetry.
std::atomic<unsigned> g_censusFastMaxUs{0};
std::atomic<unsigned> g_censusSlowMaxUs{0};
std::atomic<int>      g_censusObjCount{0};

// Returns 1 = named menu object alive, 0 = none, -1 = walk faulted (caller holds its verdict).
int GuiFastPresenceScan(const std::uintptr_t* clsPtrs, const char* const* clsNames, int clsN,
                        char* what, int whatCap) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(base + 0x1887E40);
        const int num = *reinterpret_cast<int volatile*>(base + 0x1887E40 + 8);
        if (data < 0x10000 || num <= 0 || num > 5000000) return -1;
        g_censusObjCount.store(num, std::memory_order_relaxed);   // [FRAMETIME]
        for (int k = 0; k < num; ++k)
        {
            const std::uintptr_t obj =
                *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(k) * 8);
            if (obj < 0x10000) continue;
            const std::uintptr_t cls = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClass);
            int match = -1;
            for (int c = 0; c < clsN; ++c) if (cls == clsPtrs[c]) { match = c; break; }
            if (match < 0) continue;
            const unsigned long long on = *reinterpret_cast<unsigned long long volatile*>(obj + kObjName);
            char onm[48] = {};
            if (!ReadSfxName(on, onm, sizeof(onm))) continue;
            if (std::strncmp(onm, "Default__", 9) == 0) continue;
            if (std::strcmp(clsNames[match], "SFXGUI_MainMenu_RightComputer") == 0 &&
                std::strcmp(onm, "SFXGUI_MainMenu_RightComputer") != 0)
                continue;
            if (what != nullptr && whatCap > 0) sprintf_s(what, static_cast<size_t>(whatCap), "%s", onm);
            return 1;
        }
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

DWORD WINAPI GuiCenThread(LPVOID) noexcept
{
    constexpr int kCap = 1024;
    static GuiCenEntry s_prev[kCap];   // static: too big for the thread stack
    static GuiCenEntry s_cur[kCap];
    int prevN = -1;                    // -1 = no baseline yet (first scan logs nothing)
    // One slot per allowlisted class. The old hard cap of 8 left later entries on the
    // 1.5-second slow path after the allowlist grew, making those menus appear unreliable.
    // Keep the fast pointer cache structurally tied to the allowlist.
    std::uintptr_t clsPtrs[kMonoGuiClassCount] = {};
    int clsN = 0;
    const char* clsNames[kMonoGuiClassCount] = {};
    int sinceSlow = 1000;              // force a slow census on the first pass
    int idleTicks = 1000;               // likewise: populate class/marker caches immediately at startup
    bool scanWindowWasActive = false;
    constexpr int kIdleMaintenanceTicks = 30;   // 3s: retain marker/class discovery without 10Hz heap walks
    const auto publishPresence = [](bool found, const char* what) noexcept
    {
        const bool was = g_namedGuiActive.exchange(found, std::memory_order_release);
        if (found != was)
            ME2VR::Log::Line(found ? std::string("[GUISIGNAL] object ON: ") + what : "[GUISIGNAL] object OFF");
    };
    LARGE_INTEGER qf = {}; QueryPerformanceFrequency(&qf);
    const auto noteUs = [](std::atomic<unsigned>& slot, unsigned us) noexcept
    {
        unsigned prev = slot.load(std::memory_order_relaxed);
        while (us > prev && !slot.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
    };
    for (;;)
    {
        Sleep(100);
        // [CENSUSIDLE 2026-08-17] The old loop walked every live UObject every 100ms even during
        // ordinary gameplay (201k objects in the Citadel, 6-13ms per fast walk plus a 18-32ms deep
        // walk every 1.5s). That is useful only while a full-screen menu may be opening/closing.
        // ME3's verified input-owner fields fire before those menus need classification. On the
        // rising edge force a deep scan immediately, so a newly loaded menu class is discovered in
        // <=100ms instead of waiting for the old 1.5s cache refresh. While gameplay owns input, keep
        // just one deep maintenance scan every 3s for late-loaded marker/menu classes.
        const bool scanWindowActive =
            ME2VR::EngineProbe::IsMenuInputOwned() ||
            g_namedGuiActive.load(std::memory_order_acquire);
        if (!scanWindowActive)
        {
            scanWindowWasActive = false;
            if (++idleTicks < kIdleMaintenanceTicks) continue;
            idleTicks = 0;
            sinceSlow = 1000;   // maintenance pass is the deep class/marker harvest
        }
        else
        {
            if (!scanWindowWasActive) sinceSlow = 1000;   // menu edge: deep scan now, no stretched flash
            scanWindowWasActive = true;
            idleTicks = 0;
        }

        // [GUINAME FAST] 100ms pointer-compare scans between the ~1.5s slow censuses, so the
        // mono flip lands before the menu's open animation finishes (the 250ms name-resolving
        // cadence was a visible stretched flash on every open).
        // [FRAMETIME] both walks are timed: they touch every live UObject (random reads across the
        // whole heap), so their duration is the number to read when the render thread stutters.
        if (++sinceSlow < 15 && clsN > 0)
        {
            char what[64] = {};
            LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
            const int r = GuiFastPresenceScan(clsPtrs, clsNames, clsN, what, sizeof(what));
            LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
            noteUs(g_censusFastMaxUs, static_cast<unsigned>((q1.QuadPart - q0.QuadPart) * 1000000ll / qf.QuadPart));
            if (r >= 0) publishPresence(r == 1, what);   // -1 = walk faulted -> hold verdict
            continue;
        }
        sinceSlow = 0;
        int total = 0;
        LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
        const int n = GuiCensusScan(s_cur, kCap, &total);
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        noteUs(g_censusSlowMaxUs, static_cast<unsigned>((q1.QuadPart - q0.QuadPart) * 1000000ll / qf.QuadPart));
        // [GUINAME] presence -> mono. A failed/aborted scan (n==0 AND total==0, seen as
        // "total=0" lines when the object array moves mid-walk) keeps the previous verdict
        // rather than flapping the presentation.
        DumpMarkerNativeTargets(); // one-shot evidence retained for build verification
        InstallMarkerObjectiveDirectHookIfReady();
        if (n > 0 || total > 0)
        {
            bool found = false;
            char what[100] = {};
            for (int i = 0; i < n; ++i)
            {
                // harvest the target UClass pointers by NAME, Default__ objects included - the
                // class-default object shares the class pointer and exists from class load, so
                // the fast path is armed long before any menu opens.
                for (const char* want : kMonoGuiClasses)
                {
                    if (std::strcmp(s_cur[i].cls, want) != 0) continue;
                    bool have = false;
                    for (int c = 0; c < clsN; ++c) if (clsPtrs[c] == s_cur[i].clsPtr) { have = true; break; }
                    if (!have && clsN < kMonoGuiClassCount)
                    {
                        clsPtrs[clsN] = s_cur[i].clsPtr;
                        clsNames[clsN++] = want;
                    }
                }
                if (!found && GuiEntryIsMonoName(s_cur[i]))
                {
                    found = true;
                    sprintf_s(what, "%s:%s", s_cur[i].cls, s_cur[i].name);
                }
            }
            publishPresence(found, what);
        }
        if (!ME2VR::Log::DiagnosticsOn()) { prevN = -1; continue; }
        if (prevN >= 0)
        {
            char line[640] = {};
            int off = sprintf_s(line, "[GUICEN] total=%d delta:", total);
            int shown = 0;
            for (int i = 0; i < n && shown < 12 && off > 0 && off < 520; ++i)   // appeared
            {
                bool inPrev = false;
                for (int j = 0; j < prevN; ++j) if (s_prev[j].hash == s_cur[i].hash) { inPrev = true; break; }
                // de-dup within this scan so one prolific class:name doesn't fill the print
                if (!inPrev) for (int j = 0; j < i; ++j) if (s_cur[j].hash == s_cur[i].hash) { inPrev = true; break; }
                if (!inPrev) { off += sprintf_s(line + off, sizeof(line) - off, " +%s:%s", s_cur[i].cls, s_cur[i].name); ++shown; }
            }
            for (int j = 0; j < prevN && shown < 20 && off > 0 && off < 560; ++j)   // disappeared
            {
                bool inCur = false;
                for (int i = 0; i < n; ++i) if (s_cur[i].hash == s_prev[j].hash) { inCur = true; break; }
                if (!inCur) for (int i = 0; i < j; ++i) if (s_prev[i].hash == s_prev[j].hash) { inCur = true; break; }
                if (!inCur) { off += sprintf_s(line + off, sizeof(line) - off, " -%s:%s", s_prev[j].cls, s_prev[j].name); ++shown; }
            }
            if (shown > 0) ME2VR::Log::Line(line);
        }
        memcpy(s_prev, s_cur, sizeof(GuiCenEntry) * static_cast<size_t>(n));
        prevN = n;
    }
}

namespace ME2VR::EngineProbe
{
// [GUICEN] start the census thread once (delta logging is Diagnostics-gated; the [GUINAME]
// named-menu detection always runs - it is what drives the mono menus).
void GuiProbeStart() noexcept
{
    static std::atomic_bool s_started{false};
    if (s_started.exchange(true)) return;
    HANDLE t = CreateThread(nullptr, 0, GuiCenThread, nullptr, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
}

// [GUINAME] true while a named 3D-model menu object is alive (squad record, weapon bench,
// personalization). See kMonoGuiNames by GuiCenThread.
bool GetNamedGuiActive() noexcept { return g_namedGuiActive.load(std::memory_order_acquire); }

// [FRAMETIME] worst fast/slow census walk (us) since the last call, and the live UObject count.
void TakeCensusStats(unsigned* fastMaxUs, unsigned* slowMaxUs, int* objects) noexcept
{
    const unsigned f = g_censusFastMaxUs.exchange(0, std::memory_order_relaxed);
    const unsigned s = g_censusSlowMaxUs.exchange(0, std::memory_order_relaxed);
    if (fastMaxUs != nullptr) *fastMaxUs = f;
    if (slowMaxUs != nullptr) *slowMaxUs = s;
    if (objects != nullptr) *objects = g_censusObjCount.load(std::memory_order_relaxed);
}

std::uintptr_t GetPrimaryLocalPlayer() noexcept
{
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return 0;
    std::uintptr_t gEngine = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine == 0) return 0;
    std::uintptr_t playersData = 0;
    int playersNum = 0;
    if (!SafeReadPtr(gEngine + kEngineGamePlayers + kTArrayData, &playersData) ||
        !SafeReadInt(gEngine + kEngineGamePlayers + kTArrayNum, &playersNum))
        return 0;
    if (playersData == 0 || playersNum < 1 || playersNum > 8) return 0;
    std::uintptr_t p1 = 0;
    if (!SafeReadPtr(playersData, &p1)) return 0;
    return p1;
}

// ===================== LE3 DISCOVERY PROBES (read-only) =====================
// Added 2026-07-28 to find the LE3 offsets that head aim, mono menus and loading-screen detection
// need. ME2 reaches these through hardcoded LE2 offsets which mean nothing here, so they have to be
// observed rather than copied. EVERYTHING BELOW IS READ-ONLY AND SEH-GUARDED: writing live engine
// objects is the random-crash family in the ME1/ME2 notes, and a probe must never be the cause.
// All output is behind [AIMPROBE] / [MODEPROBE], so it only appears with Diagnostics=1.

// [AIMPROBE] The player's aim lives in the PlayerController as a rotator (UE3: int32 pitch/yaw/roll,
// 65536 = 360 degrees). The mod does not know LE3's offset, so: snapshot a window of the PC every ~0.5s,
// diff it against the previous snapshot, and report the int32 slots that MOVED. Look around in game
// and the yaw slot will dominate; look up and down and the pitch slot appears one dword away.
// A rotator triple shows up as two neighbouring active slots with a near-static third.
void AimProbeTick() noexcept
{
    if (!ME2VR::Log::DiagnosticsOn()) return;
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 500) return;
    s_last = now;

    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;

    constexpr int kFirst = 0x80;    // skip the UObject header
    constexpr int kLast  = 0x280;   // AActor.Rotation sits at 0x124 in LE2; scan well past it
    constexpr int kSlots = (kLast - kFirst) / 4;
    static int  s_prev[kSlots] = {};
    static bool s_have = false;

    int cur[kSlots] = {};
    for (int i = 0; i < kSlots; ++i)
        if (!SafeReadInt(pc + kFirst + i * 4, &cur[i])) return;   // bail on the first unreadable slot

    if (s_have)
    {
        // Report the most active slots this tick. Rotator components wrap at 65536, so ignore
        // absurd deltas (pointers and counters) and anything that never settles.
        char line[512] = {};
        int used = 0;
        used += sprintf_s(line + used, sizeof(line) - used, "[AIMPROBE] moved:");
        int reported = 0;
        for (int i = 0; i < kSlots && reported < 6; ++i)
        {
            const int d = cur[i] - s_prev[i];
            if (d == 0) continue;
            const int ad = (d < 0) ? -d : d;
            if (ad > 65536) continue;                     // not rotator-shaped
            used += sprintf_s(line + used, sizeof(line) - used,
                              "  +0x%03X=%d(%+d)", kFirst + i * 4, cur[i], d);
            ++reported;
        }
        if (reported > 0) ME2VR::Log::Line(line);
    }
    for (int i = 0; i < kSlots; ++i) s_prev[i] = cur[i];
    s_have = true;
}

// [MODEPROBE] The engine's UI-context enum drives mono menus, the galaxy map and loading screens.
// In LE2 it is GameModeManager2(0xA68) -> CurrentMode byte(0xB8) off GEngine. Try that exact path
// first - if LE3 shares it the mod is done - and in parallel report any byte in a window off GEngine
// that CHANGES, so opening a menu or hitting a load screen names the real offset either way.
void ModeProbeTick() noexcept
{
    if (!ME2VR::Log::DiagnosticsOn()) return;
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 500) return;
    s_last = now;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return;
    std::uintptr_t gEngine = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine == 0) return;

    // 1. the LE2 path, verbatim, as a straight hypothesis test
    {
        std::uintptr_t mgr = 0;
        int  le2Mode = -1;
        static int s_le2Prev = -2;
        if (SafeReadPtr(gEngine + 0xA68, &mgr) && mgr > 0x10000)
        {
            int v = 0;
            if (SafeReadInt(mgr + 0xB8, &v)) le2Mode = v & 0xFF;
        }
        if (le2Mode != s_le2Prev)
        {
            s_le2Prev = le2Mode;
            ME2VR::Log::Line("[MODEPROBE] LE2-path GEngine+0xA68->+0xB8 = " + std::to_string(le2Mode) +
                             "  (-1 = unreadable, so LE3 differs)");
        }
    }

    // 2. independent of that, report which pointer slots off GEngine lead somewhere whose first
    //    bytes change - the manager object will show up here even if its offset moved.
    constexpr int kFirst = 0x400;
    constexpr int kLast  = 0xC00;
    constexpr int kSlots = (kLast - kFirst) / 8;
    static unsigned char s_prev[kSlots] = {};
    static bool s_have = false;
    unsigned char cur[kSlots] = {};
    for (int i = 0; i < kSlots; ++i)
    {
        std::uintptr_t p = 0;
        int v = 0;
        if (SafeReadPtr(gEngine + kFirst + i * 8, &p) && p > 0x10000 && SafeReadInt(p + 0xB8, &v))
            cur[i] = static_cast<unsigned char>(v & 0xFF);
    }
    if (s_have)
    {
        char line[512] = {};
        int used = sprintf_s(line, sizeof(line), "[MODEPROBE] context byte changed:");
        int reported = 0;
        for (int i = 0; i < kSlots && reported < 6; ++i)
        {
            if (cur[i] == s_prev[i]) continue;
            used += sprintf_s(line + used, sizeof(line) - used, "  GEngine+0x%03X->+0xB8: %u->%u",
                              kFirst + i * 8, s_prev[i], cur[i]);
            ++reported;
        }
        if (reported > 0) ME2VR::Log::Line(line);
    }
    for (int i = 0; i < kSlots; ++i) s_prev[i] = cur[i];
    s_have = true;
}

// [CAMPROBE] Find LE3's PlayerController -> PlayerCamera -> CurrentCameraMode chain and report the
// live mode's NAME. Read-only, SEH-guarded, Diagnostics=1 only. This single chain is the dependency
// under three separate asks: ADS stays flat because the FOV cinematic detector has no weapon-out
// guard (ME2 uses the camera mode for it); first person is per-camera-mode; and cover/uncover
// transitions are camera-mode changes. LE2's offsets (PlayerCamera 0x6A0, CurrentCameraMode 0x580)
// mean nothing here, so both are discovered by scanning rather than assumed.
//
// Method: walk pointer slots of the PlayerController; for each, read the pointee's class name via the
// existing ReadSfxName path; report any whose class name contains "Camera". Then walk THAT object's
// pointer slots the same way looking for a class name containing "CameraMode", and log the mode's own
// name whenever it changes. Aim, sprint, cover and ADS each have distinct mode names in this engine,
// so a single play session with combat produces the whole vocabulary.
// The engine's authoritative "what context is the game in" enum, LE3 chain. LE2 reaches it via
// GameModeManager2(0xA68) -> CurrentMode(0xB8); neither offset means anything here. The measured LE3
// chain is PlayerController -> SFXGameModeManager(0xC38) -> mode byte(0xA8), found by watching which
// byte moved in small isolated steps while everything around it churned as text.
// EGameModes (LE2 SDK): 0 Default(gameplay), 1 PowerWheel, 2 WeaponWheel, 3 Command, 4 Vehicle,
// 5 Conversation, 6 Cinematic, 7 GUI, 8 Movie, 9 Galaxy, 10 Orbital, 11 Photo, 12 CheatMenu.
// Pure READ. Returns -1 when unreadable, and every caller must fail OPEN to VR on -1 so a bad read
// can never latch mono into live gameplay.

// [CAMPROBE4] Report TArray<UObject*> members. A UE3 TArray is {void* data, int32 count, int32 max};
// the slot holds a heap allocation, so a pointer census that resolves class names walks straight past
// it - which is very likely how the camera behaviour stayed hidden through three probe rounds.
// Read-only, every dereference SEH-guarded, and bounded hard: a bogus count can otherwise walk memory.
void ReportObjectArrays(std::uintptr_t obj, std::uintptr_t lo, std::uintptr_t hi, const char* tag) noexcept
{
    char line[460] = {};
    int used = 0;
    for (std::uintptr_t off = lo; off + 16 <= hi; off += 8)
    {
        std::uintptr_t data = 0;
        if (!SafeReadPtr(obj + off, &data) || data < 0x10000) continue;
        int count = 0, cap = 0;
        if (!SafeReadInt(obj + off + 8, &count)) continue;
        if (!SafeReadInt(obj + off + 12, &cap)) continue;
        if (count <= 0 || count > 64 || cap < count || cap > 4096) continue;

        // every element must resolve as a UObject or this is not an object array
        char first[64] = {};
        int good = 0;
        const int probe = (count < 4) ? count : 4;
        for (int i = 0; i < probe; ++i)
        {
            std::uintptr_t el = 0;
            if (!SafeReadPtr(data + static_cast<std::uintptr_t>(i) * 8, &el) || el < 0x10000) break;
            char cn[64] = {};
            if (!ClassNameOf(el, cn, sizeof(cn))) break;
            if (i == 0) strncpy_s(first, cn, sizeof(first) - 1);
            ++good;
        }
        if (good != probe || good == 0) continue;

        if (used == 0) used = sprintf_s(line, sizeof(line), "[CAMPROBE4] %s arrays:", tag);
        used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%s[%d]",
                          static_cast<unsigned long long>(off), first, count);
        if (used > 360) { ME2VR::Log::Line(line); used = 0; line[0] = 0; }
    }
    if (used > 0) ME2VR::Log::Line(line);
}

// Pointer census over an arbitrary range, printed as class names.
void ReportObjectSlots(std::uintptr_t obj, std::uintptr_t lo, std::uintptr_t hi, const char* tag) noexcept
{
    char line[460] = {};
    int used = 0, per = 0;
    for (std::uintptr_t off = lo; off < hi; off += 8)
    {
        std::uintptr_t v = 0;
        if (!SafeReadPtr(obj + off, &v) || v < 0x10000) continue;
        char cn[64] = {};
        if (!ClassNameOf(v, cn, sizeof(cn))) continue;
        if (used == 0) used = sprintf_s(line, sizeof(line), "[CAMPROBE4] %s slots:", tag);
        used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%s",
                          static_cast<unsigned long long>(off), cn);
        if (++per >= 8 || used > 360) { ME2VR::Log::Line(line); used = 0; per = 0; line[0] = 0; }
    }
    if (used > 0) ME2VR::Log::Line(line);
}


// [COVERSCAN3] Correlate every byte against the cover transition pulse.
// One bulk SEH-guarded copy per frame; all comparisons happen on the local copy, so this costs one
// guarded memcpy per frame rather than thousands of guarded byte reads.
constexpr int kCorrRange  = 0xC00;
constexpr int kCorrWindow = 8;      // frames after a pulse in which a related change should land

bool SafeReadBlock(std::uintptr_t addr, unsigned char* out, int n) noexcept
{
    __try { memcpy(out, reinterpret_cast<const void*>(addr), static_cast<size_t>(n)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

struct CorrWatch
{
    unsigned char  last[kCorrRange];
    unsigned short hits[kCorrRange];    // changed inside a post-pulse window
    unsigned short miss[kCorrRange];    // changed outside any window (noise)
    unsigned char  boolOnly[kCorrRange];
    bool primed;
};

void CorrWatchTick(CorrWatch* w, std::uintptr_t obj, std::uintptr_t lo, bool inWindow) noexcept
{
    if (obj < 0x10000) return;
    unsigned char cur[kCorrRange];
    if (!SafeReadBlock(obj + lo, cur, kCorrRange)) return;
    if (!w->primed)
    {
        for (int i = 0; i < kCorrRange; ++i)
        {
            w->last[i] = cur[i]; w->hits[i] = 0; w->miss[i] = 0; w->boolOnly[i] = 1;
        }
        w->primed = true;
        return;
    }
    for (int i = 0; i < kCorrRange; ++i)
    {
        if (!w->boolOnly[i]) continue;
        if (cur[i] > 1) { w->boolOnly[i] = 0; continue; }
        if (cur[i] != w->last[i])
        {
            w->last[i] = cur[i];
            if (inWindow) { if (w->hits[i] < 65000) ++w->hits[i]; }
            else          { if (w->miss[i] < 65000) ++w->miss[i]; }
        }
    }
}

void CorrWatchReport(const CorrWatch* w, std::uintptr_t lo, const char* tag, int pulses) noexcept
{
    if (!w->primed || pulses < 2) return;
    // score = changes that landed with a cover transition, minus the ones that did not. A byte that
    // tracks cover fires on nearly every pulse and almost never otherwise.
    int bestIdx[6] = {};
    int bestScore[6] = {};
    for (int i = 0; i < kCorrRange; ++i)
    {
        if (!w->boolOnly[i] || w->hits[i] == 0) continue;
        const int sc = static_cast<int>(w->hits[i]) - static_cast<int>(w->miss[i]);
        if (sc <= 0) continue;
        for (int k = 0; k < 6; ++k)
        {
            if (sc <= bestScore[k]) continue;
            for (int j = 5; j > k; --j) { bestScore[j] = bestScore[j - 1]; bestIdx[j] = bestIdx[j - 1]; }
            bestScore[k] = sc; bestIdx[k] = i;
            break;
        }
    }
    if (bestScore[0] == 0) return;
    char line[440] = {};
    int used = sprintf_s(line, sizeof(line), "[COVERSCAN3] %s vs %d transitions:", tag, pulses);
    for (int k = 0; k < 6 && bestScore[k] > 0; ++k)
    {
        const int i = bestIdx[k];
        used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%d(hit%d/miss%d)",
                          static_cast<unsigned long long>(lo + static_cast<std::uintptr_t>(i)),
                          static_cast<int>(w->last[i]),
                          static_cast<int>(w->hits[i]), static_cast<int>(w->miss[i]));
    }
    ME2VR::Log::Line(line);
}

// [COVERSCAN2] Rank boolean fields by DWELL, not by how often they change.
// The first pass ranked by flip count and surfaced a ~10ms transition pulse (+0x5AC) rather than the
// cover state, because a state that is held for seconds flips far LESS often than an event that fires
// on every transition. Being in cover is a sustained condition, so score each byte by how many times it
// stays at 1 across several consecutive samples, and record its longest run. A pulse scores zero.
constexpr int kCoverRange = 0xC00;
constexpr int kDwellTicks = 4;      // ~1.2s at the 300ms tick: longer than any transition pulse

struct DwellWatch
{
    unsigned char last[kCoverRange];
    unsigned short runNow[kCoverRange];    // consecutive samples currently at 1
    unsigned short longRuns[kCoverRange];  // completed runs that lasted >= kDwellTicks
    unsigned short bestRun[kCoverRange];   // longest run seen, in samples
    unsigned char  boolOnly[kCoverRange];  // cleared by any value outside {0,1}
    unsigned char  everZero[kCoverRange];  // a byte pinned at 1 forever is not a state, it is a constant
    bool primed;
};

void DwellWatchTick(DwellWatch* w, std::uintptr_t obj, std::uintptr_t lo) noexcept
{
    if (obj < 0x10000) return;
    if (!w->primed)
    {
        for (int i = 0; i < kCoverRange; ++i)
        {
            w->last[i] = 0; w->runNow[i] = 0; w->longRuns[i] = 0;
            w->bestRun[i] = 0; w->boolOnly[i] = 1; w->everZero[i] = 0;
        }
        w->primed = true;
        return;
    }
    for (int i = 0; i < kCoverRange; ++i)
    {
        if (!w->boolOnly[i]) continue;
        unsigned char b = 0;
        if (!SafeReadByte(obj + lo + static_cast<std::uintptr_t>(i), &b)) continue;
        if (b > 1) { w->boolOnly[i] = 0; continue; }
        if (b == 0) w->everZero[i] = 1;
        if (b == 1)
        {
            if (w->runNow[i] < 65000) ++w->runNow[i];
            if (w->runNow[i] > w->bestRun[i]) w->bestRun[i] = w->runNow[i];
        }
        else
        {
            if (w->runNow[i] >= kDwellTicks && w->longRuns[i] < 65000) ++w->longRuns[i];
            w->runNow[i] = 0;
        }
        w->last[i] = b;
    }
}

void DwellWatchReport(const DwellWatch* w, std::uintptr_t lo, const char* tag) noexcept
{
    if (!w->primed) return;
    int bestIdx[8] = {};
    int bestCnt[8] = {};
    for (int i = 0; i < kCoverRange; ++i)
    {
        // must be a bool, must have actually been 0 at some point, and must have held 1 for a stretch
        if (!w->boolOnly[i] || !w->everZero[i] || w->longRuns[i] == 0) continue;
        const int c = static_cast<int>(w->longRuns[i]);
        for (int k = 0; k < 8; ++k)
        {
            if (c <= bestCnt[k]) continue;
            for (int j = 7; j > k; --j) { bestCnt[j] = bestCnt[j - 1]; bestIdx[j] = bestIdx[j - 1]; }
            bestCnt[k] = c; bestIdx[k] = i;
            break;
        }
    }
    if (bestCnt[0] == 0) return;
    char line[440] = {};
    int used = sprintf_s(line, sizeof(line), "[COVERSCAN2] %s held-bools:", tag);
    for (int k = 0; k < 8 && bestCnt[k] > 0; ++k)
    {
        const int i = bestIdx[k];
        used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%d(runs%d,max%.1fs)",
                          static_cast<unsigned long long>(lo + static_cast<std::uintptr_t>(i)),
                          static_cast<int>(w->last[i]), bestCnt[k],
                          static_cast<double>(w->bestRun[i]) * 0.3);
    }
    ME2VR::Log::Line(line);
}

int ReadGameMode() noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return -1;
    std::uintptr_t pc = 0, gmm = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return -1;
    if (!SafeReadPtr(pc + kPcGameModeManager, &gmm) || gmm < 0x10000) return -1;
    unsigned char raw = 0;
    if (!SafeReadByte(gmm + kGmmCurrentMode, &raw)) return -1;
    return (raw <= 12) ? static_cast<int>(raw) : -1;
}

const char* GameModeName(int m) noexcept
{
    switch (m)
    {
        case 0:  return "Default/gameplay";  case 1:  return "PowerWheel";
        case 2:  return "WeaponWheel";       case 3:  return "Command";
        case 4:  return "Vehicle";           case 5:  return "Conversation";
        case 6:  return "Cinematic";         case 7:  return "GUI/menu";
        case 8:  return "Movie";             case 9:  return "Galaxy";
        case 10: return "Orbital";           case 11: return "Photo";
        case 12: return "CheatMenu";         default: return "unreadable";
    }
}
void CamProbeTick() noexcept
{
    if (!ME2VR::Log::DiagnosticsOn()) return;
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 300) return;
    s_last = now;

    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;

    // [CAMPROBE3] The full PlayerController census found them: SFXPlayerCamera at PC+0xE28 and
    // SFXGameModeManager at PC+0xC38. Earlier scans missed the camera because they matched PC slots
    // against objects pulled from the object table by class name - and every one of those turned out
    // to be a class DEFAULT object (Default__BioCameraBehaviorFree and friends), which nothing in a
    // live PlayerController ever points at. Verify the class name rather than trusting the constant.
    std::uintptr_t cam = 0;
    if (!SafeReadPtr(pc + kPcPlayerCamera, &cam) || cam < 0x10000) return;

    static bool s_verified = false;
    if (!s_verified)
    {
        s_verified = true;
        char cn[64] = {};
        if (ClassNameOf(cam, cn, sizeof(cn)))
        {
            char b[160] = {};
            sprintf_s(b, "[CAMPROBE3] player camera at PC+0x%03llX is a %s",
                      static_cast<unsigned long long>(kPcPlayerCamera), cn);
            ME2VR::Log::Line(b);
        }

        // one-shot census of the camera's object slots, so the behaviour/zoom fields are visible
        char line[500] = {};
        int used = 0, per = 0;
        for (std::uintptr_t off = 0x40; off < 0xE00; off += 8)
        {
            std::uintptr_t v = 0;
            if (!SafeReadPtr(cam + off, &v) || v < 0x10000) continue;
            char c2[64] = {};
            if (!ClassNameOf(v, c2, sizeof(c2))) continue;
            if (used == 0) used = sprintf_s(line, sizeof(line), "[CAMPROBE3] camera slots:");
            used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%s",
                              static_cast<unsigned long long>(off), c2);
            if (++per >= 8 || used > 380) { ME2VR::Log::Line(line); used = 0; per = 0; line[0] = 0; }
        }
        if (used > 0) ME2VR::Log::Line(line);

        std::uintptr_t gmm = 0;
        if (SafeReadPtr(pc + kPcGameModeManager, &gmm) && gmm > 0x10000)
        {
            char b2[160] = {};
            char c3[64] = {};
            if (ClassNameOf(gmm, c3, sizeof(c3)))
            {
                sprintf_s(b2, "[MODEPROBE2] game mode manager at PC+0x%03llX is a %s",
                          static_cast<unsigned long long>(kPcGameModeManager), c3);
                ME2VR::Log::Line(b2);
            }
        }
    }

    // --- live tracking: report a camera slot whenever the CLASS of the object it holds changes -----
    // The behaviour objects (BioCameraBehaviorFree / Locked / Flourish) are what LE3 uses in place of
    // LE2's SFXCameraMode, so a switch into aim, cover or a cutscene shows up here as a class change.
    struct Slot { std::uintptr_t off; char cls[48]; };
    static Slot s_slots[48] = {};
    static int s_slotCount = 0;
    static bool s_slotsInit = false;
    if (!s_slotsInit)
    {
        s_slotsInit = true;
        for (std::uintptr_t off = 0x40; off < 0xE00 && s_slotCount < 48; off += 8)
        {
            std::uintptr_t v = 0;
            if (!SafeReadPtr(cam + off, &v) || v < 0x10000) continue;
            char c2[64] = {};
            if (!ClassNameOf(v, c2, sizeof(c2))) continue;
            s_slots[s_slotCount].off = off;
            strncpy_s(s_slots[s_slotCount].cls, c2, sizeof(s_slots[0].cls) - 1);
            ++s_slotCount;
        }
    }
    for (int i = 0; i < s_slotCount; ++i)
    {
        std::uintptr_t v = 0;
        if (!SafeReadPtr(cam + s_slots[i].off, &v) || v < 0x10000) continue;
        char c2[64] = {};
        if (!ClassNameOf(v, c2, sizeof(c2))) continue;
        if (std::strcmp(c2, s_slots[i].cls) == 0) continue;
        // Log the raw game FOV with every behaviour change. ME3 decides "cutscene" from FOV alone
        // (<32deg), which is why aiming down sights - a zoom - gets forced to flat mono. ME2 vetoes
        // that with weapon-out. Pairing the behaviour name with the FOV at the moment it changes says
        // exactly which behaviour ADS uses, so the veto can key on that instead of on a guess about
        // which of Free/Locked/Flourish means gameplay.
        // Behaviour names come from the engine and are logged verbatim. Hinny that GOAT.
        char b[220] = {};
        sprintf_s(b, "[CAMPROBE3] camera +%03llX: %s -> %s   (fov %.1f deg)",
                  static_cast<unsigned long long>(s_slots[i].off), s_slots[i].cls, c2,
                  static_cast<double>(ME2VR::CalcViewHook::GetGameRawFovH() * 2.0f * 57.2957795f));
        ME2VR::Log::Line(b);
        strncpy_s(s_slots[i].cls, c2, sizeof(s_slots[0].cls) - 1);
    }


    // [CAMPROBE4] One deep census, taken only once the game is genuinely in gameplay. The first pass ran
    // at the probe's very first tick - before a Pawn existed - which is why the PlayerController dump had
    // no Pawn in it and the camera had no behaviour attached yet.
    {
        static bool s_deepDone = false;
        static int  s_gameplayTicks = 0;
        if (!s_deepDone)
        {
            if (ReadGameMode() == 0) ++s_gameplayTicks; else s_gameplayTicks = 0;
            if (s_gameplayTicks > 30)          // ~9s of continuous gameplay
            {
                s_deepDone = true;
                ReportObjectSlots(pc,  0x60,  0x1200, "playercontroller");
                ReportObjectArrays(pc, 0x60,  0x1200, "playercontroller");
                ReportObjectSlots(cam, 0xE00, 0x2000, "camera-deep");
                ReportObjectArrays(cam, 0x40, 0x2000, "camera");
                std::uintptr_t dyn = 0;
                if (SafeReadPtr(cam + 0x4C8, &dyn) && dyn > 0x10000)
                {
                    ReportObjectSlots(dyn,  0x40, 0xC00, "dynamiccameraactor");
                    ReportObjectArrays(dyn, 0x40, 0xC00, "dynamiccameraactor");
                }
            }
        }
    }

    // [COVERSCAN2] Sample the pawn and the controller and report which boolean fields are HELD, every
    // 20 seconds. Cover is a sustained state, so it shows up here as repeated multi-second runs; the
    // transition pulse the first attempt latched onto cannot score at all.
    {
        static DwellWatch s_pawnWatch = {};
        static DwellWatch s_ctrlWatch = {};
        static int s_reportTick = 0;
        std::uintptr_t pawn = 0;
        static std::uintptr_t s_watchedPawn = 0;
        if (SafeReadPtr(pc + kPcPawn, &pawn) && pawn > 0x10000)
        {
            if (pawn != s_watchedPawn) { s_watchedPawn = pawn; s_pawnWatch.primed = false; }
            DwellWatchTick(&s_pawnWatch, pawn, 0x100);
        }
        DwellWatchTick(&s_ctrlWatch, pc, 0x100);
        if (++s_reportTick >= 66)          // ~20s at the 300ms tick
        {
            s_reportTick = 0;
            DwellWatchReport(&s_pawnWatch, 0x100, "pawn");
            DwellWatchReport(&s_ctrlWatch, 0x100, "controller");
        }
    }

    // --- the mode byte, now identified: log transitions by name -----------------------------------
    // The sweep that found it also dumped a UTF-16 text buffer sitting a few hundred bytes further on
    // (108/115/105/118/101 = "lsive"), which is pure noise. Only +0A8 moved in small isolated steps.
    {
        const int gm = ReadGameMode();
        static int s_lastGm = -2;
        if (gm != s_lastGm)
        {
            char b[200] = {};
            sprintf_s(b, "[GAMEMODE] %d = %s   (fov %.1f deg)", gm, GameModeName(gm),
                      static_cast<double>(ME2VR::CalcViewHook::GetGameRawFovH() * 2.0f * 57.2957795f));
            ME2VR::Log::Line(b);
            s_lastGm = gm;
        }
    }
}

void DriveHeadAim(float headYawDeg, float headPitchDeg, bool allowAim) noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0 || IsGamePaused()) return;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;

    const bool want = g_headAimOn.load(std::memory_order_acquire) && allowAim;
    int p = 0, y = 0, r = 0;
    if (!want)
    {
        // [AIMUNWIND 2026-08-16] Leaving aim used to strip the whole injection in ONE write. Entering
        // aim has always been ramped, with the comment naming the unramped version "the [FPSTORM]
        // violent-snap bug ME1 already solved" - the exit had no such ramp.
        // Why that is a visible FLASH and not a silent swap: aim and view-look are two halves of the
        // same rotation and they are supposed to cancel (aim owns it -> view-look 0; aim off ->
        // view-look carries it all). But ControlRotation is written HERE, on the XR thread, while the
        // view-look half is consumed by the next CalcSceneView on the GAME thread. Flip both in one
        // frame and they land in different frames, so for one frame the offset is counted twice or
        // not at all - a full-magnitude jump. It happens every time with up + A, because A
        // plus a pushed stick latches the storm state and that drops aim instantly.
        // So ramp the exit exactly like the entry, and publish the part no longer held by the aim as
        // the seed remainder, which the XR side already renders as view-look. The two halves keep
        // summing to the same total, so the worst one-frame mismatch is a ramp step instead of the
        // whole offset.
        static int s_unwindYaw0 = 0, s_unwindPitch0 = 0;
        if (g_aimActive && (g_aimAppliedYawUU != 0 || g_aimAppliedPitchUU != 0) && ReadCtrlRot(pc, &p, &y, &r))
        {
            if (s_unwindYaw0 == 0 && s_unwindPitch0 == 0)
            {
                s_unwindYaw0 = g_aimAppliedYawUU;      // the totals this unwind has to hand over
                s_unwindPitch0 = g_aimAppliedPitchUU;
                // [AIMSNAP] How big the old one-write strip would have been, in degrees. This is the
                // size of the snap that was reported: confirmed by an A/B (HeadAim=0 -> gone,
                // HeadAim=1 -> back). One line per disengage, so the next log shows both how often it
                // happens and how violent each one was, without needing another guess.
                char sb[160] = {};
                sprintf_s(sb, "[AIMSNAP] unwind start: yaw %.1fdeg pitch %.1fdeg -> ramping out over ~%d frames",
                          static_cast<double>(s_unwindYaw0) * 360.0 / 65536.0,
                          static_cast<double>(s_unwindPitch0) * 360.0 / 65536.0,
                          12);
                ME2VR::Log::Line(sb);
            }
            const auto stepToward0 = [](int v) noexcept {
                constexpr int kMinStep = 150;          // same tail guard the entry ramp uses
                if (v == 0) return 0;
                int step = v / 4;
                if (v > 0) { if (step < kMinStep) step = (v < kMinStep ? v : kMinStep); }
                else       { if (step > -kMinStep) step = (v > -kMinStep ? v : -kMinStep); }
                return step;
            };
            const int stepY = stepToward0(g_aimAppliedYawUU);
            const int stepP = stepToward0(g_aimAppliedPitchUU);
            WriteCtrlRot(pc, p - stepP, y - stepY, r);      // give back only this frame's slice
            g_aimAppliedYawUU -= stepY;
            g_aimAppliedPitchUU -= stepP;
            // Complement, in the seed-remainder convention the XR side already consumes
            // (it renders SetHeadLook(-remYaw, remPitch)): what the aim has handed back so far.
            g_seedRemYawUU = s_unwindYaw0 - g_aimAppliedYawUU;
            g_seedRemPitchUU = s_unwindPitch0 - g_aimAppliedPitchUU;
            if (g_aimAppliedYawUU == 0 && g_aimAppliedPitchUU == 0)
            {
                g_aimActive = false; s_unwindYaw0 = 0; s_unwindPitch0 = 0;
                g_seedRemYawUU = 0; g_seedRemPitchUU = 0; g_seedDoneYawUU = 0; g_seedDonePitchUU = 0;
            }
            return;   // stay "active" while unwinding so the XR side keeps rendering the complement
        }
        s_unwindYaw0 = 0; s_unwindPitch0 = 0;
        g_aimActive = false; g_aimAppliedYawUU = 0; g_aimAppliedPitchUU = 0;
        g_seedRemYawUU = 0; g_seedRemPitchUU = 0; g_seedDoneYawUU = 0; g_seedDonePitchUU = 0;
        return;
    }
    if (!ReadCtrlRot(pc, &p, &y, &r)) return;
    constexpr float kDegToUU = 65536.0f / 360.0f;
    if (!g_aimActive)   // entering aim: center on the current head; seed the look->aim ramp
    {
        // Signs (ME1/ME2-verified convention): look +UU = view turned LEFT, control rotation +UU =
        // turn RIGHT -> yaw flips; pitch +up on both sides -> 1:1.
        g_seedRemYawUU   = -static_cast<int>(ME2VR::CalcViewHook::HeadLookYawUU());
        g_seedRemPitchUU =  static_cast<int>(ME2VR::CalcViewHook::HeadLookPitchUU());
        g_seedDoneYawUU = 0; g_seedDonePitchUU = 0;
        g_aimRefYawDeg = headYawDeg; g_aimRefPitchDeg = headPitchDeg;
        g_aimAppliedYawUU = 0; g_aimAppliedPitchUU = 0;
        g_aimActive = true;
        ME2VR::Log::Line("Head aim engaged.");
    }

    // Advance the ramp: ~25% of the remainder per frame (min 150 UU so the tail doesn't crawl).
    const auto rampStep = [](int& rem, int& done) noexcept {
        if (rem == 0) return;
        constexpr int kMinStep = 150;
        int step = rem / 4;
        if (rem > 0) { if (step < kMinStep) step = (rem < kMinStep ? rem : kMinStep); }
        else         { if (step > -kMinStep) step = (rem > -kMinStep ? rem : -kMinStep); }
        done += step; rem -= step;
    };
    rampStep(g_seedRemYawUU, g_seedDoneYawUU);
    rampStep(g_seedRemPitchUU, g_seedDonePitchUU);

    const float sy = g_headAimInvYaw.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const float sp = g_headAimInvPitch.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
    const int headYawUU = static_cast<int>(sy * WrapDegF(headYawDeg - g_aimRefYawDeg) * kDegToUU) + g_seedDoneYawUU;
    const int headPitchUU = static_cast<int>(sp * (headPitchDeg - g_aimRefPitchDeg) * kDegToUU) + g_seedDonePitchUU;
    // ME1 model: strip last frame's injection, add this frame's on top of the game's own (stick) aim.
    const int rawStickYaw = y - g_aimAppliedYawUU;
    const int rawStickPitch = p - g_aimAppliedPitchUU;
    int newYaw = rawStickYaw + headYawUU;
    int newPitch = NormUU(rawStickPitch) + headPitchUU;
    const int clampUU = static_cast<int>(80.0f * kDegToUU);
    if (newPitch > clampUU) newPitch = clampUU; else if (newPitch < -clampUU) newPitch = -clampUU;
    WriteCtrlRot(pc, newPitch, newYaw, r);
    g_aimAppliedYawUU = headYawUU; g_aimAppliedPitchUU = headPitchUU;
}

// [HEADAIM] handoff support, ME2 parity: while the seed ramp is transferring the head-look offset
// into the aim, the UNTRANSFERRED remainder must still be rendered as view-look, so the two halves
// always sum to the full offset and the view never jumps during the glide.
bool IsHeadAimActive() noexcept { return g_aimActive; }
void GetAimSeedRem(int* yawUU, int* pitchUU) noexcept { *yawUU = g_seedRemYawUU; *pitchUU = g_seedRemPitchUU; }
void SetHeadAimEnabled(bool on) noexcept { g_headAimOn.store(on, std::memory_order_release); }
bool GetHeadAimEnabled() noexcept { return g_headAimOn.load(std::memory_order_acquire); }
void SetHeadAimInvertYaw(bool on) noexcept { g_headAimInvYaw.store(on, std::memory_order_release); }
bool GetHeadAimInvertYaw() noexcept { return g_headAimInvYaw.load(std::memory_order_acquire); }
void SetHeadAimInvertPitch(bool on) noexcept { g_headAimInvPitch.store(on, std::memory_order_release); }
bool GetHeadAimInvertPitch() noexcept { return g_headAimInvPitch.load(std::memory_order_acquire); }
void SetHeadAimWeaponOnly(bool on) noexcept { g_headAimWeaponOnly.store(on, std::memory_order_release); }
bool GetHeadAimWeaponOnly() noexcept { return g_headAimWeaponOnly.load(std::memory_order_acquire); }
// [AIMCOVER] A/B switch for the cover stand-down. ON (default) = the fix; OFF = the pre-fix
// behaviour where head aim keeps writing ControlRotation while the game drives the cover lean.
// Exposed because "the camera swings off target" is a feel judgement, and flipping it in-headset
// between two firefights is a far better test than swapping dlls between sessions.
void SetHeadAimCoverOff(bool on) noexcept { g_headAimCoverOff.store(on, std::memory_order_release); }
bool GetHeadAimCoverOff() noexcept { return g_headAimCoverOff.load(std::memory_order_acquire); }

bool IsMenuInputOwned(unsigned char* moveValue, unsigned char* lookValue) noexcept
{
    if (moveValue != nullptr) *moveValue = 0;
    if (lookValue != nullptr) *lookValue = 0;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return false;
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    unsigned char move = 0, look = 0;
    if (!SafeReadByte(pc + kPcIgnoreMove, &move) ||
        !SafeReadByte(pc + kPcIgnoreLook, &look))
        return false;
    if (moveValue != nullptr) *moveValue = move;
    if (lookValue != nullptr) *lookValue = look;
    return move != 0 && look != 0;
}

bool IsGamePaused() noexcept
{
    // P1 ULocalPlayer -> Actor(0x68, PlayerController) -> AActor::WorldInfo(0x198) -> Pauser(0x670).
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return false;
    std::uintptr_t pc = 0, wi = 0, pauser = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc == 0) return false;
    if (!SafeReadPtr(pc + 0x198, &wi) || wi == 0) return false;
    if (!SafeReadPtr(wi + 0x670, &pauser)) return false;
    return pauser != 0;
}

void GetPauseChain(unsigned long long out[5]) noexcept
{
    for (int i = 0; i < 5; ++i) out[i] = 0;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    out[0] = lp; if (lp == 0) return;
    std::uintptr_t pc = 0, wi = 0, pauser = 0;
    if (!SafeReadPtr(lp + 0x68, &pc)) return; out[1] = pc; if (pc == 0) return;
    if (!SafeReadPtr(pc + 0x198, &wi)) return; out[2] = wi; if (wi == 0) return;
    if (SafeReadPtr(wi + 0x670, &pauser)) out[3] = pauser;
    int playersOnly = 0;
    if (SafeReadInt(wi + 0x7C0, &playersOnly)) out[4] = (playersOnly & 0x00000400) != 0;
}

// Discovery: dump the GFx UI object graph reachable from P1, with class names, so the mod can find which
// object/field tells the mod a full-screen screen (galaxy map / journal / squad) is open. Read-only.
// P1 ULocalPlayer -> ViewportClient(0x594, USFXGameViewportClient) -> GFxUIController(0x1A8, UGFxInteraction).
void DumpGfxState() noexcept
{
    char nm[96] = {};
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) { ME2VR::Log::Line("[ME3DISC] GFXDUMP: no P1"); return; }

    ME2VR::Log::Line("[ME3DISC] ===== GFXDUMP =====");
    if (ClassNameOf(lp, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP P1 class=") + nm);

    std::uintptr_t pc = 0; SafeReadPtr(lp + 0x68, &pc);
    if (pc && ClassNameOf(pc, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP PC(Actor) ") + Hex(pc) + " class=" + nm);
    std::uintptr_t cam = 0; if (pc) SafeReadPtr(pc + 0x684, &cam);   // APlayerController.PlayerCamera
    if (cam && ClassNameOf(cam, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP PlayerCamera ") + Hex(cam) + " class=" + nm);
    std::uintptr_t vpc = 0; SafeReadPtr(lp + 0x594, &vpc);
    if (vpc && ClassNameOf(vpc, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP ViewportClient ") + Hex(vpc) + " class=" + nm);
    std::uintptr_t ctrl = 0; if (vpc) SafeReadPtr(vpc + 0x1A8, &ctrl);
    if (ctrl && ClassNameOf(ctrl, nm, sizeof(nm))) ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP GFxUIController ") + Hex(ctrl) + " class=" + nm);

    // Scan PC / viewport client / GFx controller members for (a) direct UObjects and (b) object-arrays,
    // logging class names + ALL elements. A full-screen screen (galaxy map) shows up as a movie/handler.
    const std::uintptr_t bases[4] = { pc, cam, vpc, ctrl };
    const char* baseName[4] = { "pc", "cam", "vpc", "ctrl" };
    for (int b = 0; b < 4; ++b)
    {
        if (bases[b] == 0) continue;
        for (std::uintptr_t off = 0x40; off <= 0x900; off += 8)
        {
            std::uintptr_t p = 0;
            if (!SafeReadPtr(bases[b] + off, &p) || p < 0x10000) continue;
            if (ClassNameOf(p, nm, sizeof(nm)))
            {
                ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP ") + baseName[b] + "+" + Hex(off) + " -> obj class=" + nm);
                continue;
            }
            // object-array { Data=p, Num@+8 }: only if elem0 is a real object, then log all elements.
            int num = 0;
            std::uintptr_t e0 = 0;
            if (SafeReadInt(bases[b] + off + 8, &num) && num >= 1 && num <= 64 &&
                SafeReadPtr(p, &e0) && e0 >= 0x10000 && ClassNameOf(e0, nm, sizeof(nm)))
            {
                ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP ") + baseName[b] + "+" + Hex(off) + " -> TArray num=" + std::to_string(num) + ":");
                const int cap = num < 16 ? num : 16;
                for (int i = 0; i < cap; ++i)
                {
                    std::uintptr_t e = 0;
                    if (SafeReadPtr(p + static_cast<std::uintptr_t>(i) * 8, &e) && e >= 0x10000 && ClassNameOf(e, nm, sizeof(nm)))
                        ME2VR::Log::Line(std::string("[ME3DISC] GFXDUMP     [") + std::to_string(i) + "] " + nm);
                }
            }
        }
    }
    ME2VR::Log::Line("[ME3DISC] ===== GFXDUMP end =====");
}

// True when the galaxy map is open: PlayerCamera(PC+0x684).CurrentMode(+0x558) is the galaxy camera
// behavior (class "BioCameraBehaviorGalaxy"). Gameplay = SFXCameraMode_Explore/Combat; conversations
// have their own mode -> they stay stereo. Validated live 2026-06-27.
bool IsGalaxyMapOpen() noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return false;
    std::uintptr_t pc = 0;   if (!SafeReadPtr(lp + 0x68, &pc) || pc == 0) return false;
    std::uintptr_t cam = 0;  if (!SafeReadPtr(pc + 0x684, &cam) || cam == 0) return false;
    std::uintptr_t mode = 0; if (!SafeReadPtr(cam + 0x558, &mode) || mode == 0) return false;
    char nm[96] = {};
    if (!ClassNameOf(mode, nm, sizeof(nm))) return false;
    return std::strstr(nm, "Galaxy") != nullptr;
}

void TryDumpOnce() noexcept
{
    if (g_dumped.load(std::memory_order_acquire)) return;

    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) return;

    // 1) GEngine
    std::uintptr_t gEngine = 0;
    if (!SafeReadPtr(base + kGEngineRva, &gEngine) || gEngine == 0)
    {
        // Engine global not populated yet (very early), or wrong RVA. Don't latch; retry.
        return;
    }

    // 2) GamePlayers TArray on the engine
    std::uintptr_t playersData = 0;
    int playersNum = 0;
    if (!SafeReadPtr(gEngine + kEngineGamePlayers + kTArrayData, &playersData) ||
        !SafeReadInt(gEngine + kEngineGamePlayers + kTArrayNum, &playersNum))
    {
        return;
    }
    if (playersData == 0 || playersNum < 1 || playersNum > 8)
    {
        // Not in a game world yet (no local player). Log a few times, then go quiet.
        const auto w = g_waitLogs.fetch_add(1, std::memory_order_relaxed);
        if (w < 4)
        {
            ME2VR::Log::Line("[ME3DISC] EngineProbe waiting: GEngine=" + Hex(gEngine) +
                             " GamePlayers.Num=" + std::to_string(playersNum) +
                             " (load a save / reach gameplay)");
        }
        return;
    }

    // 3) Primary ULocalPlayer (P1) = GamePlayers[0]
    std::uintptr_t p1 = 0;
    if (!SafeReadPtr(playersData + 0 * sizeof(void*), &p1) || p1 == 0) return;

    // 4) ULocalPlayer fields (the offsets the mod claims are LE1-identical -- confirm live)
    std::uintptr_t viewState = 0, viewportClient = 0;
    float origin[2] = {-1, -1}, size[2] = {-1, -1};
    int controllerId = -999;
    SafeReadPtr(p1 + kLpViewState, &viewState);
    SafeReadPtr(p1 + kLpViewportClient, &viewportClient);
    SafeReadFloat2(p1 + kLpOrigin, origin);
    SafeReadFloat2(p1 + kLpSize, size);
    SafeReadInt(p1 + kLpControllerId, &controllerId);

    // 5) FViewportClient vtable off the viewport client -> Draw slot
    std::uintptr_t vpcPrimaryVtable = 0, fvpVtable = 0, drawAddr = 0;
    if (viewportClient != 0)
    {
        SafeReadPtr(viewportClient + 0, &vpcPrimaryVtable);
        if (SafeReadPtr(viewportClient + kVpcFViewportClientVtable, &fvpVtable) && fvpVtable != 0)
        {
            SafeReadPtr(fvpVtable + static_cast<std::uintptr_t>(kDrawVtableSlot) * sizeof(void*), &drawAddr);
        }
    }

    // Latch BEFORE logging so the mod dumps exactly once.
    bool expected = false;
    if (!g_dumped.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    ME2VR::Log::Line("[ME3DISC] ===== EngineProbe dump (read-only) =====");
    ME2VR::Log::Line("[ME3DISC] base=" + Hex(base) + " GEngine=" + Hex(gEngine) +
                     " (rva " + Hex(kGEngineRva) + ") GamePlayers.Num=" + std::to_string(playersNum));
    ME2VR::Log::Line("[ME3DISC] P1 ULocalPlayer=" + Hex(p1));

    char fbuf[256] = {};
    sprintf_s(fbuf,
              "[ME3DISC]   Origin=(%.3f,%.3f) Size=(%.3f,%.3f) ControllerId=%d  <-- expect Origin~(0,0) Size~(1,1)",
              origin[0], origin[1], size[0], size[1], controllerId);
    ME2VR::Log::Line(fbuf);

    ME2VR::Log::Line("[ME3DISC]   ViewState=" + Hex(viewState) +
                     " ViewportClient=" + Hex(viewportClient));

    if (drawAddr != 0)
    {
        const std::uintptr_t drawRva = (drawAddr > base) ? (drawAddr - base) : 0;
        ME2VR::Log::Line("[ME3DISC]   FViewportClient vtable=" + Hex(fvpVtable) +
                         " (rva " + Hex((fvpVtable > base) ? fvpVtable - base : 0) + ")");
        ME2VR::Log::Line("[ME3DISC]   >>> FViewportClient::Draw (slot 2) = " + Hex(drawAddr) +
                         "  rva=MassEffect2.exe+" + Hex(drawRva) + " <<<");
        ME2VR::Log::Line("[ME3DISC]   VPC primary vtable rva=" +
                         Hex((vpcPrimaryVtable > base) ? vpcPrimaryVtable - base : 0));
    }
    else
    {
        ME2VR::Log::Line("[ME3DISC]   Draw slot resolve FAILED (viewportClient=" + Hex(viewportClient) +
                         " fvpVtable=" + Hex(fvpVtable) + ")");
    }

    const bool offsetsSane =
        origin[0] > -2.0f && origin[0] < 2.0f && size[0] > 0.1f && size[0] < 2.0f &&
        viewportClient > 0x10000 && viewState != 0;
    ME2VR::Log::Line(std::string("[ME3DISC] EngineProbe verdict: ") +
                     (offsetsSane ? "OFFSETS LOOK VALID for this build -> proceed to CalcSceneView hunt"
                                  : "OFFSETS LOOK WRONG -> installed exe likely differs from SDK build; switch to pattern scan"));
    ME2VR::Log::Line("[ME3DISC] ==========================================");
}


// [ME3BLEND] LE3 drives every camera change through SFXCameraMode_Interpolate, and it is the most
// common mode in a play session by a wide margin. Mapping it to no FP state made first person collapse
// to third person for the whole transition and snap back afterwards.
// Resolve the transition's TARGET mode and follow that instead. Returns the FP state index of the
// target, or -2 when the layout is not known yet (caller then holds the previous state).
// MEASURED by [INTERP] on a live transition: From=+0x158, To=+0x160. LE2's 0x250/0x258 are meaningless
// here. Seeded with the measured values so the very first transition of a session already tracks
// correctly instead of holding the previous state until discovery runs; the probe still re-confirms
// them and overwrites if the layout ever differs.
std::uintptr_t g_interpFromOff = 0x158;
std::uintptr_t g_interpToOff   = 0x160;

// [INTERP] One-shot: on an Interpolate object, any slot holding a pointer to an SFXCameraMode_* is
// either From or To. Two such slots, in address order, are From then To (LE2 orders them that way and
// the log confirms the pair appears adjacent). Read-only and SEH-guarded like every other probe here.
void InterpDiscoverLayout(std::uintptr_t interp) noexcept
{
    static bool s_confirmed = false;
    if (s_confirmed) return;
    s_confirmed = true;
    std::uintptr_t hits[8] = {};
    std::uintptr_t offs[8] = {};
    int n = 0;
    for (std::uintptr_t off = 0x40; off < 0x600 && n < 8; off += 8)
    {
        std::uintptr_t v = 0;
        if (!SafeReadPtr(interp + off, &v) || v < 0x10000) continue;
        char cn[64] = {};
        if (!ClassNameOf(v, cn, sizeof(cn))) continue;
        if (std::strncmp(cn, "SFXCameraMode_", 14) != 0) continue;
        if (std::strcmp(cn, "SFXCameraMode_Interpolate") == 0) continue;   // self-reference
        hits[n] = v; offs[n] = off; ++n;
    }
    if (n < 2) return;
    g_interpFromOff = offs[0];
    g_interpToOff   = offs[1];
    char a[64] = {}, b[64] = {};
    ClassNameOf(hits[0], a, sizeof(a));
    ClassNameOf(hits[1], b, sizeof(b));
    char line[240] = {};
    sprintf_s(line, "[INTERP] transition layout: From=+%03llX (%s)  To=+%03llX (%s)",
              static_cast<unsigned long long>(g_interpFromOff), a,
              static_cast<unsigned long long>(g_interpToOff), b);
    ME2VR::Log::Line(line);
}

// FP state index of a transition's target mode; -2 = layout unknown, hold the previous state.
int InterpTargetState(std::uintptr_t interp) noexcept
{
    InterpDiscoverLayout(interp);
    if (g_interpToOff == 0) return -2;
    std::uintptr_t to = 0;
    if (!SafeReadPtr(interp + g_interpToOff, &to) || to < 0x10000) return -2;
    char cn[64] = {};
    if (!ClassNameOf(to, cn, sizeof(cn))) return -2;
    return FpStateIndexForMode(cn);
}

// [COVER] LE3 exposes no cover CAMERA mode - taking cover leaves the mode at SFXCameraMode_Combat,
// identical to standing in the open, which is why cover could not be detected from the camera at all.
// [COVERSCAN] found the flag by counting boolean flips across the controller and the pawn while
// cover was deliberately used: PlayerController +0x5AC flipped 22 times against a best of 6 anywhere else,
// and read 1 in the report that landed while settled behind cover.
// MEASURED across three long covers (07:04:29-07:05:46, 07:05:54-07:06:45, 07:06:46-07:08:41):
// pawn +0x3A9 and +0xB6D both sat at 1 for every one and at 0 between them. The old +0x5AC was a
// camera-transition pulse, not cover: those covers produced zero pulses from it.
// [COVERBYTES 2026-08-21] The two are NOT two halves of one signal, and requiring both was wrong.
// Printing them on every change settled it in one session:
//   +0xB6D is COVER. It leads on entry (a=0 b=1 first, then a=1 b=1 once the player crouches) and clears
//     with the exit. Every cover in the 18:50-18:53 session began with it.
//   +0x3A9 is the CROUCH/stance flag, not cover. It sat at 1 alone for 16s at 18:52:48 and for 6.6s
//     at 18:50:53, with no cover anywhere near either.
// So "both bytes" actually meant "in cover AND crouched", and every stretch of upright cover
// (a=0 b=1: 2.1s, 2.9s, 2.1s, 2.5s, 3.1s in that one session) read as NOT in cover - which is the
// camera getting stuck in first person when popping out of crouch cover. Cover keys off +0xB6D
// alone now; +0x3A9 is logged only, and gates nothing.
constexpr std::uintptr_t kPawnCrouch = 0x3A9;
constexpr std::uintptr_t kPawnCover  = 0xB6D;
std::atomic_bool g_coverThirdPerson{true};   // "Third person in cover" (ini CoverThirdPerson)
std::atomic_bool g_inCover{false};           // [AIMCOVER] live cover state, published for the aim gate
// [VRCINE] LE3 identifies conversations through the CAMERA (BioCameraBehaviorConversation was
// observed live during one), not through LE2's game-mode 5 - which has never appeared in a log.
std::atomic_bool g_convoCamera{false};
std::atomic_bool g_gameplayCam{false};   // camera is SFXCameraMode_* = the gameplay camera system
std::atomic_bool g_photoCamera{false};   // [PHOTOVR] SFXCameraMode_PhotoFree = photo mode free camera
// [COMBATCTX] Authoritative combat-vs-roaming context.
// LE3 is NOT like ME1/ME2: weapons cannot be drawn in hub areas (Citadel, Normandy) at all, so
// "roaming" is a real game state rather than "player happens to be holstered". The game declares it
// by swapping the player PAWN - SFXPawn_Player<Class> in mission areas, SFXPawn_Player<Class>NonCombat
// in hubs (measured 2026-08-14: PlayerVanguard at 07:17:42, PlayerVanguardNonCombat at 07:18:41).
// The pawn is the strong signal: it does not flap per camera shot and does not blend through
// SFXCameraMode_Interpolate the way the camera-mode name does. The camera family then distinguishes
// weapon-out from holstered WITHIN a mission area. Unknown/blend states hold the last verdict, so a
// transition can never produce a one-frame context flip.
std::atomic_bool g_nonCombatPawn{false};   // pawn class says NonCombat = hub, weapons impossible
std::atomic_bool g_mechPawn{false};        // operating a vehicle/mech/turret = armed by definition
std::atomic_bool g_combatContext{false};   // final verdict: true = combat-capable AND weapon out
// Case-insensitive substring. Used for the pawn verdict so a capitalisation difference in some
// class or DLC pawn name cannot silently flip the game into permanent combat context.
bool ContainsNoCase(const char* hay, const char* needle) noexcept
{
    if (hay == nullptr || needle == nullptr) return false;
    auto lc = [](char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    for (const char* p = hay; *p != '\0'; ++p)
    {
        int i = 0;
        while (needle[i] != '\0' && p[i] != '\0' && lc(p[i]) == lc(needle[i])) ++i;
        if (needle[i] == '\0') return true;
    }
    return false;
}
// [HEADAIM] weapon out = Combat-family camera mode. Cover reports as plain Combat and ADS keeps the
// Combat name too (see the g_fpDefs census notes), so the Combat prefix covers all armed states.
std::atomic_bool g_weaponOut{false};
bool IsConvoCamera() noexcept { return g_convoCamera.load(std::memory_order_acquire); }
bool IsGameplayCamera() noexcept { return g_gameplayCam.load(std::memory_order_acquire); }
bool IsPhotoCamera() noexcept { return g_photoCamera.load(std::memory_order_acquire); }
bool IsRoamingArea() noexcept { return g_nonCombatPawn.load(std::memory_order_acquire); }
bool IsCombatContext() noexcept { return g_combatContext.load(std::memory_order_acquire); }
bool IsWeaponOut() noexcept { return g_weaponOut.load(std::memory_order_acquire); }

// [CAMFOV] Runtime discovery of the LE3 camera FOV field(s), for the FILLSRC glass-smear fix.
// ME2 hardcodes the field (cam+0x47C); LE3's camera layout differs (PlayerCamera itself moved
// 0x6A0 -> 0xE28), so it is MEASURED: scan the camera object for floats equal to the live raw
// FOV during stable wide gameplay, and lock the offsets once the same set has held ~90
// consecutive frames. Multiple hits are expected (FOVAngle plus its cache copies) - all are
// written and all are restored, so equality between them is preserved by construction.
// [CAMFOV WIDEN 2026-08-14] The window was 0x100..0xA00 and found only 2 copies, yet [FILLSRC]
// reported "active" while the glass/refraction smear persisted - the signature of writing SOME of
// the FOV copies but not all. UE3 mirrors the FOV across FOVAngle, CameraCache.POV.FOV,
// LastFrameCameraCache.POV.FOV and the ViewTarget POVs, and LE3's PlayerCamera is a far larger actor
// than ME2's (the pointer alone moved pc+0x6A0 -> pc+0xE28), so copies sit past the old ceiling.
// Whatever the distortion pass samples has to be written too or its screen-space offsets stay
// derived from the raw narrow FOV, which is exactly the smear.
constexpr int kCamFovMaxOffs = 24;
constexpr std::uintptr_t kCamFovScanLo = 0x100;
constexpr std::uintptr_t kCamFovScanHi = 0x2000;
std::uintptr_t g_camFovObj = 0;
std::uintptr_t g_camFovOffs[kCamFovMaxOffs] = {};
int  g_camFovCount = 0;
int  g_camFovStable = 0;
std::atomic_bool g_camFovLocked{false};
// Widening the window also widens the false-positive risk: gameplay FOV is a CONSTANT 70.0, so any
// unrelated float parked at 70.0 matches the scan. Real FOV copies track the value when it actually
// moves (ADS, zoom); a coincidental constant does not. So after locking, whenever the live FOV has
// moved well away from the lock value, re-read every locked offset and PRUNE the ones that did not
// follow. Pruning only ever REMOVES writes, so it cannot introduce a new failure, and it needs 20
// consecutive disagreements so a mid-write sample can never evict a real field.
float g_camFovLockDeg = 0.0f;
int   g_camFovMiss[kCamFovMaxOffs] = {};
bool  g_camFovPruneLogged = false;

bool CamFovReadF(std::uintptr_t addr, float* out) noexcept
{
    __try { *out = *reinterpret_cast<const volatile float*>(addr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CamFovWriteF(std::uintptr_t addr, float v) noexcept
{
    __try { *reinterpret_cast<volatile float*>(addr) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CamFovResolveCam(std::uintptr_t* outCam) noexcept
{
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp < 0x10000) return false;
    std::uintptr_t pc = 0, cam = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    if (!SafeReadPtr(pc + kPcPlayerCamera, &cam) || cam < 0x10000) return false;
    *outCam = cam;
    return true;
}

// Drop locked offsets that stop tracking the FOV once it genuinely moves. See the note above.
void CamFovPruneTick(std::uintptr_t cam) noexcept
{
    const float liveDeg = ME2VR::CalcViewHook::GetGameRawFovH() * 2.0f * 57.2957795f;
    if (!(liveDeg > 10.0f && liveDeg < 170.0f)) return;
    if (liveDeg > g_camFovLockDeg - 5.0f && liveDeg < g_camFovLockDeg + 5.0f) return;   // needs a real move
    for (int i = 0; i < g_camFovCount; ++i)
    {
        float f = 0.0f;
        if (!CamFovReadF(cam + g_camFovOffs[i], &f)) continue;
        if (f > liveDeg - 0.5f && f < liveDeg + 0.5f) { g_camFovMiss[i] = 0; continue; }
        if (++g_camFovMiss[i] < 20) continue;
        char b[160] = {};
        sprintf_s(b, "[CAMFOV] pruning +0x%llX - held %.1f while the camera moved to %.1f deg (not a FOV field)",
                  static_cast<unsigned long long>(g_camFovOffs[i]),
                  static_cast<double>(f), static_cast<double>(liveDeg));
        ME2VR::Log::Line(b);
        for (int j = i; j + 1 < g_camFovCount; ++j)
        {
            g_camFovOffs[j] = g_camFovOffs[j + 1];
            g_camFovMiss[j] = g_camFovMiss[j + 1];
        }
        --g_camFovCount;
        --i;
        g_camFovPruneLogged = true;
    }
}

void CamFovDiscoverTick(std::uintptr_t cam) noexcept
{
    if (cam < 0x10000) return;
    if (g_camFovLocked.load(std::memory_order_relaxed)) { CamFovPruneTick(cam); return; }
    // Live raw FOV from the view build (degrees, full horizontal). Only scan stable wide
    // gameplay: ADS/cines narrow it and transitions blend it - those frames cannot hold a match.
    const float liveDeg = ME2VR::CalcViewHook::GetGameRawFovH() * 2.0f * 57.2957795f;
    if (!(liveDeg > 60.0f && liveDeg < 120.0f) || ReadGameMode() != 0) { g_camFovStable = 0; return; }

    unsigned char buf[kCamFovScanHi - kCamFovScanLo];
    if (!SafeReadBlock(cam + kCamFovScanLo, buf, sizeof(buf))) { g_camFovStable = 0; return; }
    std::uintptr_t offs[kCamFovMaxOffs] = {};
    int count = 0;
    for (std::uintptr_t o = 0; o + 4 <= sizeof(buf) && count < kCamFovMaxOffs; o += 4)
    {
        float f = 0.0f;
        std::memcpy(&f, buf + o, 4);
        if (f > liveDeg - 0.02f && f < liveDeg + 0.02f) offs[count++] = kCamFovScanLo + o;
    }
    if (count == 0) { g_camFovStable = 0; return; }
    const bool same = (cam == g_camFovObj) && (count == g_camFovCount) &&
                      (std::memcmp(offs, g_camFovOffs, sizeof(offs)) == 0);
    if (!same)
    {
        g_camFovObj = cam;
        g_camFovCount = count;
        std::memcpy(g_camFovOffs, offs, sizeof(offs));
        g_camFovStable = 0;
        return;
    }
    if (++g_camFovStable < 90) return;
    g_camFovLockDeg = liveDeg;
    for (int i = 0; i < kCamFovMaxOffs; ++i) g_camFovMiss[i] = 0;
    g_camFovLocked.store(true, std::memory_order_release);
    char b[240] = {};
    int n = sprintf_s(b, "[CAMFOV] locked %d offset(s) at %.1f deg:", count, static_cast<double>(liveDeg));
    for (int i = 0; i < count && n > 0 && n < 200; ++i)
        n += sprintf_s(b + n, sizeof(b) - static_cast<size_t>(n), " +0x%llX",
                       static_cast<unsigned long long>(g_camFovOffs[i]));
    ME2VR::Log::Line(b);
}

bool CamFovReady() noexcept { return g_camFovLocked.load(std::memory_order_acquire); }
bool GetCameraFovDeg(float* outDeg) noexcept
{
    if (outDeg == nullptr || !CamFovReady()) return false;
    std::uintptr_t cam = 0;
    if (!CamFovResolveCam(&cam)) return false;
    float f = 0.0f;
    if (!CamFovReadF(cam + g_camFovOffs[0], &f)) return false;
    if (!(f > 1.0f && f < 179.0f)) return false;   // not a sane FOV -> wrong object, refuse
    *outDeg = f;
    return true;
}
bool SetCameraFovDeg(float deg, float* outPrev) noexcept
{
    if (!(deg > 1.0f && deg < 179.0f) || !CamFovReady()) return false;
    std::uintptr_t cam = 0;
    if (!CamFovResolveCam(&cam)) return false;
    bool any = false;
    for (int i = 0; i < g_camFovCount; ++i)
    {
        float prev = 0.0f;
        if (!CamFovReadF(cam + g_camFovOffs[i], &prev)) continue;
        if (!(prev > 1.0f && prev < 179.0f)) continue;   // garbage/wrong object: do not write
        if (!any && outPrev != nullptr) *outPrev = prev;
        if (CamFovWriteF(cam + g_camFovOffs[i], deg)) any = true;
    }
    return any;
}

// Takes the VALIDATED pawn (caller has already checked its class name), not the PlayerController.
// Neither byte is a boolean - both are stance TYPES. Low covers read 1; a later session of wall
// cover read 2 the whole way through, and an == 1 test silently matched only half the game's cover.
// Values above 4 are treated as garbage, not cover: these reads survive pawn swaps and loads, and
// "any nonzero byte" is how a stale pointer becomes a permanently third-person camera.
// `cover` is the verdict (from +0xB6D). `crouch` (+0x3A9) is returned for the log only.
bool ReadCoverBytes(std::uintptr_t pawn, bool* cover,
                    unsigned char* rawCrouch, unsigned char* rawCover) noexcept
{
    *cover = false; *rawCrouch = 0; *rawCover = 0;
    if (pawn < 0x10000) return false;
    unsigned char a = 0, b = 0;
    if (!SafeReadByte(pawn + kPawnCrouch, &a)) return false;
    if (!SafeReadByte(pawn + kPawnCover, &b)) return false;
    *rawCrouch = a; *rawCover = b;
    *cover = b >= 1 && b <= 4;
    return true;
}

bool GetCoverThirdPerson() noexcept { return g_coverThirdPerson.load(std::memory_order_relaxed); }
void SetCoverThirdPerson(bool on) noexcept { g_coverThirdPerson.store(on, std::memory_order_relaxed); }
bool IsInCover() noexcept { return g_inCover.load(std::memory_order_acquire); }   // [AIMCOVER]
void ApplyFirstPerson() noexcept
{
    FpEnsureInit();
    const bool fpCam = g_fpEnabled.load(std::memory_order_acquire);
    const bool meshOverride = g_meshHidden.load(std::memory_order_acquire);
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    if (lp == 0) return;
    if (IsGamePaused()) return;   // pause-freeze: a load is launched from the (paused) menu -> write nothing
    std::uintptr_t pc = 0;
    if (!SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return;
    char pcName[96] = {};
    if (!ClassNameOf(pc, pcName, sizeof(pcName)) || std::strstr(pcName, "Controller") == nullptr || ActorIsDying(pc)) return;

    std::uintptr_t cam = 0, mode = 0;
    char modeName[96] = {};
    if (SafeReadPtr(pc + kPcPlayerCamera, &cam) && cam >= 0x10000 &&
        SafeReadPtr(cam + kCamCurrentMode, &mode) && mode >= 0x10000)
        ClassNameOf(mode, modeName, sizeof(modeName));
    CamFovDiscoverTick(cam);   // [CAMFOV] find the camera FOV field(s) for the FILLSRC glass fix

    std::uintptr_t pawn = 0;
    char pawnName[96] = {};
    const bool havePawn = SafeReadPtr(pc + kPcPawn, &pawn) && pawn >= 0x10000 &&
                          ClassNameOf(pawn, pawnName, sizeof(pawnName)) && std::strstr(pawnName, "Pawn") != nullptr &&
                          !ActorIsDying(pawn);
    static std::uintptr_t s_lastPawn = 0;
    static int s_stableFrames = 0;
    if (havePawn && pawn == s_lastPawn) { if (s_stableFrames < 100000) ++s_stableFrames; }
    else { s_stableFrames = 0; s_lastPawn = havePawn ? pawn : 0; }
    const bool stable = havePawn && s_stableFrames >= 30;

    // [COVERWATCH] Trace the two candidates the dwell pass surfaced (pawn +0x3A9 and +0xB6D) directly.
    // They held for 28.5s and 17.1s and moved together, so one may well be cover and the other
    // something that merely coincides with it. Logging each change with the camera mode settles it
    // against a single cover session, without needing the correlation probe to reach quorum.
    {
        // Use the pawn ApplyFirstPerson already VALIDATED (class name checked, not dying) instead of a
        // second raw pointer read. In the 07:11 session the raw read went bad for the whole run:
        // COVERWATCH and PAWNSEEN fell silent while COVERSCAN4 kept reporting - and with the cover
        // bytes unreadable, cover could not trigger anything no matter what the player did.
        if (havePawn)
        {
            const std::uintptr_t wpawn = pawn;
            const std::uintptr_t offs[2] = { 0x3A9, 0xB6D };
            static unsigned char s_prev[2] = { 0xFF, 0xFF };
            for (int k = 0; k < 2; ++k)
            {
                unsigned char v = 0;
                if (!SafeReadByte(wpawn + offs[k], &v)) continue;
                if (v == s_prev[k]) continue;
                s_prev[k] = v;
                char b[200] = {};
                sprintf_s(b, "[COVERWATCH] pawn+0x%03llX -> %d   (camera %s)",
                          static_cast<unsigned long long>(offs[k]), static_cast<int>(v),
                          modeName[0] ? modeName : "?");
                ME2VR::Log::Line(b);
            }
        }
    }
    // [COVERSCAN4] Cover is taken ~4 times and holds each for ~5 seconds. That pattern is the signal:
    // count, per boolean byte, how many times it stays HIGH for between 2 and 12 seconds. A cover flag
    // lands 4-5 such runs in a session; almost nothing else in these structures does.
    // The previous probe correlated against +0x5AC on the belief it marked cover. It does not - four
    // covers produced zero pulses from it, and every earlier pulse coincided with
    // SFXCameraMode_Interpolate, i.e. ANY camera transition. That premise is dropped entirely.
    {
        static CorrWatch s_pawnBand = {};
        static CorrWatch s_ctrlBand = {};
        static unsigned short s_pawnRun[kCorrRange] = {};
        static unsigned short s_ctrlRun[kCorrRange] = {};
        static unsigned short s_pawnBandHits[kCorrRange] = {};
        static unsigned short s_ctrlBandHits[kCorrRange] = {};
        static std::uintptr_t s_bandPawn = 0;

        struct BandFn
        {
            static void Tick(CorrWatch* w, unsigned short* run, unsigned short* hits,
                             std::uintptr_t obj, std::uintptr_t lo, float fps) noexcept
            {
                if (obj < 0x10000) return;
                unsigned char cur[kCorrRange];
                if (!SafeReadBlock(obj + lo, cur, kCorrRange)) return;
                if (!w->primed)
                {
                    for (int i = 0; i < kCorrRange; ++i)
                    { w->last[i] = cur[i]; w->boolOnly[i] = 1; run[i] = 0; hits[i] = 0; }
                    w->primed = true;
                    return;
                }
                const int loF = static_cast<int>(2.0f * fps);    // 2s in frames
                const int hiF = static_cast<int>(12.0f * fps);   // 12s in frames
                for (int i = 0; i < kCorrRange; ++i)
                {
                    if (!w->boolOnly[i]) continue;
                    if (cur[i] > 1) { w->boolOnly[i] = 0; continue; }
                    if (cur[i] == 1) { if (run[i] < 65000) ++run[i]; }
                    else
                    {
                        const int r = static_cast<int>(run[i]);
                        if (r >= loF && r <= hiF && hits[i] < 65000) ++hits[i];
                        run[i] = 0;
                    }
                    w->last[i] = cur[i];
                }
            }
            static void Report(const CorrWatch* w, const unsigned short* hits, std::uintptr_t lo,
                               const char* tag) noexcept
            {
                if (!w->primed) return;
                int bi[6] = {}, bc[6] = {};
                for (int i = 0; i < kCorrRange; ++i)
                {
                    if (!w->boolOnly[i] || hits[i] == 0) continue;
                    const int c = static_cast<int>(hits[i]);
                    for (int k = 0; k < 6; ++k)
                    {
                        if (c <= bc[k]) continue;
                        for (int j = 5; j > k; --j) { bc[j] = bc[j - 1]; bi[j] = bi[j - 1]; }
                        bc[k] = c; bi[k] = i; break;
                    }
                }
                char line[440] = {};
                int used = sprintf_s(line, sizeof(line), "[COVERSCAN4] %s 2-12s holds:", tag);
                if (bc[0] == 0) { used += sprintf_s(line + used, sizeof(line) - used, "  none yet"); }
                for (int k = 0; k < 6 && bc[k] > 0; ++k)
                    used += sprintf_s(line + used, sizeof(line) - used, "  +%03llX=%d(x%d)",
                                      static_cast<unsigned long long>(lo + static_cast<std::uintptr_t>(bi[k])),
                                      static_cast<int>(w->last[bi[k]]), bc[k]);
                ME2VR::Log::Line(line);
            }
        };

        const float fps = 90.0f;   // frame-rate estimate only sets the band edges; wide band tolerates it
        std::uintptr_t bpawn = 0;
        if (SafeReadPtr(pc + kPcPawn, &bpawn) && bpawn > 0x10000)
        {
            if (bpawn != s_bandPawn) { s_bandPawn = bpawn; s_pawnBand.primed = false; }
            BandFn::Tick(&s_pawnBand, s_pawnRun, s_pawnBandHits, bpawn, 0x100, fps);
        }
        BandFn::Tick(&s_ctrlBand, s_ctrlRun, s_ctrlBandHits, pc, 0x100, fps);

        static int s_bandReport = 0;
        if (++s_bandReport >= 900)
        {
            s_bandReport = 0;
            BandFn::Report(&s_pawnBand, s_pawnBandHits, 0x100, "pawn");
            BandFn::Report(&s_ctrlBand, s_ctrlBandHits, 0x100, "controller");
        }
    }

    // [COVER] In cover, hand the camera back to the game. First person in cover fights the
    // lean/peek the game drives itself, and third person is preferred there. The eye offset
    // glides rather than cutting, so this reads as the camera pulling out and back rather than a
    // swap. Logged on every transition: if this flag ever turns out to mean something other than
    // cover, the log says so immediately instead of it being a mystery in the headset.
    // [FPDIAG] Chain-health heartbeat. The 07:11 session burned a test because the pawn read failed
    // silently for the whole run and there was nothing in the log to say so. One line every ~15s:
    // every link, so the broken one is named instead of inferred.
    {
        static int s_fpDiagTick = 0;
        if (++s_fpDiagTick >= 900)
        {
            s_fpDiagTick = 0;
            unsigned char ca = 0xFF, cb2 = 0xFF;
            if (havePawn) { SafeReadByte(pawn + kPawnCrouch, &ca); SafeReadByte(pawn + kPawnCover, &cb2); }
            char hb[300] = {};
            sprintf_s(hb, "[FPDIAG] fp=%d pawn=%d(%s) stable=%d mode=%s crouch/cover=%d/%d",
                      fpCam ? 1 : 0, havePawn ? 1 : 0, pawnName[0] ? pawnName : "-", stable ? 1 : 0,
                      modeName[0] ? modeName : "-", static_cast<int>(ca), static_cast<int>(cb2));
            ME2VR::Log::Line(hb);
        }
    }
    bool coverFlag = false;
    unsigned char rawCrouch = 0, rawCover = 0;
    if (havePawn) ReadCoverBytes(pawn, &coverFlag, &rawCrouch, &rawCover);
    // [COVERHOLD] A named cover camera (SFXCameraMode_EnterCover and friends) is proof on its own,
    // the way ME2 arms from a named cover cam.
    const bool coverCam = std::strstr(modeName, "Cover") != nullptr;
    // [COVERBYTES] One line whenever the pawn's stance bytes change value. This is what separated
    // the cover byte from the crouch byte; keep it, it costs one line per stance change.
    {
        static unsigned char s_lastCrouch = 0xFF, s_lastCover = 0xFF;
        if (havePawn && (rawCrouch != s_lastCrouch || rawCover != s_lastCover))
        {
            s_lastCrouch = rawCrouch; s_lastCover = rawCover;
            char bb[200] = {};
            sprintf_s(bb, "[COVERBYTES] crouch=%u cover=%u   (camera %s)",
                      static_cast<unsigned>(rawCrouch), static_cast<unsigned>(rawCover),
                      modeName[0] ? modeName : "?");
            ME2VR::Log::Line(bb);
        }
    }
    const bool inCover = coverFlag;
    {
        static bool s_prevCover = false;
        if (inCover != s_prevCover)
        {
            s_prevCover = inCover;
            char cb[200] = {};
            sprintf_s(cb, "[COVER] %s   (camera %s, mode %d)", inCover ? "IN cover" : "out of cover",
                      modeName[0] ? modeName : "?", ReadGameMode());
            ME2VR::Log::Line(cb);
        }
    }
    // [VRCINE] publish whether the live camera is a conversation camera (the LE3 convo signal)
    g_convoCamera.store(std::strstr(modeName, "Conversation") != nullptr, std::memory_order_release);
    g_gameplayCam.store(std::strncmp(modeName, "SFXCameraMode_", 14) == 0, std::memory_order_release);
    // [PHOTOVR] Photo mode is a free camera for composing shots, not a cinematic and not a menu, so
    // it should render exactly like gameplay - in stereo. It was presenting flat because it satisfies
    // both halves of the cinematic test at once: the class is in the SFXCameraMode_ family so it
    // counts as a gameplay camera (failing the !gameplayCam eligibility guard), while its narrow
    // framing FOV trips the cine detector - so it classified as a FLAT cinematic. Measured
    // 2026-08-14 07:18:35: submit=CINEMATIC_MONO the instant SFXCameraMode_PhotoFree became active,
    // held for the whole ~6s session. Published here so the detector can veto it by name.
    g_photoCamera.store(std::strcmp(modeName, "SFXCameraMode_PhotoFree") == 0, std::memory_order_release);
    // [HEADAIM] weapon-out publish: Combat/CombatStorm (cover and ADS report as Combat too) plus
    // TightAim if it ever appears. SFXCameraMode_Interpolate is the transient blend the game routes
    // EVERY camera change through, and non-gameplay cameras (conversation, cine) say nothing about
    // the holster state - hold the last verdict through both so aim mode cannot flap mid-transition.
    // Update the weapon verdict ONLY from cameras that actually state it, and HOLD through every
    // other camera. LE3 swaps to a transient action camera constantly during a firefight - measured
    // 2026-08-14: SFXCameraMode_Melee, _Roll, _HitReaction and _EnterCover each flipped the verdict
    // to "no weapon" for ~0.5s, so head aim cut out every time the player rolled, meleed, took a hit
    // or entered cover. None of those cameras say anything about whether the gun is up; treating
    // "not Combat-prefixed" as "holstered" was the bug. Holding is correct for all of them, and it
    // also covers _Interpolate, the mech cameras and the SFXCameraTransition_*/SFXCameraAction_*
    // families without needing to enumerate them.
    if (std::strncmp(modeName, "SFXCameraMode_Combat", 20) == 0 ||     // Combat, CombatStorm
        std::strcmp(modeName, "SFXCameraMode_TightAim") == 0)          // ADS
        g_weaponOut.store(true, std::memory_order_release);
    else if (std::strncmp(modeName, "SFXCameraMode_Explore", 21) == 0) // Explore, ExploreStorm
        g_weaponOut.store(false, std::memory_order_release);
    // [COMBATCTX] Pawn identity first - it is the only signal that states outright whether this area
    // permits weapons. Only update from a NAMED pawn; an empty read holds the previous verdict.
    // Matched as a case-insensitive SUBSTRING and never against a specific class, so every origin
    // and class works the same: PlayerVanguardNonCombat, PlayerSentinelNonCombat, PlayerAdeptNonCombat
    // and so on. Vanguard is only what happened to be measured.
    if (pawnName[0] != 0)
    {
        g_nonCombatPawn.store(ContainsNoCase(pawnName, "NonCombat"), std::memory_order_release);
        // Anything the player OPERATES is armed by definition, and none of it reports a Combat
        // camera. Measured 2026-08-14: the Leviathan Triton is SFXPawn_DivingAtlas on
        // SFXCameraMode_Atlas and classified as "no weapon", which switched head aim off inside a
        // combat mech - the view rotated but ControlRotation did not, so the crosshair (drawn for
        // ControlRotation) separated from where the player was looking, the reported "aims below
        // the crosshair".
        // Identified by ABSENCE of "Player" rather than by listing vehicles: Shepard's own pawn is
        // always SFXPawn_Player<Class>[NonCombat], while everything climbed into is not
        // (SFXPawn_Atlas, SFXPawn_DivingAtlas, and the mounted turrets on Rannoch: Admiral Koris,
        // which the game likewise swaps the pawn for). That covers emplacements this build has never
        // seen without enumerating them. Degrades safely: the NonCombat check still outranks this,
        // so a hub can never be classified as combat, and cinematics veto head aim separately.
        g_mechPawn.store(!ContainsNoCase(pawnName, "Player"), std::memory_order_release);
    }
    const bool roaming = g_nonCombatPawn.load(std::memory_order_acquire);
    const bool mech = g_mechPawn.load(std::memory_order_acquire);
    // In a hub the answer is NO regardless of what the camera is doing. In a mission area the weapon
    // verdict decides - unless the player is piloting a mech, which is always armed.
    const bool combatNow = !roaming && (mech || g_weaponOut.load(std::memory_order_acquire));
    const bool prevCombat = g_combatContext.exchange(combatNow, std::memory_order_acq_rel);
    if (prevCombat != combatNow)
    {
        char cb[220] = {};
        sprintf_s(cb, "[COMBATCTX] %s   (pawn %s, camera %s, roamingArea %d, mech %d)",
                  combatNow ? "COMBAT (weapon up)" : "ROAMING (no weapon)",
                  pawnName[0] ? pawnName : "?", modeName[0] ? modeName : "?",
                  roaming ? 1 : 0, mech ? 1 : 0);   // mech 1 = operating a vehicle/mech/turret
        ME2VR::Log::Line(cb);
    }
    // [PAWNSEEN] Log each pawn class the first time it appears, and every change after that. Piloting
    // an Atlas or manning a turret SWAPS the pawn - the camera mode may not change at all - so the pawn
    // name is what identifies them. Both need their own first-person treatment (a mech cockpit and a
    // fixed gun are not Shepard-with-an-eye-offset), and neither can be handled before it has a name.
    // Cover shows up on the camera-mode side instead, which [MODESEEN] below already covers.
    {
        static char s_lastPawnName[96] = {};
        if (pawnName[0] != 0 && std::strcmp(pawnName, s_lastPawnName) != 0)
        {
            strncpy_s(s_lastPawnName, pawnName, sizeof(s_lastPawnName) - 1);
            char b[240] = {};
            sprintf_s(b, "[PAWNSEEN] pawn -> %s   (camera %s, mode %d)", pawnName,
                      modeName[0] ? modeName : "?", ReadGameMode());
            ME2VR::Log::Line(b);
        }
    }
    // [MODESEEN] Log each camera-mode class the first time it appears. The mode census so far came from
    // one short session and showed only Explore/Combat/their storms plus Interpolate - no cover mode and
    // no aim mode, which is why cover currently keeps you in first person: it reports as plain Combat in
    // everything logged so far. Enumerating the full set is what lets cover map to third person by name
    // instead of by guess.
    if (modeName[0] != 0)
    {
        static char s_seen[24][64] = {};
        static int  s_seenCount = 0;
        bool known = false;
        for (int i = 0; i < s_seenCount; ++i) if (std::strcmp(s_seen[i], modeName) == 0) { known = true; break; }
        if (!known && s_seenCount < 24)
        {
            strncpy_s(s_seen[s_seenCount], modeName, sizeof(s_seen[0]) - 1);
            ++s_seenCount;
            ME2VR::Log::Line(std::string("[MODESEEN] new camera mode: ") + modeName);
        }
    }
    // [ME3BLEND] Pick the FP state. SFXCameraMode_Interpolate is the transient BLEND the game makes
    // the ACTIVE mode during any camera change - it is not a camera in its own right. Treating it as
    // "no state" is what made first person drop to third person mid-transition and snap back. Follow
    // the transition TARGET; if its layout is not known yet, HOLD the last real state rather than
    // falling out of first person, which is always the more jarring of the two.
    static int s_heldState = -1;
    int stateIdx;
    // +0x5AC is a ~10ms transition PULSE, not the cover state - it is what made third person appear
    // for a frame and snap back. Require the flag to be held across consecutive frames before acting
    // on it, which the pulse can never satisfy, so nothing flickers while the real state flag is
    // still being hunted. When [COVERSCAN2] names it, only the source below changes.
    // Debounce both edges. ~0.3s to ENTER third person (a 0.14s blip on one byte was observed, and 3
    // frames would have passed it) and ~0.15s to LEAVE, so a single dropped read mid-cover cannot
    // flick the camera. The eye glide covers the delay visually.
    // [COVERHOLD 2026-08-21] One signal in, one signal out: the cover byte (+0xB6D). The crouch byte
    // gates nothing - see the kPawnCover notes for why requiring both meant "cover AND crouched" and
    // left the camera stuck in first person for every upright second of a cover.
    // ARM after 20 consecutive frames (~0.2s), which absorbs the one 0.14s blip this byte has ever
    // been seen to produce, or instantly from a named cover CAMERA, which is proof on its own.
    // LEAVE fast: the byte clearing IS the exit, and the 20-frame lease only absorbs a dropped read.
    // An earlier 90-frame lease left ~0.9s of third person hanging off the end of every cover, which
    // is what made it feel sloppy.
    static int s_coverArm = 0;
    static int s_coverLease = 0;
    static bool s_coverState = false;
    if (coverFlag) { if (s_coverArm < 1000) ++s_coverArm; } else s_coverArm = 0;
    if (!s_coverState && (s_coverArm >= 20 || coverCam)) s_coverState = true;
    if (s_coverState)
    {
        if (coverFlag || coverCam) s_coverLease = 20;
        else if (s_coverLease > 0) --s_coverLease;
        if (s_coverLease <= 0) s_coverState = false;
    }
    else s_coverLease = 0;
    const bool coverSteady = s_coverState;
    {
        static bool s_prevSteady = false;
        if (coverSteady != s_prevSteady)
        {
            s_prevSteady = coverSteady;
            char cb[220] = {};
            sprintf_s(cb, "[COVERHOLD] cover %s   (crouch=%u cover=%u cam=%d, camera %s)",
                      coverSteady ? "ON  -> third person" : "OFF -> first person",
                      static_cast<unsigned>(rawCrouch), static_cast<unsigned>(rawCover),
                      coverCam ? 1 : 0, modeName[0] ? modeName : "?");
            ME2VR::Log::Line(cb);
        }
    }
    // [AIMCOVER 2026-08-21] Publish cover for the head-aim gate. Head aim writes ControlRotation,
    // and in cover the GAME writes ControlRotation too - the lean, the peek, the blind-fire pivot.
    // Two writers on one value every frame is the exact fight [FPSTORM] documented for sprinting;
    // in cover it surfaces as the view swinging off the gaze target.
    // PUBLISH THE DEBOUNCED STATE, NOT THE RAW FLAG. The first cut of this published raw `inCover`
    // from further up, which is precisely the ~10ms PULSE the debounce above exists to absorb - so
    // head aim flapped off and on with every pulse, ramping each time, while the camera (correctly
    // debounced) held still. The mismatch reads as the view jerking on cover entry, which is worse
    // than the bug being fixed. Same source as the camera decision = the two can never disagree.
    g_inCover.store(coverSteady, std::memory_order_release);
    const bool coverTp = coverSteady && g_coverThirdPerson.load(std::memory_order_relaxed);
    if (!fpCam || coverTp) { stateIdx = -1; if (!fpCam) s_heldState = -1; }
    else if (std::strcmp(modeName, "SFXCameraMode_Interpolate") == 0 && mode >= 0x10000)
    {
        const int tgt = InterpTargetState(mode);
        stateIdx = (tgt == -2) ? s_heldState : tgt;
    }
    else
    {
        stateIdx = FpStateIndexForMode(modeName);
        if (std::strncmp(modeName, "SFXCameraMode_", 14) == 0) s_heldState = stateIdx;
    }
    const FpStateCfg* st = (stateIdx >= 0) ? &g_fpStates[stateIdx] : nullptr;
    const bool active = (st != nullptr) && st->on;
    const bool hideHead = (active && st->hideHead) || meshOverride;
    const bool hideBody = active && st->hideBody;

    // Engine UObject writes happen from the Present thread, while ME3 builds the galaxy map on its
    // Draw thread. Two dumps (2026-08-14 and 2026-08-17) caught the identical galaxy renderer crash:
    // MassEffect3.exe+0x4688B5 dereferenced sentinel pointer 1. In both sessions this updater was
    // still touching retained gameplay camera/mesh objects after the map owned presentation. Only
    // mutate the camera in real gameplay and only when the object has the layout FpApplyOffset uses.
    // On the way out, mesh visibility is restored once below; after that menus are strictly read-only.
    const int currentGameMode = ReadGameMode();
    // Preserve Shepard's head during ME3's intentional third-person conversation walk-in.
    const bool convoFpOwnsMesh = ME2VR::ConvoFp::OwnsHeadVisibility();
    const bool fpWriteContext = currentGameMode == 0 &&
                                std::strncmp(modeName, "SFXCameraMode_", 14) == 0;
    {
        static bool s_prevFpWriteContext = true;
        if (fpWriteContext != s_prevFpWriteContext)
        {
            char b[192] = {};
            sprintf_s(b, "[FPWRITE] %s engine writes (gm %d, camera %s)",
                      fpWriteContext ? "resuming gameplay" : "suspending non-gameplay",
                      currentGameMode, modeName[0] ? modeName : "-");
            ME2VR::Log::Line(b);
            s_prevFpWriteContext = fpWriteContext;
        }
    }

    if (stable)
    {
        if (fpWriteContext && mode >= 0x10000)
        {
            // [ME3BLEND2] The cover transition skip, root-caused - it is ME2's [COVSTOMP] bug
            // reintroduced. Two of the mod's writes were fighting the game's own camera blend:
            //   1. the restore path ran EVERY frame while first person was inactive, stomping the
            //      mode's Offset with cached vanilla - and in cover the game drives that offset
            //      itself for lean/peek, so each stomp teleported the camera out and back;
            //   2. during SFXCameraMode_Interpolate the mod wrote into the INTERPOLATE object, whose
            //      offset the game recomputes from From/To as the blend runs, and the mod's first write
            //      poisoned its vanilla cache with a mid-blend value.
            // The engine already interpolates between modes - the glide was double-smoothing on top
            // of a fight. ME2's proven arrangement instead: write the state offset while active,
            // restore ONCE per mode class when inactive (a restore exists to undo THE MOD'S writes, not to
            // run forever), and never touch the Interpolate object - the game blends
            // From.Offset -> To.Offset itself, and the real mode objects already hold what the mod wants.
            static char s_restoredCls[64] = {};
            const bool isInterp = std::strcmp(modeName, "SFXCameraMode_Interpolate") == 0;
            if (isInterp)
            {
                // [ME3BLEND3] The blend reads From.Offset and To.Offset directly - ME2's [ME2BLEND]
                // lesson, and it showed up here exactly as the notes describe: the FIRST sprint went
                // third person for the length of the blend then snapped to first person, the second
                // stayed first person - because the To mode object only received the mod's offset once it
                // became CURRENT. "Hands off during Interpolate" was one step too far: hands off the
                // INTERPOLATE object, yes, but the TO object must be written the moment the blend
                // starts, holding whatever it will hold when it lands.
                std::uintptr_t toObj = 0;
                if (g_interpToOff != 0 && SafeReadPtr(mode + g_interpToOff, &toObj) && toObj >= 0x10000)
                {
                    char toCls[96] = {};
                    if (ClassNameOf(toObj, toCls, sizeof(toCls)) &&
                        std::strncmp(toCls, "SFXCameraMode_", 14) == 0 &&
                        std::strcmp(toCls, "SFXCameraMode_Interpolate") != 0)
                    {
                        const int toIdx = FpStateIndexForMode(toCls);
                        const FpStateCfg* ts = (toIdx >= 0) ? &g_fpStates[toIdx] : nullptr;
                        const bool toActive = fpCam && !coverTp && ts != nullptr && ts->on;
                        if (toActive)
                        {
                            FpApplyOffset(toObj, true, ts->x, ts->y, ts->z, toCls);
                            s_restoredCls[0] = 0;
                        }
                        else if (std::strcmp(s_restoredCls, toCls) != 0)
                        {
                            // [COVEROFFSET] Same rule on the blend's TO object. Taking cover blends
                            // Combat -> Combat with the game recomputing the offset for the lean, so
                            // stamping the mod's cached snapshot on the landing mode is what put the camera
                            // in the wrong place on the way in.
                            const bool ok = coverTp ? FpReleaseToGame(toObj, toCls)
                                                    : FpApplyOffset(toObj, false, 0.0f, 0.0f, 0.0f, toCls);
                            if (ok) strncpy_s(s_restoredCls, toCls, sizeof(s_restoredCls) - 1);
                        }
                    }
                }
            }
            else if (active)
            {
                FpApplyOffset(mode, true, st->x, st->y, st->z, modeName);
                s_restoredCls[0] = 0;
            }
            else if (std::strcmp(s_restoredCls, modeName) != 0)
            {
                // [COVEROFFSET] Cover is the game's camera, not the mod's - hand it back without writing
                // a cached offset over the one it is computing. Everything else restores as before.
                const bool ok = coverTp ? FpReleaseToGame(mode, modeName)
                                        : FpApplyOffset(mode, false, 0.0f, 0.0f, 0.0f, modeName);
                if (ok)
                {
                    strncpy_s(s_restoredCls, modeName, sizeof(s_restoredCls) - 1);
                    char rb[160] = {};
                    sprintf_s(rb, "[COVEROFFSET] %s on %s", coverTp ? "released to game (cover)" : "restored vanilla", modeName);
                    ME2VR::Log::Line(rb);
                }
            }
        }
        static bool s_prevHead = false, s_prevBody = false;
        static std::uintptr_t s_hidPawn = 0;
        // Re-assert on a timer, not only when the value CHANGES. The cache alone is why the head
        // could stay hidden after leaving cover: if a call lands while the game is rebuilding the
        // pawn's meshes it quietly does nothing, but the cache already says "applied" so it is
        // never retried and the head stays gone until something else happens to change state.
        static int s_reassert = 0;
        if (fpWriteContext) ++s_reassert; else s_reassert = 0;
        const bool due = fpWriteContext && s_reassert >= 30;
        if (due) s_reassert = 0;
        const bool wantedHead = convoFpOwnsMesh ? ME2VR::ConvoFp::GetHideHead() : (fpWriteContext ? hideHead : false);
        const bool wantedBody = convoFpOwnsMesh ? false : (fpWriteContext ? hideBody : false);
        if (due || pawn != s_hidPawn || wantedHead != s_prevHead || wantedBody != s_prevBody)
        {
            FpHideMeshes(pawn, wantedHead, wantedBody);
            s_prevHead = wantedHead; s_prevBody = wantedBody; s_hidPawn = pawn;
        }
    }

    static int s_prevState = -2;
    if (stateIdx != s_prevState)
    {
        char b[208] = {};
        sprintf_s(b, "[ME3FP] state=%s mode=%s eye=(%.0f,%.0f,%.0f) hideHead=%d hideBody=%d",
                  (stateIdx >= 0) ? g_fpDefs[stateIdx].label : "(third person)", modeName,
                  active ? st->x : 0.0f, active ? st->y : 0.0f, active ? st->z : 0.0f, hideHead ? 1 : 0, hideBody ? 1 : 0);
        ME2VR::Log::Line(b);
        s_prevState = stateIdx;
    }
}

int FpStateCount() noexcept { return kFpCount; }
FpStateCfg* GetFpStateCfg(int id) noexcept { FpEnsureInit(); return (id >= 0 && id < kFpCount) ? &g_fpStates[id] : nullptr; }
const char* FpStateLabel(int id) noexcept { return (id >= 0 && id < kFpCount) ? g_fpDefs[id].label : ""; }
FpStateCfg FpStateDefault(int id) noexcept { return (id >= 0 && id < kFpCount) ? g_fpDefs[id].def : FpStateCfg{}; }
void SetFirstPerson(bool on) noexcept { g_fpEnabled.store(on, std::memory_order_release); }
bool GetFirstPerson() noexcept { return g_fpEnabled.load(std::memory_order_acquire); }
void SetMeshHide(bool on) noexcept { g_meshHidden.store(on, std::memory_order_release); }
bool GetMeshHide() noexcept { return g_meshHidden.load(std::memory_order_acquire); }

int ConvoFpSetHeadHidden(std::uintptr_t actor, bool hide) noexcept
{
    if (actor < 0x10000 || ActorIsDying(actor)) return 0;
    int count = 0;
    __try
    {
        const std::uintptr_t offsets[] = { 0xB38, 0xB48, 0xB50, 0xB58, 0xB60 };
        for (const auto off : offsets)
        {
            const std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(actor + off);
            if (comp < 0x10000) continue;
            CallPrimSetHidden(comp, hide); FpSetOwnerNoSee(comp, hide); ++count;
        }
        const std::uintptr_t data = *reinterpret_cast<std::uintptr_t volatile*>(actor + 0x6FC);
        const int num = *reinterpret_cast<int volatile*>(actor + 0x6FC + 8);
        if (data >= 0x10000 && num > 0 && num <= 32)
            for (int i = 0; i < num; ++i)
            {
                const std::uintptr_t comp = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
                if (comp < 0x10000) continue;
                CallPrimSetHidden(comp, hide); FpSetOwnerNoSee(comp, hide); ++count;
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return count;
}

bool ConvoFpReadStageDofActive(std::uintptr_t stage, bool* outActive) noexcept
{
    if (stage < 0x10000 || !outActive) return false;
    __try { *outActive = (*reinterpret_cast<std::uint32_t volatile*>(stage + 0x2EC) & 0x08u) != 0; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ConvoFpSetStageDofActive(std::uintptr_t stage, bool active) noexcept
{
    if (stage < 0x10000) return false;
    __try
    {
        auto* bits = reinterpret_cast<std::uint32_t volatile*>(stage + 0x2EC);
        const std::uint32_t old = *bits;
        *bits = active ? (old | 0x08u) : (old & ~0x08u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ConvoFpDisableDof() noexcept
{
    FindConvoFunctions();
    if (!g_disableDofFn) return false;
    const std::uintptr_t lp = GetPrimaryLocalPlayer();
    std::uintptr_t pc = 0;
    if (lp < 0x10000 || !SafeReadPtr(lp + 0x68, &pc) || pc < 0x10000) return false;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        auto pe = reinterpret_cast<tProcessEvent>(base + 0xF4F80);
        pe(reinterpret_cast<void*>(pc), g_disableDofFn, nullptr, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
