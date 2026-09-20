#pragma once

#include <Windows.h>

namespace ME2VR::Dxgi
{
bool EnsureLoaded() noexcept;
FARPROC Resolve(const char* name) noexcept;
HRESULT MissingExport(const char* name) noexcept;
}
