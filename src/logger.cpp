#include "logger.h"

#include <ShlObj.h>

#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>

namespace
{
std::mutex g_mutex;
// The log file stays open for the life of the process. Every line used to open, append and close
// the file (plus a directory create and an environment lookup) on whatever thread logged it - and
// most lines come from the render thread. Open/close is several kernel calls each way and hands the
// file to on-access antivirus on every close, so a burst of lines was a burst of render-thread
// stalls. One handle, one write per line, flushed so a crash loses nothing.
FILE* g_file = nullptr;
bool g_fileTried = false;
LARGE_INTEGER g_qpcFreq = {};
// [FRAMETIME] cost accounting: how many lines and the worst single write in the current window.
std::atomic<unsigned> g_statLines{0};
std::atomic<unsigned> g_statMaxUs{0};

// [LOGDIR 2026-08-20] %LOCALAPPDATA%\MELEVR - the SAME folder ME1 (and the rest of the MELE VR line)
// writes to, so one place holds every game's log and a user can be pointed at a single path.
// Admin-free, survives a Steam verify, and not buried in a build tree.
// The old location was a nested path that existed only on the build machine, and because the
// creation call below only ever made ONE level, the mod silently wrote no log at all for
// everyone else. That is how v1.0 shipped with no field logs.
std::wstring BaseDir()
{
    wchar_t local[MAX_PATH] = {};
    const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return std::wstring(local, len) + L"\\MELEVR";
    return L".";
}

// [LOGDIR 2026-08-20] CreateDirectoryW creates only the FINAL component, and only when its parent
// already exists. On the build machine every level of the old nested path already existed, so
// this worked and hid the bug through the whole v1.0 cycle.
// On any other machine none of those levels exist, the create fails, _wfopen_s fails, and the mod
// writes NO LOG AT ALL - which is how a v1.0 report ended up unable to send one. Build the tree.
bool EnsureDirTree(const std::wstring& dir) noexcept
{
    if (dir.empty()) return false;
    if (SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr) == ERROR_SUCCESS) return true;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Fallback log location: next to the game exe. That folder provably exists and is writable (the mod
// was installed into it), and it is where a user will actually look. Used only when the primary
// location cannot be created or opened - e.g. a redirected/absent Documents folder.
std::wstring FallbackPath() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring p(exePath);
    const size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    return p.substr(0, slash) + L"\\ME3PassiveDxgiProbe.log";
}

// One header per opened file, written directly (not through Line, which would re-enter OpenOnce).
// The exe fingerprint is here because the mod hardcodes MassEffect3.exe RVAs: a different game
// build (EA App vs Steam, a patch, a repacked exe) silently lands every hook on the wrong bytes,
// and the visible symptom is "stereo renders flat". Size + write time identifies the exe without
// pulling in version.lib.
void WriteHeader(FILE* f) noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    unsigned long long exeSize = 0;
    SYSTEMTIME st = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        if (GetFileAttributesExW(exePath, GetFileExInfoStandard, &fad))
        {
            exeSize = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
            FILETIME local = {};
            FileTimeToLocalFileTime(&fad.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &st);
        }
    }
    char buf[768] = {};
    char exeUtf8[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, exePath, -1, exeUtf8, sizeof(exeUtf8), nullptr, nullptr);
    sprintf_s(buf,
              "\n===== MELE3 VR log opened =====\n"
              "[HOST] mod built %s %s\n"
              "[HOST] exe %s\n"
              "[HOST] exe size=%llu bytes, modified %04u-%02u-%02u %02u:%02u:%02u\n"
              "[HOST] NOTE: exe size/date identify the game build. The mod's hooks are addresses into\n"
              "[HOST]       a specific MassEffect3.exe - a different build renders flat or crashes.\n",
              __DATE__, __TIME__, exeUtf8, exeSize,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fputs(buf, f);
    fflush(f);
}

// Called with g_mutex held.
FILE* OpenOnce() noexcept
{
    if (g_file != nullptr || g_fileTried) return g_file;
    g_fileTried = true;
    try
    {
        FILE* file = nullptr;
        const std::wstring dir = BaseDir();
        if (EnsureDirTree(dir))
            _wfopen_s(&file, ME2VR::Log::LogPath().c_str(), L"ab");
        if (file == nullptr)
        {
            const std::wstring fb = FallbackPath();
            if (!fb.empty()) _wfopen_s(&file, fb.c_str(), L"ab");
        }
        if (file != nullptr)
        {
            g_file = file;
            WriteHeader(g_file);
        }
    }
    catch (...)
    {
        g_file = nullptr;
    }
    return g_file;
}
}

