#include "dxgi_proxy.h"
#include "d3d_capture.h"
#include "logger.h"


#include <Windows.h>

#include <string>

namespace
{
std::string ModuleState(const wchar_t* name)
{
    HMODULE module = GetModuleHandleW(name);
    if (module == nullptr) return ME2VR::Log::WideToUtf8(name) + "=not_loaded";

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    return ME2VR::Log::WideToUtf8(name) + "=" + ME2VR::Log::WideToUtf8(path);
}

DWORD WINAPI StartupThread(LPVOID) noexcept
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ME2VR::Log::Line("[ME3DISC] process=" + ME2VR::Log::WideToUtf8(exe) +
                     " pid=" + std::to_string(GetCurrentProcessId()));
    ME2VR::Log::Line("[ME3DISC] " + ModuleState(L"dxgi.dll"));
    ME2VR::Log::Line("[ME3DISC] " + ModuleState(L"d3d11.dll"));
    ME2VR::Log::Line("[ME3DISC] " + ModuleState(L"d3dcompiler_46.dll"));
    ME2VR::Log::Line("[ME3DISC] " + ModuleState(L"openxr_loader.dll"));
    // Before the game reads its resolution settings (clamps to desktop mode internally, ahead of the
    // swapchain). ArmResolutionSpoof() already ran synchronously in DllMain; this installs the hooks.
    ME2VR::D3DCapture::InstallDisplayQueryHooks();
    ME2VR::D3DCapture::InstallBreakpointProbes();
    ME2VR::Dxgi::EnsureLoaded();
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        // BEFORE the worker thread and before main(). The engine reads GamerSettings.ini during its own
        // init and will beat any thread the mod spawns. File I/O + GetSystemMetrics only - loader-lock safe.
        ME2VR::D3DCapture::ArmResolutionSpoof();

        HANDLE thread = CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    }
    return TRUE;
}
