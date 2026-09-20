#include "hud_probe.h"

#include "logger.h"

#include "MinHook.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ME2VR::HudProbe
{
namespace
{
constexpr std::uintptr_t kGObjectsRva = 0x1887E40;
constexpr std::uintptr_t kNamePoolsRva = 0x17B33D0;
constexpr std::uintptr_t kProcessEventRva = 0xF4F80;
constexpr std::uintptr_t kCallFunctionRva = 0xF2C30;
constexpr std::uintptr_t kObjOuter = 0x40;
constexpr std::uintptr_t kObjName = 0x48;
constexpr std::uintptr_t kObjClass = 0x50;

using tProcessEvent = void(__fastcall*)(void*, void*, void*, void*);
using tCallFunction = void(__fastcall*)(void*, void*, void*, void*);

struct FStringParam { const wchar_t* data; int count; int capacity; };
struct GetVariableObjectParams
{
    FStringParam path;
    void* type;
    void* result;
};
struct GetVariableNumberParams
{
    FStringParam path;
    float result;
};
struct SetVariableNumberParams
{
    FStringParam path;
    float value;
    unsigned char result;
};

void* g_original = nullptr;
void* g_originalCallFunction = nullptr;
void* g_getVariableObjectFn = nullptr;
void* g_actionScriptObjectMovieFn = nullptr;
void* g_actionScriptObjectValueFn = nullptr;
void* g_getObjectValueFn = nullptr;
void* g_getVariableNumberFn = nullptr;
void* g_setVariableNumberFn = nullptr;
std::uintptr_t g_hudClass = 0;
std::atomic_bool g_dumpRequested{false};
std::atomic_bool g_autoDumpPending{false};
std::atomic_ullong g_lastMappingChangeMs{0};
std::atomic_ullong g_lastAutoDumpMs{0};
std::atomic_uintptr_t g_lastHud{0};
thread_local bool g_reading = false;

constexpr int kGroupCount = 7;
struct GroupDefinition
{
    const char* label;
    const wchar_t* paths[6];
};
const GroupDefinition kGroups[kGroupCount] = {
    {"Weapon + ammo", {L"Weapon", L"SpareAmmo", L"MagazineAmmo", L"WeaponGrenades", L"WeaponOverheat", nullptr}},
    {"Target info", {L"TargetBackground", L"TargetName", L"TargetStatus", L"ButtonA", L"resistanceBar", nullptr}},
    {"Health + squad", {L"Player", L"Team.Team1", L"Team.Team2", nullptr}},
    {"Powers", {L"PlayerPower1", L"PlayerPower2", nullptr}},
    {"Notifications", {L"mcNotification", nullptr}},
    {"Centre status", {L"CenterStatus", L"AmmoFull", nullptr}},
    {"Action / POI prompts", {L"ActionIcon", L"POI", nullptr}},
};

std::atomic_bool g_editorEnabled{true};
const GroupConfig kBakedDefaults[kGroupCount] = {
    {151.3f, 5.9f, 1.245f, 1.246f}, // Weapon + ammo
    {0, 0, 1, 1},                   // Target info
    {0, 0, 1.008f, 1},              // Health + squad
    {0, 0, 1, 1},                   // Powers
    {-137.2f, 0, 1, 1},             // Notifications
    {0, 0, 1, 1},                   // Centre status
    {0, 0, 1, 1},                   // Action / POI prompts
};
GroupConfig g_groupConfig[kGroupCount] = {
    kBakedDefaults[0], kBakedDefaults[1], kBakedDefaults[2], kBakedDefaults[3],
    kBakedDefaults[4], kBakedDefaults[5], kBakedDefaults[6],
};
SRWLOCK g_configLock = SRWLOCK_INIT;

struct PathState
{
    int group;
    const wchar_t* path;
    float x, y, scaleX, scaleY;
    bool valid;
    bool dirty;
};
PathState g_pathState[] = {
    {0, L"Weapon"}, {0, L"SpareAmmo"}, {0, L"MagazineAmmo"},
    {0, L"WeaponGrenades"}, {0, L"WeaponOverheat"},
    {1, L"TargetBackground"}, {1, L"TargetName"}, {1, L"TargetStatus"},
    {1, L"ButtonA"}, {1, L"resistanceBar"},
    {2, L"Player"}, {2, L"Team.Team1"}, {2, L"Team.Team2"},
    {3, L"PlayerPower1"}, {3, L"PlayerPower2"},
    {4, L"mcNotification"},
    {5, L"CenterStatus"}, {5, L"AmmoFull"},
    {6, L"ActionIcon"}, {6, L"POI"},
};
void* g_editorHud = nullptr;
void* g_editorMovie = nullptr;
unsigned long long g_lastEditorApplyMs = 0;

float SafeClamp(float value, float lo, float hi, float fallback) noexcept
{
    if (!std::isfinite(value)) return fallback;
    return value < lo ? lo : (value > hi ? hi : value);
}

bool IsNeutral(const GroupConfig& c) noexcept
{
    return std::fabs(c.offsetX) < 0.001f && std::fabs(c.offsetY) < 0.001f &&
           std::fabs(c.scaleX - 1.0f) < 0.0001f && std::fabs(c.scaleY - 1.0f) < 0.0001f;
}

void ClearEditorBaselines() noexcept
{
    g_editorMovie = nullptr;
    g_lastEditorApplyMs = 0;
    for (PathState& state : g_pathState)
    {
        state.valid = false;
        state.dirty = false;
        state.x = state.y = 0.0f;
        state.scaleX = state.scaleY = 0.0f;
    }
}

bool ReadName(unsigned long long packed, char* out, size_t cap) noexcept
{
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const unsigned p32 = static_cast<unsigned>(packed & 0xFFFFFFFFull);
        const unsigned offset = p32 & 0x1FFFFFFFu;
        const unsigned chunk = (p32 >> 29) & 7u;
        auto* pools = reinterpret_cast<unsigned char* volatile*>(base + kNamePoolsRva);
        unsigned char* pool = pools[chunk];
        if (pool == nullptr) return false;
        const char* ansi = reinterpret_cast<const char*>(pool + offset + 12);
        size_t i = 0;
        for (; i + 1 < cap && ansi[i] >= 32 && ansi[i] < 127; ++i) out[i] = ansi[i];
        out[i] = 0;
        return i != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ObjName(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    __try
    {
        if (obj < 0x10000) return false;
        return ReadName(*reinterpret_cast<unsigned long long volatile*>(obj + kObjName), out, cap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ClassNameOf(std::uintptr_t obj, char* out, size_t cap) noexcept
{
    __try
    {
        if (obj < 0x10000) return false;
        const auto cls = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjClass);
        if (cls < 0x10000) return false;
        return ReadName(*reinterpret_cast<unsigned long long volatile*>(cls + kObjName), out, cap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* FindFunction(const char* nameWanted, const char* outerWanted) noexcept
{
    void* found = nullptr;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjectsRva);
        const int count = *reinterpret_cast<int volatile*>(base + kGObjectsRva + 8);
        if (data < 0x10000 || count <= 0 || count > 5000000) return nullptr;
        char name[64] = {}, outer[64] = {};
        for (int i = 0; i < count; ++i)
        {
            const auto obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (!ObjName(obj, name, sizeof(name)) || std::strcmp(name, nameWanted) != 0) continue;
            const auto owner = *reinterpret_cast<std::uintptr_t volatile*>(obj + kObjOuter);
            if (ObjName(owner, outer, sizeof(outer)) && std::strcmp(outer, outerWanted) == 0)
            {
                found = reinterpret_cast<void*>(obj);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

std::uintptr_t FindClass(const char* wanted) noexcept
{
    std::uintptr_t found = 0;
    __try
    {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto data = *reinterpret_cast<std::uintptr_t volatile*>(base + kGObjectsRva);
        const int count = *reinterpret_cast<int volatile*>(base + kGObjectsRva + 8);
        if (data < 0x10000 || count <= 0 || count > 5000000) return 0;
        char name[64] = {}, kind[64] = {};
        for (int i = 0; i < count; ++i)
        {
            const auto obj = *reinterpret_cast<std::uintptr_t volatile*>(data + static_cast<std::uintptr_t>(i) * 8);
            if (!ObjName(obj, name, sizeof(name)) || std::strcmp(name, wanted) != 0) continue;
            if (ClassNameOf(obj, kind, sizeof(kind)) && std::strcmp(kind, "Class") == 0)
            {
                found = obj;
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

struct Mapping
{
    void* value;
    void* movie;
    wchar_t path[192];
    unsigned serial;
};
Mapping g_mappings[4096] = {};
unsigned g_mappingCount = 0;
unsigned g_serial = 0;
SRWLOCK g_mappingLock = SRWLOCK_INIT;

void CaptureMapping(void* movie, const GetVariableObjectParams* params) noexcept
{
    if (params == nullptr || params->result == nullptr || params->path.data == nullptr ||
        params->path.count <= 1 || params->path.count > 512) return;
    wchar_t path[192] = {};
    size_t i = 0;
    __try
    {
        for (; i + 1 < 192 && i < static_cast<size_t>(params->path.count) && params->path.data[i] != 0; ++i)
            path[i] = params->path.data[i];
        path[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (i == 0) return;

    AcquireSRWLockExclusive(&g_mappingLock);
    unsigned slot = 4096;
    for (unsigned n = 0; n < g_mappingCount; ++n)
        if (g_mappings[n].value == params->result) { slot = n; break; }
    if (slot == 4096)
    {
        slot = (g_mappingCount < 4096) ? g_mappingCount++ : (g_serial % 4096);
    }
    Mapping& m = g_mappings[slot];
    const bool changed = m.value != params->result || m.movie != movie || wcscmp(m.path, path) != 0;
    if (!changed)
    {
        ReleaseSRWLockExclusive(&g_mappingLock);
        return;
    }
    m.value = params->result;
    m.movie = movie;
    wcscpy_s(m.path, path);
    m.serial = ++g_serial;
    ReleaseSRWLockExclusive(&g_mappingLock);

    // Let a burst of GetVariableObject calls settle before dumping. A rebuilt HUD normally
    // resolves many children in rapid succession, so this produces one useful snapshot
    // instead of hundreds of partial ones.
    g_lastMappingChangeMs.store(GetTickCount64(), std::memory_order_release);
}

bool ReadFString(std::uintptr_t address, wchar_t* out, size_t cap) noexcept
{
    __try
    {
        const wchar_t* data = *reinterpret_cast<wchar_t* volatile*>(address);
        const int count = *reinterpret_cast<int volatile*>(address + 8);
        if (data == nullptr || count <= 1 || count > 512) return false;
        size_t i = 0;
        for (; i + 1 < cap && i < static_cast<size_t>(count) && data[i] != 0; ++i) out[i] = data[i];
        out[i] = 0;
        return i != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadNumber(void* movie, const wchar_t* path, float* out) noexcept
{
    if (movie == nullptr || path == nullptr || out == nullptr || g_getVariableNumberFn == nullptr) return false;
    __try
    {
        GetVariableNumberParams p = {};
        p.path.data = path;
        p.path.count = static_cast<int>(wcslen(path)) + 1;
        p.path.capacity = p.path.count;
        g_reading = true;
        reinterpret_cast<tProcessEvent>(g_original)(movie, g_getVariableNumberFn, &p, nullptr);
        g_reading = false;
        *out = p.result;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_reading = false; return false; }
}

bool WriteNumber(void* movie, const wchar_t* path, float value) noexcept
{
    if (movie == nullptr || path == nullptr || g_setVariableNumberFn == nullptr || !std::isfinite(value)) return false;
    __try
    {
        SetVariableNumberParams p = {};
        p.path.data = path;
        p.path.count = static_cast<int>(wcslen(path)) + 1;
        p.path.capacity = p.path.count;
        p.value = value;
        g_reading = true;
        reinterpret_cast<tProcessEvent>(g_original)(movie, g_setVariableNumberFn, &p, nullptr);
        g_reading = false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_reading = false; return false; }
}

bool ReadDisplayInfo(void* movie, PathState& state) noexcept
{
    wchar_t property[224] = {};
    float x = 0, y = 0, sx = 0, sy = 0;
    swprintf_s(property, L"%s._x", state.path); if (!ReadNumber(movie, property, &x)) return false;
    swprintf_s(property, L"%s._y", state.path); if (!ReadNumber(movie, property, &y)) return false;
    swprintf_s(property, L"%s._xscale", state.path); if (!ReadNumber(movie, property, &sx)) return false;
    swprintf_s(property, L"%s._yscale", state.path); if (!ReadNumber(movie, property, &sy)) return false;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(sx) || !std::isfinite(sy) ||
        std::fabs(sx) < 0.01f || std::fabs(sy) < 0.01f || std::fabs(sx) > 1000.0f || std::fabs(sy) > 1000.0f)
        return false;
    state.x = x; state.y = y; state.scaleX = sx; state.scaleY = sy; state.valid = true;
    return true;
}

void WriteDisplayInfo(void* movie, const PathState& state, float x, float y, float sx, float sy) noexcept
{
    wchar_t property[224] = {};
    swprintf_s(property, L"%s._x", state.path); WriteNumber(movie, property, SafeClamp(x, -400.0f, 1680.0f, state.x));
    swprintf_s(property, L"%s._y", state.path); WriteNumber(movie, property, SafeClamp(y, -300.0f, 1020.0f, state.y));
    swprintf_s(property, L"%s._xscale", state.path); WriteNumber(movie, property, SafeClamp(sx, 5.0f, 600.0f, state.scaleX));
    swprintf_s(property, L"%s._yscale", state.path); WriteNumber(movie, property, SafeClamp(sy, 5.0f, 600.0f, state.scaleY));
}

struct Field { std::uintptr_t offset; const char* label; };
const Field kFields[] = {
    {0x06EC, "Notification"},
    {0x0720, "WeaponIcon"}, {0x0728, "WeaponAmmo"}, {0x0730, "WeaponClip"}, {0x0738, "GrenadeAmmo"},
    {0x0740, "TargetName"}, {0x0748, "TargetStatus"}, {0x0750, "ButtonA"}, {0x0758, "TargetBackground"},
    {0x0760, "HealthBar"}, {0x0768, "ArmourBar"}, {0x0770, "BioticBar"}, {0x0778, "ShieldBar"},
    {0x0780, "ResistanceText"}, {0x0788, "ResistanceBar"},
    {0x0790, "CenterStatus"}, {0x0798, "CenterStatusText"},
    {0x07A0, "OverheatIndicator"}, {0x07A8, "OverheatTextAnim"}, {0x07B0, "OverheatText"},
    {0x07B8, "PlayerPowerLeft"}, {0x07C0, "PlayerPowerRight"}, {0x07C8, "AmmoFull"},
    {0x07D0, "ActionIcon"}, {0x07D8, "POI"},
};

bool FieldValue(void* hud, std::uintptr_t offset, void** out) noexcept
{
    __try
    {
        *out = *reinterpret_cast<void* volatile*>(reinterpret_cast<std::uintptr_t>(hud) + offset);
        return reinterpret_cast<std::uintptr_t>(*out) >= 0x10000;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool FindMapping(void* value, Mapping* out) noexcept
{
    bool found = false;
    AcquireSRWLockShared(&g_mappingLock);
    for (unsigned i = 0; i < g_mappingCount; ++i)
    {
        if (g_mappings[i].value != value) continue;
        *out = g_mappings[i];
        found = true;
        break;
    }
    ReleaseSRWLockShared(&g_mappingLock);
    return found;
}

void* FindHudMovie(void* hud) noexcept
{
    for (const Field& field : kFields)
    {
        void* value = nullptr;
        Mapping mapping = {};
        if (!FieldValue(hud, field.offset, &value) || !FindMapping(value, &mapping) || mapping.movie == nullptr) continue;
        char ownerClass[64] = {};
        if (ClassNameOf(reinterpret_cast<std::uintptr_t>(mapping.movie), ownerClass, sizeof(ownerClass)) &&
            std::strcmp(ownerClass, "GFxValue") != 0)
            return mapping.movie;
    }
    return nullptr;
}

void ApplyEditor(void* hud, unsigned long long now) noexcept
{
    if (hud == nullptr || g_getVariableNumberFn == nullptr || g_setVariableNumberFn == nullptr) return;
    if (g_editorHud != hud)
    {
        g_editorHud = hud;
        ClearEditorBaselines();
    }
    // Avoid capturing construction/transition coordinates as immutable baselines.
    const unsigned long long settledAt = g_lastMappingChangeMs.load(std::memory_order_acquire) + 1500;
    if (now < settledAt || now < g_lastEditorApplyMs + 33) return;

    GroupConfig cfg[kGroupCount] = {};
    AcquireSRWLockShared(&g_configLock);
    for (int i = 0; i < kGroupCount; ++i) cfg[i] = g_groupConfig[i];
    ReleaseSRWLockShared(&g_configLock);
    const bool enabled = g_editorEnabled.load(std::memory_order_acquire);
    bool needMovie = false;
    for (const PathState& state : g_pathState)
        if ((enabled && !IsNeutral(cfg[state.group])) || state.dirty) { needMovie = true; break; }
    if (!needMovie) return;
    if (g_editorMovie == nullptr) g_editorMovie = FindHudMovie(hud);
    if (g_editorMovie == nullptr) return;
    g_lastEditorApplyMs = now;

    for (int group = 0; group < kGroupCount; ++group)
    {
        const bool active = enabled && !IsNeutral(cfg[group]);
        if (active)
            for (PathState& state : g_pathState)
                if (state.group == group && !state.valid) ReadDisplayInfo(g_editorMovie, state);

        float centerX = 0.0f, centerY = 0.0f;
        int count = 0;
        for (const PathState& state : g_pathState)
            if (state.group == group && state.valid) { centerX += state.x; centerY += state.y; ++count; }
        if (count != 0) { centerX /= count; centerY /= count; }

        for (PathState& state : g_pathState)
        {
            if (state.group != group || !state.valid) continue;
            if (active)
            {
                const float x = centerX + (state.x - centerX) * cfg[group].scaleX + cfg[group].offsetX;
                const float y = centerY + (state.y - centerY) * cfg[group].scaleY + cfg[group].offsetY;
                WriteDisplayInfo(g_editorMovie, state, x, y,
                                 state.scaleX * cfg[group].scaleX, state.scaleY * cfg[group].scaleY);
                state.dirty = true;
            }
            else if (state.dirty)
            {
                WriteDisplayInfo(g_editorMovie, state, state.x, state.y, state.scaleX, state.scaleY);
                state.dirty = false;
            }
        }
    }
}

void LogChain(const Mapping& mapping) noexcept
{
    if (mapping.movie == nullptr || mapping.path[0] == 0) return;
    const size_t len = wcslen(mapping.path);
    wchar_t prefix[192] = {};
    for (size_t cut = 0; cut <= len; ++cut)
    {
        if (cut != len && mapping.path[cut] != L'.') continue;
        if (cut == 0 || cut >= 192) continue;
        wmemcpy(prefix, mapping.path, cut);
        prefix[cut] = 0;
        wchar_t property[224] = {};
        float x = 0.0f, y = 0.0f, sx = 0.0f, sy = 0.0f, alpha = 0.0f, visible = 0.0f;
        swprintf_s(property, L"%s._x", prefix); ReadNumber(mapping.movie, property, &x);
        swprintf_s(property, L"%s._y", prefix); ReadNumber(mapping.movie, property, &y);
        swprintf_s(property, L"%s._xscale", prefix); ReadNumber(mapping.movie, property, &sx);
        swprintf_s(property, L"%s._yscale", prefix); ReadNumber(mapping.movie, property, &sy);
        swprintf_s(property, L"%s._alpha", prefix); ReadNumber(mapping.movie, property, &alpha);
        swprintf_s(property, L"%s._visible", prefix); ReadNumber(mapping.movie, property, &visible);
        char line[448] = {};
        sprintf_s(line, "[ME3HUDPATH]     '%ls' x=%.1f y=%.1f xs=%.1f ys=%.1f alpha=%.1f visible=%.0f",
                  prefix, x, y, sx, sy, alpha, visible);
        ME2VR::Log::Line(line);
    }
}

void DumpHud(void* hud) noexcept
{
    char header[192] = {};
    sprintf_s(header, "[ME3HUDPATH] === read-only HUD ownership dump; captured mappings=%u ===", g_mappingCount);
    ME2VR::Log::Line(header);
    int mapped = 0;
    for (const Field& field : kFields)
    {
        void* value = nullptr;
        Mapping mapping = {};
        char line[448] = {};
        if (FieldValue(hud, field.offset, &value) && FindMapping(value, &mapping))
        {
            ++mapped;
            char movieClass[64] = {};
            ClassNameOf(reinterpret_cast<std::uintptr_t>(mapping.movie), movieClass, sizeof(movieClass));
            sprintf_s(line, "[ME3HUDPATH] %-22s value=%p movie=%p(%s) path='%ls'",
                      field.label, value, mapping.movie, movieClass, mapping.path);
            ME2VR::Log::Line(line);
            // Value.ActionScriptObject/GetObject paths are relative to another GFxValue.
            // They are still useful ownership evidence, but only movie contexts can service
            // the read-only GetVariableNumber ancestor query.
            if (std::strcmp(movieClass, "GFxValue") != 0) LogChain(mapping);
        }
        else
        {
            sprintf_s(line, "[ME3HUDPATH] %-22s value=%p path=UNMAPPED", field.label, value);
            ME2VR::Log::Line(line);
        }
    }
    const std::uintptr_t squadOffsets[3] = {0x0334, 0x03F4, 0x04B4};
    const char* squadLabels[3] = {"ShepardRoot", "Hench1Root", "Hench2Root"};
    for (int i = 0; i < 3; ++i)
    {
        wchar_t path[192] = {};
        char line[320] = {};
        if (ReadFString(reinterpret_cast<std::uintptr_t>(hud) + squadOffsets[i], path, 192))
            sprintf_s(line, "[ME3HUDPATH] %-22s SDK path='%ls'", squadLabels[i], path);
        else
            sprintf_s(line, "[ME3HUDPATH] %-22s SDK path=unavailable", squadLabels[i]);
        ME2VR::Log::Line(line);
    }
    sprintf_s(header, "[ME3HUDPATH] === dump complete: %d/%zu direct fields mapped; reload gameplay HUD if unmapped ===",
              mapped, sizeof(kFields) / sizeof(kFields[0]));
    ME2VR::Log::Line(header);
}

bool IsExactClass(void* context, std::uintptr_t expectedClass) noexcept
{
    __try
    {
        const auto address = reinterpret_cast<std::uintptr_t>(context);
        return address >= 0x10000 &&
               *reinterpret_cast<std::uintptr_t volatile*>(address + kObjClass) == expectedClass;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool DecodeLiteralPath(void* stack, wchar_t* out, size_t cap) noexcept
{
    if (stack == nullptr || out == nullptr || cap < 2) return false;
    __try
    {
        // LE3 FFrame is packed to 4 bytes; Code is at +0x28. CallFunction receives
        // the frame positioned at the first argument expression. HUD construction uses
        // literal ActionScript paths, so decoding those two literal opcodes avoids
        // evaluating bytecode a second time or causing any game-side effects.
        const auto code = *reinterpret_cast<unsigned char* volatile*>(
            reinterpret_cast<std::uintptr_t>(stack) + 0x28);
        if (code == nullptr) return false;
        const unsigned token = code[0];
        size_t i = 0;
        if (token == 0x34) // EX_UnicodeStringConst
        {
            const auto src = reinterpret_cast<const wchar_t*>(code + 1);
            for (; i + 1 < cap && src[i] != 0; ++i) out[i] = src[i];
        }
        else if (token == 0x1F) // EX_StringConst
        {
            const auto src = reinterpret_cast<const unsigned char*>(code + 1);
            for (; i + 1 < cap && src[i] != 0; ++i) out[i] = static_cast<wchar_t>(src[i]);
        }
        else
        {
            return false;
        }
        out[i] = 0;
        return i != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __fastcall CallFunctionHook(void* context, void* stack, void* result, void* function) noexcept
{
    const bool target = !g_reading &&
        (function == g_getVariableObjectFn || function == g_actionScriptObjectMovieFn ||
         function == g_actionScriptObjectValueFn || function == g_getObjectValueFn);
    wchar_t path[192] = {};
    const bool havePath = target && DecodeLiteralPath(stack, path, 192);

    reinterpret_cast<tCallFunction>(g_originalCallFunction)(context, stack, result, function);

    if (!havePath || result == nullptr) return;
    void* returned = nullptr;
    __try { returned = *reinterpret_cast<void* volatile*>(result); }
    __except (EXCEPTION_EXECUTE_HANDLER) { returned = nullptr; }
    if (returned == nullptr) return;
    GetVariableObjectParams captured = {};
    captured.path.data = path;
    captured.path.count = static_cast<int>(wcslen(path)) + 1;
    captured.path.capacity = captured.path.count;
    captured.result = returned;
    CaptureMapping(context, &captured);
}

void __fastcall ProcessEventHook(void* context, void* function, void* params, void* result) noexcept
{
    const bool capture = !g_reading && function == g_getVariableObjectFn;
    const bool isHud = !g_reading && g_hudClass != 0 && IsExactClass(context, g_hudClass);
    if (isHud)
    {
        const auto hud = reinterpret_cast<std::uintptr_t>(context);
        const auto previous = g_lastHud.exchange(hud, std::memory_order_acq_rel);
        if (previous != hud)
        {
            g_editorHud = reinterpret_cast<void*>(hud);
            ClearEditorBaselines();
            g_lastMappingChangeMs.store(GetTickCount64(), std::memory_order_release);
            ME2VR::Log::Line("[ME3HUDEDIT] new gameplay HUD detected; baked layout baseline invalidated");
        }
    }

    reinterpret_cast<tProcessEvent>(g_original)(context, function, params, result);

    if (capture) CaptureMapping(context, reinterpret_cast<const GetVariableObjectParams*>(params));
    if (!isHud) return;

    ApplyEditor(context, GetTickCount64());
}

bool Resolve() noexcept
{
    if (g_hudClass == 0) g_hudClass = FindClass("SFXSFHandler_PCHUD");
    if (g_getVariableObjectFn == nullptr) g_getVariableObjectFn = FindFunction("GetVariableObject", "GFxMovie");
    if (g_actionScriptObjectMovieFn == nullptr) g_actionScriptObjectMovieFn = FindFunction("ActionScriptObject", "GFxMovie");
    if (g_actionScriptObjectValueFn == nullptr) g_actionScriptObjectValueFn = FindFunction("ActionScriptObject", "GFxValue");
    if (g_getObjectValueFn == nullptr) g_getObjectValueFn = FindFunction("GetObject", "GFxValue");
    if (g_getVariableNumberFn == nullptr) g_getVariableNumberFn = FindFunction("GetVariableNumber", "GFxMovie");
    if (g_setVariableNumberFn == nullptr) g_setVariableNumberFn = FindFunction("SetVariableNumber", "GFxMovie");
    return g_hudClass != 0 && g_getVariableObjectFn != nullptr &&
           g_actionScriptObjectMovieFn != nullptr && g_actionScriptObjectValueFn != nullptr &&
           g_getObjectValueFn != nullptr && g_getVariableNumberFn != nullptr &&
           g_setVariableNumberFn != nullptr;
}

}  // namespace

void SetEditorEnabled(bool enabled) noexcept
{
    g_editorEnabled.store(enabled, std::memory_order_release);
}

bool GetEditorEnabled() noexcept { return g_editorEnabled.load(std::memory_order_acquire); }
int GroupCount() noexcept { return kGroupCount; }
const char* GroupLabel(int group) noexcept
{
    return (group >= 0 && group < kGroupCount) ? kGroups[group].label : "Unknown";
}

GroupConfig GetGroupConfig(int group) noexcept
{
    GroupConfig result{0, 0, 1, 1};
    if (group < 0 || group >= kGroupCount) return result;
    AcquireSRWLockShared(&g_configLock);
    result = g_groupConfig[group];
    ReleaseSRWLockShared(&g_configLock);
    return result;
}

GroupConfig DefaultGroupConfig(int group) noexcept
{
    return (group >= 0 && group < kGroupCount) ? kBakedDefaults[group] : GroupConfig{0, 0, 1, 1};
}

void SetGroupConfig(int group, GroupConfig config) noexcept
{
    if (group < 0 || group >= kGroupCount) return;
    config.offsetX = SafeClamp(config.offsetX, -600.0f, 600.0f, 0.0f);
    config.offsetY = SafeClamp(config.offsetY, -500.0f, 500.0f, 0.0f);
    config.scaleX = SafeClamp(config.scaleX, 0.30f, 3.00f, 1.0f);
    config.scaleY = SafeClamp(config.scaleY, 0.30f, 3.00f, 1.0f);
    AcquireSRWLockExclusive(&g_configLock);
    g_groupConfig[group] = config;
    ReleaseSRWLockExclusive(&g_configLock);
}

void ResetGroup(int group) noexcept { if (group >= 0 && group < kGroupCount) SetGroupConfig(group, kBakedDefaults[group]); }
void ResetAll() noexcept { for (int i = 0; i < kGroupCount; ++i) ResetGroup(i); }

void Tick() noexcept
{
    static bool installed = false;
    static unsigned retry = 0;
    if (installed || (retry++ % 120) != 0 || !Resolve()) return;
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return;
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    void* original = nullptr;
    if (MH_CreateHook(reinterpret_cast<void*>(base + kProcessEventRva),
                      reinterpret_cast<void*>(&ProcessEventHook), &original) != MH_OK) return;
    g_original = original;
    void* originalCall = nullptr;
    if (MH_CreateHook(reinterpret_cast<void*>(base + kCallFunctionRva),
                      reinterpret_cast<void*>(&CallFunctionHook), &originalCall) != MH_OK) return;
    g_originalCallFunction = originalCall;
    if (MH_EnableHook(reinterpret_cast<void*>(base + kProcessEventRva)) != MH_OK) return;
    if (MH_EnableHook(reinterpret_cast<void*>(base + kCallFunctionRva)) != MH_OK) return;
    installed = true;
    ME2VR::Log::Line("[ME3HUDEDIT] baked per-element layout active; validated GFxMovie roots only");
}
}  // namespace ME2VR::HudProbe
