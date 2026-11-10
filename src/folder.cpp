#include "folder.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <system_error>
#include <utility>

namespace folder {
namespace {
std::wstring NativePath(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\")) return path;
    if (path.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
}

std::wstring Parent(const std::wstring& path) {
    auto parent = std::filesystem::path(path).parent_path().wstring();
    return parent == path ? std::wstring{} : parent;
}

bool IsDirectory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(NativePath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

DWORD EnsureDirectory(const std::wstring& path) {
    if (IsDirectory(path) || CreateDirectoryW(NativePath(path).c_str(), nullptr)) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) return IsDirectory(path) ? ERROR_SUCCESS : ERROR_DIRECTORY;
    return error;
}

class Watch {
public:
    Watch(const std::wstring& path, DWORD filter) {
        directory_ = CreateFileW(NativePath(path).c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) return;
        operation_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation_.hEvent) return;
        armed_ = ReadDirectoryChangesW(directory_, buffer_, sizeof(buffer_), FALSE, filter,
            nullptr, &operation_, nullptr) != FALSE;
    }

    ~Watch() {
        if (armed_) {
            CancelIoEx(directory_, &operation_);
            DWORD transferred = 0;
            GetOverlappedResult(directory_, &operation_, &transferred, TRUE);
        }
        if (operation_.hEvent) CloseHandle(operation_.hEvent);
        if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
    }

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    HANDLE Event() const { return armed_ ? operation_.hEvent : nullptr; }

private:
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    OVERLAPPED operation_{};
    alignas(DWORD) BYTE buffer_[8192]{};
    bool armed_ = false;
};
}

std::optional<Source> MakeSource(const std::wstring& selected, bool monthly) {
    if (selected.empty()) return std::nullopt;
    auto path = std::filesystem::path(selected).lexically_normal();
    if (!path.is_absolute()) return std::nullopt;
    if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    if (!monthly) return Source{path.wstring(), false};

    auto name = path.filename().wstring();
    if (name.size() != 7 || name[4] != L'-') return std::nullopt;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i != 4 && (name[i] < L'0' || name[i] > L'9')) return std::nullopt;
    }
    if (name.substr(0, 4) == L"0000" || name.substr(5) < L"01" || name.substr(5) > L"12") {
        return std::nullopt;
    }
    return Source{path.parent_path().wstring(), true};
}

std::wstring Join(const std::wstring& path, const std::wstring& name) {
    return (std::filesystem::path(path) / name).wstring();
}

std::wstring Resolve(const Source& source, const SYSTEMTIME& local) {
    if (!source.monthly || source.path.empty()) return source.path;
    wchar_t month[16]{};
    swprintf_s(month, L"%04u-%02u", unsigned(local.wYear), unsigned(local.wMonth));
    return Join(source.path, month);
}

UINT MillisecondsUntilMonth(const SYSTEMTIME& local) {
    SYSTEMTIME next{};
    next.wYear = local.wYear + (local.wMonth == 12);
    next.wMonth = local.wMonth == 12 ? 1 : local.wMonth + 1;
    next.wDay = 1;
    SYSTEMTIME utc{};
    FILETIME end{}, now{};
    if (!TzSpecificLocalTimeToSystemTimeEx(nullptr, &next, &utc) || !SystemTimeToFileTime(&utc, &end)) {
        return 3600000;
    }
    GetSystemTimeAsFileTime(&now);
    auto ticks = [](FILETIME time) {
        return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    auto remaining = ticks(end) > ticks(now) ? (ticks(end) - ticks(now)) / 10000 + 100 : 100;
    return static_cast<UINT>(std::min<std::uint64_t>(remaining, USER_TIMER_MAXIMUM));
}

Snapshot Read(const std::wstring& path) {
    Snapshot result;
    result.path = path;
    if (path.empty()) return result;

    WIN32_FIND_DATAW data{};
    auto query = Join(NativePath(path), L"*");
    HANDLE search = FindFirstFileExW(query.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
        nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) {
        auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND || !IsDirectory(path)) result.error = error;
        return result;
    }
    do {
        std::wstring name(data.cFileName);
        if (name != L"." && name != L"..") {
            result.entries.push_back({std::move(name), data.ftLastWriteTime, data.dwFileAttributes});
        }
    } while (FindNextFileW(search, &data));
    DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) {
        result.error = error;
        result.entries.clear();
        return result;
    }
    std::sort(result.entries.begin(), result.entries.end(), [](const Entry& left, const Entry& right) {
        auto time = CompareFileTime(&left.modified, &right.modified);
        if (time) return time > 0;
        auto name = CompareStringOrdinal(left.name.c_str(), -1, right.name.c_str(), -1, TRUE);
        return name == CSTR_EQUAL ? left.name < right.name : name == CSTR_LESS_THAN;
    });
    return result;
}

