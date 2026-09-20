#pragma once

#include <cstdint>

namespace ME2VR::ConvoFp
{
struct EyePose
{
    float x, y, z;
    float baseYawDeg;
};

void Tick() noexcept;
bool IsArmed() noexcept;
bool IsConversationActive() noexcept;
bool OwnsCamera() noexcept;          // true only while the actual conversation camera owns this shot
bool OwnsHeadVisibility() noexcept;  // same ownership boundary as camera relocation
bool GetEyePose(EyePose* out) noexcept;

void SetEnabled(bool on) noexcept;             bool GetEnabled() noexcept;
void SetTurnRate(float degPerSec) noexcept;    float GetTurnRate() noexcept;
void SetAnimFollow(float f) noexcept;          float GetAnimFollow() noexcept;
void SetEyeUpUU(float uu) noexcept;            float GetEyeUpUU() noexcept;
void SetEyeFwdUU(float uu) noexcept;           float GetEyeFwdUU() noexcept;
void SetZoom(float z) noexcept;                float GetZoom() noexcept;
void SetHideHead(bool on) noexcept;            bool GetHideHead() noexcept;
void SetKillDof(bool on) noexcept;             bool GetKillDof() noexcept;

struct Diag
{
    unsigned long long conv;
    unsigned long long stagedPlayer;
    unsigned long long speaker;
    unsigned long long faceTarget;
    int source;
    int speakerCount;
    float x, y, z, baseYawDeg;
};
void GetDiag(Diag* out) noexcept;
}
