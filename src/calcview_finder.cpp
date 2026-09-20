#include "calcview_finder.h"

#include "engine_probe.h"
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>
extern "C" {
#include <hde64.h>
}

namespace
{
// FViewportClient::Draw, captured live by the ME3 engine probe (2026-06-27 run).
constexpr std::uintptr_t kDrawRva = 0x6D1E30;

constexpr int kPresentsPerCandidate = 8;     // frames to let each candidate fire
constexpr size_t kMaxCandidates = 220;

std::atomic_bool g_started{false};
std::atomic_bool g_done{false};
bool g_minhookReady = false;

std::vector<std::uintptr_t> g_candidates;
size_t g_curIdx = 0;
bool g_curHooked = false;
void* g_curTarget = nullptr;
int g_curPresents = 0;
int g_matchCount = 0;

// --- shared, register-safe passthrough stub state (read/written by hand-built asm) ----
// The stub clobbers ONLY rax + flags (both volatile, never argument registers) and
// tail-jumps to the original, so it cannot corrupt integer OR XMM args of any callee.
volatile std::uint64_t g_p1Value = 0;       // current P1 ULocalPlayer pointer
volatile std::uint8_t  g_curArgHit = 0;     // set when current candidate is called with rcx==P1
volatile std::uint64_t g_curP1Count = 0;    // # of rcx==P1 calls for current candidate
void* g_curTrampoline = nullptr;            // MinHook trampoline for current candidate
void* g_stub = nullptr;                     // the shared passthrough detour

std::uintptr_t g_moduleBase = 0;
std::uintptr_t g_moduleEnd = 0;

std::string Hex(std::uintptr_t v)
{
    char buf[32] = {};
    sprintf_s(buf, "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

bool InModuleCode(std::uintptr_t addr) noexcept
{
    if (addr < g_moduleBase || addr >= g_moduleEnd) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & exec) != 0;
}

// Build the passthrough stub once. Pseudocode:
//   if (*g_p1Value == rcx) { g_curArgHit = 1; ++g_curP1Count; }
//   jmp [g_curTrampoline]
// Clobbers rax + flags only; arguments (rcx/rdx/r8/r9 and all XMM) pass through untouched.
void* BuildStub() noexcept
{
    BYTE* s = static_cast<BYTE*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (s == nullptr) return nullptr;
    size_t i = 0;
    auto emit = [&](BYTE b) { s[i++] = b; };
    auto emitImm64 = [&](void* p) { std::memcpy(s + i, &p, 8); i += 8; };

    // mov rax, &g_p1Value
    emit(0x48); emit(0xB8); emitImm64(const_cast<std::uint64_t*>(&g_p1Value));
    // cmp [rax], rcx        (48 39 08)
    emit(0x48); emit(0x39); emit(0x08);
    // jne <skip>            (75 rel8) -- patch rel8 after the mod knows block size
    emit(0x75); const size_t jneOperand = i; emit(0x00);
    const size_t blockStart = i;
    // mov rax, &g_curArgHit ; mov byte [rax], 1
    emit(0x48); emit(0xB8); emitImm64(const_cast<std::uint8_t*>(&g_curArgHit));
    emit(0xC6); emit(0x00); emit(0x01);
    // mov rax, &g_curP1Count ; inc qword [rax]   (48 FF 00)
    emit(0x48); emit(0xB8); emitImm64(const_cast<std::uint64_t*>(&g_curP1Count));
    emit(0x48); emit(0xFF); emit(0x00);
    // <skip>:
    s[jneOperand] = static_cast<BYTE>(i - blockStart);   // rel8 from end of jne to skip
    // mov rax, &g_curTrampoline ; jmp [rax]   (FF 20)
    emit(0x48); emit(0xB8); emitImm64(&g_curTrampoline);
    emit(0xFF); emit(0x20);

    FlushInstructionCache(GetCurrentProcess(), s, i);
    return s;
}

void ScanCallsInto(std::uintptr_t fnStart, std::vector<std::uintptr_t>& out) noexcept
{
    constexpr size_t kMaxFnBytes = 0x2000;
    std::uintptr_t p = fnStart;
    const std::uintptr_t end = fnStart + kMaxFnBytes;
    while (p < end && out.size() < kMaxCandidates)
    {
        hde64s hs = {};
        unsigned int len = 0;
        __try { len = hde64_disasm(reinterpret_cast<void*>(p), &hs); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (len == 0) { ++p; continue; }

        if (hs.opcode == 0xE8 && len >= 5)   // CALL rel32
        {
            const std::int32_t rel = static_cast<std::int32_t>(hs.imm.imm32);
            const std::uintptr_t target = p + len + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
            if (InModuleCode(target))
            {
                bool dup = false;
                for (std::uintptr_t t : out) if (t == target) { dup = true; break; }
                if (!dup) out.push_back(target);
            }
        }
        if (hs.opcode == 0xC3)   // RET followed by int3 padding => function end
        {
            const BYTE next = *reinterpret_cast<const BYTE*>(p + len);
            if (next == 0xCC) break;
        }
        p += len;
    }
}

void BuildCandidates() noexcept
{
    const std::uintptr_t drawAddr = g_moduleBase + kDrawRva;
    std::vector<std::uintptr_t> depth1;
    ScanCallsInto(drawAddr, depth1);
    for (std::uintptr_t t : depth1)
    {
        if (g_candidates.size() >= kMaxCandidates) break;
        g_candidates.push_back(t);
    }
    for (std::uintptr_t t : depth1)
    {
        if (g_candidates.size() >= kMaxCandidates) break;
        ScanCallsInto(t, g_candidates);   // depth-2: CalcSceneView may be one level down
    }
    ME2VR::Log::Line("[ME3DISC] CalcViewFinder: Draw=" + Hex(drawAddr) +
                     " candidates=" + std::to_string(g_candidates.size()) +
                     " (register-safe passthrough probe)");
}

void UnhookCurrent() noexcept
{
    if (g_curHooked && g_curTarget != nullptr)
    {
        MH_DisableHook(g_curTarget);
        MH_RemoveHook(g_curTarget);
    }
    g_curHooked = false;
    g_curTarget = nullptr;
    g_curTrampoline = nullptr;
}
}

namespace ME2VR::CalcViewFinder
{
void Tick() noexcept
{
    if (g_done.load(std::memory_order_acquire)) return;

    const std::uintptr_t p1 = ME2VR::EngineProbe::GetPrimaryLocalPlayer();
    if (p1 == 0) return;                       // no local player yet
    g_p1Value = p1;

    bool expectedStart = false;
    if (g_started.compare_exchange_strong(expectedStart, true, std::memory_order_acq_rel))
    {
        g_moduleBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if (g_moduleBase != 0)
        {
            const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_moduleBase);
            const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_moduleBase + dos->e_lfanew);
            g_moduleEnd = g_moduleBase + nt->OptionalHeader.SizeOfImage;
        }
        g_stub = BuildStub();
        if (g_stub == nullptr) { ME2VR::Log::Line("[ME3DISC] CalcViewFinder: stub alloc FAILED"); g_done = true; return; }
        // The draw-hook installer may have already MH_Initialize()'d - ALREADY_INITIALIZED is fine.
        const MH_STATUS mhs = MH_Initialize();
        if (mhs == MH_OK || mhs == MH_ERROR_ALREADY_INITIALIZED) g_minhookReady = true;
        else { ME2VR::Log::Line("[ME3DISC] CalcViewFinder: MH_Initialize FAILED"); g_done = true; return; }
        BuildCandidates();
        return;
    }

    if (!g_minhookReady || g_candidates.empty()) return;

    if (!g_curHooked)
    {
        if (g_curIdx >= g_candidates.size())
        {
            ME2VR::Log::Line("[ME3DISC] CalcViewFinder: done. " + std::to_string(g_matchCount) +
                             " function(s) called with rcx==P1 ULocalPlayer (CalcSceneView is among them).");
            g_done.store(true, std::memory_order_release);
            return;
        }
        void* target = reinterpret_cast<void*>(g_candidates[g_curIdx]);
        g_curArgHit = 0;
        g_curP1Count = 0;
        g_curTrampoline = nullptr;
        if (MH_CreateHook(target, g_stub, &g_curTrampoline) == MH_OK && MH_EnableHook(target) == MH_OK)
        {
            g_curHooked = true;
            g_curTarget = target;
            g_curPresents = 0;
        }
        else
        {
            MH_RemoveHook(target);
            ++g_curIdx;
        }
        return;
    }

    if (++g_curPresents >= kPresentsPerCandidate)
    {
        if (g_curArgHit != 0)
        {
            const std::uintptr_t rva = reinterpret_cast<std::uintptr_t>(g_curTarget) - g_moduleBase;
            ++g_matchCount;
            ME2VR::Log::Line("[ME3DISC] >>> CalcSceneView CANDIDATE: MassEffect2.exe+" + Hex(rva) +
                             "  (called with rcx==P1, p1Calls=" + std::to_string((unsigned long long)g_curP1Count) +
                             " over " + std::to_string(kPresentsPerCandidate) + " frames) <<<");
        }
        UnhookCurrent();
        ++g_curIdx;
    }
}
}
