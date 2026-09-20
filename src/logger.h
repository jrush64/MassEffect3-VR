#pragma once

#include <Windows.h>

#include <string>

namespace ME2VR::Log
{
void Line(const std::string& line) noexcept;
// Developer diagnostics. OFF by default so the shipped log stays short and readable: a user can
// send it and it says what the mod did, not what every draw call returned. Turn on with
// Diagnostics=1 under [VR] in MELE3VR.ini when actually investigating something.
void SetDiagnostics(bool on) noexcept;
bool DiagnosticsOn() noexcept;
void WindowsError(const char* context, DWORD error) noexcept;
// [FRAMETIME] lines written and the worst single write (microseconds) since the last call; resets.
void TakeStats(unsigned* lines, unsigned* maxUs) noexcept;
std::string WideToUtf8(const std::wstring& text);
std::wstring LogPath();
}
