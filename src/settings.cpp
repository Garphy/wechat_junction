#include "settings.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace settings {
namespace {
constexpr wchar_t legacyKey[] = L"Software\\DesktopJunction";
constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";
constexpr std::streamoff maxIniBytes = 256 * 1024;

struct LegacySettings {
    DWORD version;
    DWORD flags;
    RECT bounds;
    UINT dpi;
};

void SetBounds(Values& values, RECT bounds, UINT dpi) {
    auto width = static_cast<long long>(bounds.right) - bounds.left;
    auto height = static_cast<long long>(bounds.bottom) - bounds.top;
    if (width > 0 && height > 0 && width < 65536 && height < 65536 && dpi >= 48 && dpi <= 768) {
        values.bounds = bounds;
        values.dpi = dpi;
    }
}

void SetSource(Values& values, const std::wstring& path, bool monthly) {
    if (path.find(L'\0') != std::wstring::npos) return;
    if (auto source = folder::MakeSource(path, false)) {
        values.source = std::move(*source);
        values.source.monthly = monthly;
    }
}

Values ReadLegacy(bool& imported) {
    Values values;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, legacyKey, L"Settings", RRF_RT_REG_BINARY,
        nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(LegacySettings) ||
        size > sizeof(LegacySettings) + 32768 * sizeof(wchar_t)) return values;
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, legacyKey, L"Settings", RRF_RT_REG_BINARY,
        nullptr, bytes.data(), &size) != ERROR_SUCCESS || size < sizeof(LegacySettings) ||
        (size - sizeof(LegacySettings)) % sizeof(wchar_t)) return values;

    LegacySettings stored{};
    std::memcpy(&stored, bytes.data(), sizeof(stored));
    if (stored.version != 1) return values;
    std::wstring path((size - sizeof(stored)) / sizeof(wchar_t), L'\0');
    std::memcpy(path.data(), bytes.data() + sizeof(stored), size - sizeof(stored));
    SetSource(values, path, (stored.flags & 1) != 0);
    values.pinned = (stored.flags & 2) != 0;
    SetBounds(values, stored.bounds, stored.dpi);
    imported = true;
    return values;
}

Values ReadIni(const std::wstring& path) {
    Values values;
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (size <= 0 || size > maxIniBytes) return values;
    std::string bytes(static_cast<size_t>(size), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) return values;
    std::string_view input(bytes);
    if (input.starts_with(utf8Bom)) input.remove_prefix(utf8Bom.size());
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!length) return values;
    std::wstring decoded(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), decoded.data(), length);

    std::wistringstream lines(std::move(decoded));
    std::wstring line, section, source;
    bool monthly = false;
    RECT bounds{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.starts_with(L"[")) {
            section = line;
            continue;
        }
        auto separator = line.find(L'=');
        if (separator == std::wstring::npos) continue;
        auto key = std::wstring_view(line).substr(0, separator);
        auto value = line.substr(separator + 1);
        if (section == L"[Mapping]" && key == L"Path") source = std::move(value);
        if (section == L"[Mapping]" && key == L"Mode") monthly = value == L"Monthly";
        if (section == L"[Panel]" && key == L"Pinned") values.pinned = value == L"1";
        if (section == L"[Panel]" && key == L"Monitor" && value.size() < 128 &&
            value.find(L'\0') == std::wstring::npos) values.monitor = std::move(value);
        if (section == L"[Panel]" && key == L"WorkOrigin") {
            POINT parsed{};
            wchar_t comma{};
            std::wistringstream coordinates(value);
            if (coordinates >> parsed.x >> comma >> parsed.y && comma == L',' &&
                (coordinates >> std::ws).eof()) values.workOrigin = parsed;
        }
        if (section == L"[Panel]" && key == L"Dpi") {
            std::wistringstream number(value);
            if (!(number >> dpi) || !(number >> std::ws).eof()) dpi = USER_DEFAULT_SCREEN_DPI;
        }
        if (section == L"[Panel]" && key == L"Bounds") {
            RECT parsed{};
            wchar_t a{}, b{}, c{};
            std::wistringstream coordinates(value);
            if (coordinates >> parsed.left >> a >> parsed.top >> b >> parsed.right >> c >> parsed.bottom &&
                a == L',' && b == L',' && c == L',' && (coordinates >> std::ws).eof()) bounds = parsed;
        }
    }
    SetSource(values, source, monthly);
    SetBounds(values, bounds, dpi);
    if (IsRectEmpty(&values.bounds) || values.monitor.empty() || !values.workOrigin) {
        values.monitor.clear();
        values.workOrigin.reset();
    }
    return values;
}
}

std::wstring ExecutablePath() {
    std::wstring exe(32768, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (!length || length >= exe.size()) return {};
    exe.resize(length);
    return exe;
}

std::wstring FilePath() {
    auto exe = ExecutablePath();
    if (exe.empty()) return {};
    return (std::filesystem::path(exe).parent_path() / L"DesktopJunction.ini").wstring();
}

Values Load(const std::wstring& path, bool& imported) {
    imported = false;
    if (path.empty()) return {};
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return ReadIni(path);
    return GetLastError() == ERROR_FILE_NOT_FOUND ? ReadLegacy(imported) : Values{};
}

bool Write(const std::wstring& path, const Values& values) {
    if (path.empty()) return false;
    auto& rect = values.bounds;
    std::wstring contents = L"[Mapping]\r\nPath=" + values.source.path +
        L"\r\nMode=" + (values.source.monthly ? L"Monthly" : L"Fixed") +
        L"\r\n\r\n[Panel]\r\nBounds=" + std::to_wstring(rect.left) + L"," + std::to_wstring(rect.top) +
        L"," + std::to_wstring(rect.right) + L"," + std::to_wstring(rect.bottom) +
        L"\r\nDpi=" + std::to_wstring(values.dpi) + L"\r\nPinned=" + (values.pinned ? L"1" : L"0") + L"\r\n";
    if (!values.monitor.empty() && values.workOrigin) {
        contents += L"Monitor=" + values.monitor + L"\r\nWorkOrigin=" +
            std::to_wstring(values.workOrigin->x) + L"," + std::to_wstring(values.workOrigin->y) + L"\r\n";
    }
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()),
        nullptr, 0, nullptr, nullptr);
    if (!length) return false;
    std::string bytes(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, contents.data(), static_cast<int>(contents.size()),
        bytes.data(), length, nullptr, nullptr)) return false;

    auto temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    std::ofstream file(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(utf8Bom.data(), static_cast<std::streamsize>(utf8Bom.size()));
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (file && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    DeleteFileW(temporary.c_str());
    return false;
}

}
