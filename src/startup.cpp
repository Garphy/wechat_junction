#include "startup.h"

namespace startup {
namespace {
constexpr wchar_t valueName[] = L"DesktopJunction";
constexpr size_t commandLimit = 260;

std::wstring Command(const std::wstring& executable) {
    return L"\"" + executable + L"\"";
}
}

std::optional<bool> IsEnabled(const std::wstring& executable, const wchar_t* key) {
    if (executable.empty()) return std::nullopt;
    wchar_t registered[commandLimit + 1]{};
    DWORD size = sizeof(registered);
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, key, valueName, RRF_RT_REG_SZ | RRF_NOEXPAND,
        nullptr, registered, &size);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND ||
        status == ERROR_UNSUPPORTED_TYPE || status == ERROR_MORE_DATA) return false;
    if (status != ERROR_SUCCESS) return std::nullopt;
    return CompareStringOrdinal(registered, -1, Command(executable).c_str(), -1, TRUE) == CSTR_EQUAL;
}

LSTATUS SetEnabled(const std::wstring& executable, bool enabled, const wchar_t* key) {
    if (executable.empty()) return ERROR_INVALID_PARAMETER;
    if (!enabled) {
        auto registered = IsEnabled(executable, key);
        if (!registered) return ERROR_ACCESS_DENIED;
        if (!*registered) return ERROR_SUCCESS;
        auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, key, valueName);
        return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
    }

    auto command = Command(executable);
    if (command.size() > commandLimit) return ERROR_FILENAME_EXCED_RANGE;
    HKEY target = nullptr;
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &target, nullptr);
    if (status != ERROR_SUCCESS) return status;
    status = RegSetValueExW(target, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(target);
    return status;
}

}
