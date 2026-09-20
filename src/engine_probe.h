#pragma once

#include <cstdint>

namespace ME2VR::EngineProbe
{
// Read-only discovery: resolve GEngine at the community SDK's LE2 RVA, walk to the
// primary ULocalPlayer, confirm the (LE1-identical) ULocalPlayer field offsets read sane
// live, and dump the FViewportClient::Draw vtable slot. Self-latches after one success.
// Call freely from the Present hook; it no-ops once it has dumped or until the engine is up.
void TryDumpOnce() noexcept;

// Live re-resolve of the primary (P1) ULocalPlayer via GEngine->GamePlayers[0].
// Returns 0 until the engine + a local player exist. SEH-guarded.
std::uintptr_t GetPrimaryLocalPlayer() noexcept;
// LE3 discovery probes (read-only, Diagnostics=1 only). See engine_probe.cpp for what they hunt.
void AimProbeTick() noexcept;
// [HEADAIM] HMD -> aim injection (LE3 rotator at 0x114, discovered by AimProbe). See the .cpp.
void DriveHeadAim(float headYawDeg, float headPitchDeg, bool allowAim) noexcept;
void SetHeadAimEnabled(bool on) noexcept;    bool GetHeadAimEnabled() noexcept;
int  ReadGameMode() noexcept;         // EGameModes; -1 = unreadable, callers must fail OPEN to VR
const char* GameModeName(int m) noexcept;
bool IsHeadAimActive() noexcept;
void GetAimSeedRem(int* yawUU, int* pitchUU) noexcept;
void SetHeadAimInvertYaw(bool on) noexcept;  bool GetHeadAimInvertYaw() noexcept;
void SetHeadAimInvertPitch(bool on) noexcept; bool GetHeadAimInvertPitch() noexcept;
// [AIMSCOPE] restrict head aim to weapon-drawn only. OFF by default: aim drives ControlRotation,
// which is also what LE3 uses to decide what the player is facing for world interactions.
void SetHeadAimWeaponOnly(bool on) noexcept; bool GetHeadAimWeaponOnly() noexcept;
// [AIMCOVER] ON = head aim stands down in cover (the fix). OFF = pre-fix behaviour, for A/B testing.
void SetHeadAimCoverOff(bool on) noexcept; bool GetHeadAimCoverOff() noexcept;
void ModeProbeTick() noexcept;
void CamProbeTick() noexcept;   // [CAMPROBE] camera-mode chain discovery
void GuiProbeStart() noexcept;  // [GUICEN] GFx/GUI object census thread (menu-name discovery)
bool GetNamedGuiActive() noexcept; // [GUINAME] a named 3D-model menu (squad/bench/outfit) is open
void TakeCensusStats(unsigned* fastMaxUs, unsigned* slowMaxUs, int* objects) noexcept;   // [FRAMETIME]
// True when LE3's PlayerController owns both movement and look input away from gameplay.
// These are verified SDK fields (bIgnoreMoveInput@0x781 / bIgnoreLookInput@0x782), and unlike
// per-screen UObjects they remain asserted while the user moves between menu pages.
bool IsMenuInputOwned(unsigned char* moveValue = nullptr, unsigned char* lookValue = nullptr) noexcept;


// True when the game is paused (AWorldInfo.Pauser != null) - i.e. a full-screen blocking menu
// (journal/squad/pause) is up. Gameplay and conversations are NOT paused. Reliable, noise-free.
bool IsGamePaused() noexcept;

// Diagnostic: fill out[0..4] = {P1, PlayerController, WorldInfo, Pauser, bPlayersOnly} for logging.
void GetPauseChain(unsigned long long out[5]) noexcept;

// Discovery: dump the GFx UI object graph (class names) reachable from P1, to find the signal that a
// full-screen screen (galaxy map / journal / squad) is open. Read-only; trigger on a hotkey.
void DumpGfxState() noexcept;

// True while the galaxy map is open (PlayerCamera current mode = BioCameraBehaviorGalaxy). Used to
// auto-flatten (mono) the galaxy map. Conversations/gameplay use other camera modes -> stay stereo.
bool IsGalaxyMapOpen() noexcept;

// First-person (ported from ME2). Per-SFXCameraMode-state eye offset + head/body hide; the active state
// is chosen by the live camera-mode class name. Call ApplyFirstPerson every frame from the present hook.
void ApplyFirstPerson() noexcept;
bool IsConvoCamera() noexcept;         // [VRCINE] live camera is a conversation camera
int  ConvoFpSetHeadHidden(std::uintptr_t actor, bool hide) noexcept;
bool ConvoFpDisableDof() noexcept;
bool ConvoFpReadStageDofActive(std::uintptr_t stage, bool* outActive) noexcept;
bool ConvoFpSetStageDofActive(std::uintptr_t stage, bool active) noexcept;

// [CAMFOV] LE3 camera FOV field(s), discovered at RUNTIME: the camera object is scanned during
// stable wide gameplay for floats equal to the live FOV; offsets that hold across ~90 frames are
// locked. Ported from ME2's FILLSRC (glass/refraction smear fix), where the offset was hardcoded
// (cam+0x47C) - LE3's camera layout differs, so it is measured instead of assumed.
bool CamFovReady() noexcept;                       // offsets locked, Set/Get usable
bool GetCameraFovDeg(float* outDeg) noexcept;      // current camera FOV (degrees, horizontal)
bool SetCameraFovDeg(float deg, float* outPrev) noexcept;   // write ALL locked offsets; prev of first
bool IsGameplayCamera() noexcept;      // camera is SFXCameraMode_* (gameplay incl. ADS)
bool IsWeaponOut() noexcept;           // [HEADAIM] Combat-family camera mode (holds through blends)
bool IsPhotoCamera() noexcept;         // [PHOTOVR] SFXCameraMode_PhotoFree: render it as gameplay stereo
// [COMBATCTX] LE3 forbids drawing weapons in hub areas and says so by swapping the player pawn to a
// *NonCombat class. IsRoamingArea() is that area verdict; IsCombatContext() is the full answer
// (combat-capable area AND weapon actually up). Both hold their value through camera blends.
bool IsRoamingArea() noexcept;
bool IsCombatContext() noexcept;
bool GetCoverThirdPerson() noexcept;   // [COVER] third person while in cover
void SetCoverThirdPerson(bool on) noexcept;
// [AIMCOVER] live cover state. Head aim must stand down here: the game drives ControlRotation for
// the cover lean/peek and two writers fight, the same failure [FPSTORM] fixed for sprinting.
bool IsInCover() noexcept;
void SetFirstPerson(bool on) noexcept;   // F6
bool GetFirstPerson() noexcept;
void SetMeshHide(bool on) noexcept;      // F7: head-hide test, independent of FP camera
bool GetMeshHide() noexcept;

// Per-state config (eye offset X=fwd, Y=right, Z=up). Tunable from the First Person menu tab.
struct FpStateCfg { bool on; bool hideHead; bool hideBody; bool hideWeapon; float x; float y; float z; };
int FpStateCount() noexcept;
FpStateCfg* GetFpStateCfg(int id) noexcept;
const char* FpStateLabel(int id) noexcept;
FpStateCfg FpStateDefault(int id) noexcept;
}
