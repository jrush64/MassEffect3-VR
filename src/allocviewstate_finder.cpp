#include "allocviewstate_finder.h"

#include "engine_probe.h"
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" {
#include <hde64.h>
}

namespace
{
constexpr std::uintptr_t kViewStateDisp = 0x46C;   // ULocalPlayer::ViewState

std::atomic_bool g_done{false};
std::uintptr_t g_base = 0, g_textStart = 0, g_textEnd = 0;

std::string Hex(std::uintptr_t v)
{
    char b[32] = {};
    sprintf_s(b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

bool InText(std::uintptr_t a) noexcept { return a >= g_textStart && a < g_textEnd; }

bool LocateText() noexcept
{
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_base == 0) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (memcmp(sec->Name, ".text", 5) == 0)
        {
            g_textStart = g_base + sec->VirtualAddress;
            g_textEnd = g_textStart + sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

// Is this a `mov [reg+disp32], rax` store with disp32 == 0x46C ? (REX.W 89 /r, reg field=rax)
bool IsViewStateStore(std::uintptr_t p) noexcept
{
    __try
    {
        const BYTE* b = reinterpret_cast<const BYTE*>(p);
        if (b[0] != 0x48 && b[0] != 0x49) return false;   // REX.W (+B for r8-r15 base)
        if (b[1] != 0x89) return false;                    // MOV r/m64, r64
        const BYTE m = b[2];
        if ((m & 0xC0) != 0x80) return false;              // mod=10 (disp32)
        if (((m >> 3) & 7) != 0) return false;             // reg field == rax
        if ((m & 7) == 4) return false;                    // exclude SIB form
        const std::uint32_t disp = *reinterpret_cast<const std::uint32_t*>(p + 3);
        return disp == kViewStateDisp;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Aligned forward-disasm into the store to find the CALL whose return rax is being stored.
std::uintptr_t CallBeforeStore(std::uintptr_t store) noexcept
{
    for (std::uintptr_t off = 0x40; off >= 0x0C; off -= 4)
    {
        std::uintptr_t p = store - off;
        std::uintptr_t lastCall = 0;
        bool ok = true;
        __try
        {
            while (p < store)
            {
                hde64s hs = {};
                unsigned int len = hde64_disasm(reinterpret_cast<void*>(p), &hs);
                if (len == 0) { ok = false; break; }
                if (hs.opcode == 0xE8 && len >= 5)
                {
                    const std::int32_t rel = static_cast<std::int32_t>(hs.imm.imm32);
                    const std::uintptr_t t = p + len + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
                    if (InText(t)) lastCall = t;
                }
                p += len;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (ok && p == store && lastCall != 0) return lastCall;   // exact alignment => trustworthy
    }
    return 0;
}
}

namespace ME2VR::AllocViewStateFinder
{
void TryFindOnce() noexcept
{
    if (g_done.load(std::memory_order_acquire)) return;
    if (ME2VR::EngineProbe::GetPrimaryLocalPlayer() == 0) return;   // wait until module fully up
    g_done.store(true, std::memory_order_release);

    if (!LocateText())
    {
        ME2VR::Log::Line("[ME3DISC] AllocViewStateFinder: could not locate .text");
        return;
    }

    std::vector<std::uintptr_t> stores;
    for (std::uintptr_t p = g_textStart; p + 7 < g_textEnd; ++p)
    {
        if (IsViewStateStore(p)) { stores.push_back(p); if (stores.size() >= 32) break; }
    }
    ME2VR::Log::Line("[ME3DISC] AllocViewStateFinder: ViewState(+0x46C) store sites=" +
                     std::to_string(stores.size()));

    std::vector<std::uintptr_t> targets;
    for (std::uintptr_t s : stores)
    {
        const std::uintptr_t call = CallBeforeStore(s);
        ME2VR::Log::Line("[ME3DISC]   store@+" + Hex(s - g_base) +
                         " preceding-call=" + (call ? "MassEffect2.exe+" + Hex(call - g_base) : std::string("none")));
        if (call != 0)
        {
            bool dup = false;
            for (std::uintptr_t t : targets) if (t == call) { dup = true; break; }
            if (!dup) targets.push_back(call);
        }
    }

    if (targets.size() == 1)
    {
        ME2VR::Log::Line("[ME3DISC] >>> AllocateViewState (likely) = MassEffect2.exe+" +
                         Hex(targets[0] - g_base) + " <<<  (single consistent xref)");
    }
    else if (targets.empty())
    {
        ME2VR::Log::Line("[ME3DISC] AllocViewStateFinder: no rax-store xref found; ctor may move via another reg.");
    }
    else
    {
        std::string line = "[ME3DISC] AllocateViewState candidates:";
        for (std::uintptr_t t : targets) line += " MassEffect2.exe+" + Hex(t - g_base);
        ME2VR::Log::Line(line + "  (pick the one called from ULocalPlayer ctor)");
    }
}
}