namespace ME2VR::Log
{
std::wstring LogPath()
{
    return BaseDir() + L"\\ME3PassiveDxgiProbe.log";
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::atomic_bool g_diagnostics{false};

// Tags that exist to investigate the renderer, not to tell anyone what happened. They are per-frame
// or per-draw and bury the useful lines (an ME3 session ran to 14.8 MB). Suppressed unless
// diagnostics are on. Anything NOT listed still gets through, so a new message stays visible.
const char* const kDiagnosticTags[] = {
    "[ME3DISC]", "[ME2DISC]", "[VEHDIAG]", "[VEHAIM]", "[OBJPANEL]", "[OBJCIRC]", "[DISPQ]", "[SFRDIAG]", "[UIGATE]",
    "[AERPACE2]", "[EYETAG2]", "[UIRATIO2]", "[UIRATIO]", "[GAMEUIQ]", "[FOVEXIT]", "[UIGHOST]", "[PANELCENSUS]",
    "[HUDDISC]", "[FILLSRC]", "[POSETAG]", "[INVMAT]", "[SFRCONV]", "[MOVEFIX]", "[FPSTORM]", "[DECOUPLE]",
    "[ME3WPN]", "[ME2WPN]", "[CINEXCL]", "[XRAPI]", "[DIBR]", "[SFR]", "[ME3FP]", "[ME2FP]", "[ME3PW]", "[ME2PW]",
    // Finished discovery probes that still print in play sessions (2026-08-15: 207 [CINEMAP] lines in
    // one 20s conversation with Diagnostics=0). Their findings are in the handoffs; the lines are noise now.
    "[CINEMAP]", "[VIEWCEN]", "[COVERSCAN2]", "[COVERSCAN3]", "[COVERSCAN4]", "[COVERWATCH]", "[FPDIAG]",
    "[CAMPROBE3]", "[CAMPROBE4]", "[GUISCENE]",
    // [LOGQUIET 2026-08-22] Periodic performance/health telemetry. Every one of these emits on a
    // timer for the whole session and none of it tells a player anything: a shipped run was 7533
    // lines with Diagnostics=0. Kept out of play sessions, one ini flip away when a report needs
    // them. What deliberately still prints: [BUILD], [HOST], [VRMODE], [PRESENTATION], the [PACE]
    // lock, menu/settings lines, and anything that failed - the identity and the transitions.
    "[FRAMETIME]", "[FRAMEOWNER]", "[XRSUBMIT]", "[PACE2]", "[AERHZ]", "[AERSUB]", "[DIBRSUB]",
    "[RTTARGETS]", "[XSTATE]", "[POSEHB]", "[HOOKCOST]",
    "[DISTFIX]", "[INPUTOWNER]", "[MODESEEN]", "[AIMSNAP]", "[ME3HUDEDIT]",
    // NOT listed on purpose: [SFRHEALTH], [SFRCLEAR] and [BBARM]. Flat stereo is the number one
    // field report, and those three lines are the only evidence of it. They are silenced at the
    // SOURCE instead - healthy windows print only under Diagnostics, a failing window always prints.
    // [BBARM 2026-08-23] It was listed here by the [LOGQUIET] sweep and that cost a whole field
    // round trip: a log was sent proving bbWrittenAtClear=0 across ~10,000 clears a window,
    // and the one line built to say WHICH link of the arming chain ate it had been filtered out.
    // It only ever emits inside the capture-failure branch, so it can never be play-session noise.
    // Marker probes: discovery finished, findings are in the handoffs.
    "[MARKERRATE]", "[MARKERIMPL]", "[MARKERNATIVE]", "[MARKERMATH]", "[MARKERVR]",
    // Cover instrumentation: cover is confirmed working, these fire on every transition/stance.
    "[COVERBYTES]", "[COVERHOLD]", "[COVEROFFSET]", "[AIMCOVER]", "[COVER]",
    // Per-state heartbeats and engine-write chatter. [PRESENTATION] deliberately stays: it is the
    // one line that answers "why did VR go flat", and it only prints on a real change.
    "[CINEHB]", "[CINEVR]", "[CINEUI]", "[FPWRITE]", "[PAWNSEEN]", "[GUISIGNAL]", "[GUINAME]",
    "[FREEZETAG]", "[POSETAG]", "[AERGAP]",
};

bool IsDiagnosticLine(const std::string& line) noexcept
{
    // A line can carry more than one tag ("[ME3XR] [EYETAG2] ..."), so check them all.
    for (const char* tag : kDiagnosticTags)
    {
        if (line.find(tag) != std::string::npos) return true;
    }
    return false;
}

void Line(const std::string& line) noexcept
{
    if (!g_diagnostics.load(std::memory_order_relaxed) && IsDiagnosticLine(line)) return;
    try
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_qpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_qpcFreq);
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        FILE* file = OpenOnce();
        if (file != nullptr)
        {
            SYSTEMTIME st = {};
            GetLocalTime(&st);
            fprintf_s(file,
                      "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
                      st.wYear, st.wMonth, st.wDay,
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                      line.c_str());
            fflush(file);
        }
        // Without a debugger attached this is a kernel round trip per line for nobody.
        if (IsDebuggerPresent()) OutputDebugStringA((line + "\r\n").c_str());
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        const unsigned us = static_cast<unsigned>((t1.QuadPart - t0.QuadPart) * 1000000ll / g_qpcFreq.QuadPart);
        g_statLines.fetch_add(1, std::memory_order_relaxed);
        unsigned prev = g_statMaxUs.load(std::memory_order_relaxed);
        while (us > prev && !g_statMaxUs.compare_exchange_weak(prev, us, std::memory_order_relaxed)) {}
    }
    catch (...)
    {
    }
}

void TakeStats(unsigned* lines, unsigned* maxUs) noexcept
{
    const unsigned l = g_statLines.exchange(0, std::memory_order_relaxed);
    const unsigned m = g_statMaxUs.exchange(0, std::memory_order_relaxed);
    if (lines != nullptr) *lines = l;
    if (maxUs != nullptr) *maxUs = m;
}

void SetDiagnostics(bool on) noexcept { g_diagnostics.store(on, std::memory_order_relaxed); }
bool DiagnosticsOn() noexcept { return g_diagnostics.load(std::memory_order_relaxed); }

void WindowsError(const char* context, DWORD error) noexcept
{
    char buffer[256] = {};
    sprintf_s(buffer, "%s failed; GetLastError=%lu", context != nullptr ? context : "<unknown>", error);
    Line(buffer);
}
}
