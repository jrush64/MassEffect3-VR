#include "dxgi_proxy.h"

#include "d3d_capture.h"
#include "logger.h"

#include <iterator>
#include <mutex>
#include <string>

namespace
{
HMODULE g_realDxgi = nullptr;
HRESULT g_loadResult = E_FAIL;
std::once_flag g_loadOnce;

std::wstring SystemDxgiPath()
{
    wchar_t systemDir[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir)));
    if (length == 0 || length >= static_cast<UINT>(std::size(systemDir))) return L"C:\\Windows\\System32\\dxgi.dll";
    std::wstring path(systemDir);
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += L"dxgi.dll";
    return path;
}

void LoadRealDxgi() noexcept
{
    const std::wstring path = SystemDxgiPath();
    g_realDxgi = LoadLibraryW(path.c_str());
    if (g_realDxgi == nullptr)
    {
        const DWORD error = GetLastError();
        g_loadResult = HRESULT_FROM_WIN32(error);
        ME2VR::Log::WindowsError("[ME3DISC] LoadLibraryW real dxgi.dll", error);
        return;
    }
    g_loadResult = S_OK;
    ME2VR::Log::Line("[ME3DISC] loaded real dxgi.dll: " + ME2VR::Log::WideToUtf8(path));
}

template <typename Fn>
Fn ResolveTyped(const char* name) noexcept
{
    return reinterpret_cast<Fn>(ME2VR::Dxgi::Resolve(name));
}
}

namespace ME2VR::Dxgi
{
bool EnsureLoaded() noexcept
{
    try
    {
        std::call_once(g_loadOnce, LoadRealDxgi);
        return SUCCEEDED(g_loadResult) && g_realDxgi != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

FARPROC Resolve(const char* name) noexcept
{
    if (name == nullptr || name[0] == '\0') return nullptr;
    if (!EnsureLoaded())
    {
        Log::Line(std::string("[ME3DISC] cannot resolve export; real dxgi not loaded: ") + name);
        return nullptr;
    }
    FARPROC proc = GetProcAddress(g_realDxgi, name);
    if (proc == nullptr) Log::WindowsError((std::string("[ME3DISC] missing real dxgi export: ") + name).c_str(), GetLastError());
    return proc;
}

HRESULT MissingExport(const char* name) noexcept
{
    Log::Line(std::string("[ME3DISC] returning failure for missing dxgi export: ") +
              (name != nullptr ? name : "<null>"));
    return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("CreateDXGIFactory");
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr)
    {
        ME2VR::Log::Line("[ME3DISC] CreateDXGIFactory captured factory.");
        ME2VR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory1");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("CreateDXGIFactory1");
    const HRESULT hr = fn(riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr)
    {
        ME2VR::Log::Line("[ME3DISC] CreateDXGIFactory1 captured factory.");
        ME2VR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** factory)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto fn = ResolveTyped<Fn>("CreateDXGIFactory2");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("CreateDXGIFactory2");
    const HRESULT hr = fn(flags, riid, factory);
    if (SUCCEEDED(hr) && factory != nullptr && *factory != nullptr)
    {
        ME2VR::Log::Line("[ME3DISC] CreateDXGIFactory2 captured factory.");
        ME2VR::D3DCapture::TryInstallFactoryHooks(*factory, riid);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIDeclareAdapterRemovalSupport()
{
    using Fn = HRESULT(WINAPI*)();
    const auto fn = ResolveTyped<Fn>("DXGIDeclareAdapterRemovalSupport");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("DXGIDeclareAdapterRemovalSupport");
    return fn();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface(REFIID riid, void** debugInterface)
{
    using Fn = HRESULT(WINAPI*)(REFIID, void**);
    const auto fn = ResolveTyped<Fn>("DXGIGetDebugInterface");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("DXGIGetDebugInterface");
    return fn(riid, debugInterface);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    using Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
    const auto fn = ResolveTyped<Fn>("DXGIGetDebugInterface1");
    if (fn == nullptr) return ME2VR::Dxgi::MissingExport("DXGIGetDebugInterface1");
    return fn(flags, riid, debugInterface);
}