Monitor::Monitor(HWND receiver) : receiver_(receiver),
    changed_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
    stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!changed_ || !stop_) {
        if (changed_) CloseHandle(changed_);
        if (stop_) CloseHandle(stop_);
        throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    try {
        worker_ = std::thread([this] { Run(); });
    } catch (...) {
        CloseHandle(stop_);
        CloseHandle(changed_);
        throw;
    }
}

Monitor::~Monitor() {
    SetEvent(stop_);
    worker_.join();
    CloseHandle(stop_);
    CloseHandle(changed_);
}

void Monitor::SetPath(std::wstring path, unsigned generation, bool createMissing) {
    std::lock_guard lock(mutex_);
    path_ = std::move(path);
    generation_ = generation;
    createMissing_ = createMissing;
    pending_.reset();
    SetEvent(changed_);
}

std::optional<Snapshot> Monitor::Take() {
    std::lock_guard lock(mutex_);
    return std::exchange(pending_, std::nullopt);
}

void Monitor::Publish(Snapshot snapshot) {
    std::lock_guard lock(mutex_);
    if (snapshot.generation != generation_) return;
    bool notify = !pending_;
    pending_ = std::move(snapshot);
    if (notify && !PostMessageW(receiver_, ready, 0, 0)) pending_.reset();
}

void Monitor::Run() {
    for (;;) {
        std::wstring path;
        unsigned generation;
        bool createMissing;
        {
            std::lock_guard lock(mutex_);
            path = path_;
            generation = generation_;
            createMissing = createMissing_;
        }

        HANDLE events[4] = {stop_, changed_};
        if (path.empty()) {
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0) return;
            continue;
        }

        auto parent = Parent(path);
        while (!parent.empty() && !IsDirectory(parent)) parent = Parent(parent);
        Watch ancestor(parent, FILE_NOTIFY_CHANGE_DIR_NAME);
        DWORD error = createMissing ? EnsureDirectory(path) : ERROR_SUCCESS;
        Watch directory(path, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
        DWORD count = 2;
        if (directory.Event()) events[count++] = directory.Event();
        if (ancestor.Event()) events[count++] = ancestor.Event();

        Snapshot snapshot = Read(path);
        if (snapshot.error != ERROR_SUCCESS && error != ERROR_SUCCESS) snapshot.error = error;
        snapshot.generation = generation;
        bool missing = snapshot.error == ERROR_PATH_NOT_FOUND || snapshot.error == ERROR_FILE_NOT_FOUND;
        bool healthy = snapshot.error == ERROR_SUCCESS && directory.Event();
        bool canWaitForCreation = missing && ancestor.Event();
        Publish(std::move(snapshot));

        DWORD result = WaitForMultipleObjects(count, events, FALSE,
            healthy || canWaitForCreation ? INFINITE : 5000);
        if (result == WAIT_OBJECT_0) return;
        if (result >= WAIT_OBJECT_0 + 2 && result < WAIT_OBJECT_0 + count) {
            // A full snapshot after rearming also recovers from notification overflow.
            if (WaitForMultipleObjects(2, events, FALSE, 150) == WAIT_OBJECT_0) return;
        }
    }
}
}
