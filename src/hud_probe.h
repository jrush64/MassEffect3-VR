#pragma once

namespace ME2VR::HudProbe
{
struct GroupConfig
{
    float offsetX;
    float offsetY;
    float scaleX;
    float scaleY;
};

void Tick() noexcept;
// Guarded ME3-native per-element HUD editor. Config is copied across threads;
// all Scaleform reads/writes happen on the game's ProcessEvent thread.
void SetEditorEnabled(bool enabled) noexcept;
bool GetEditorEnabled() noexcept;
int GroupCount() noexcept;
const char* GroupLabel(int group) noexcept;
GroupConfig GetGroupConfig(int group) noexcept;
GroupConfig DefaultGroupConfig(int group) noexcept;
void SetGroupConfig(int group, GroupConfig config) noexcept;
void ResetGroup(int group) noexcept;
void ResetAll() noexcept;
}
