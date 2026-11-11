#pragma once

#include <windows.h>

#include <optional>
#include <string>

namespace startup {

inline constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

std::optional<bool> IsEnabled(const std::wstring& executable, const wchar_t* key = runKey);
LSTATUS SetEnabled(const std::wstring& executable, bool enabled, const wchar_t* key = runKey);

}
