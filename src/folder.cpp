#include "folder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

namespace folder {
namespace {
struct FindCloser {
    void operator()(HANDLE handle) const { FindClose(handle); }
};

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

bool Missing(const std::wstring& nativePath) {
    if (GetFileAttributesW(nativePath.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

bool FileIdentity(HANDLE file, FILE_ID_INFO& identity) {
    return GetFileInformationByHandleEx(file, FileIdInfo, &identity, sizeof(identity)) != FALSE;
}

DWORD ClearReadOnlyHandle(HANDLE file) {
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return GetLastError();
    if (!IsReadOnlyFile(basic.FileAttributes)) return ERROR_SUCCESS;
    FILE_BASIC_INFO update{};
    update.FileAttributes = basic.FileAttributes & ~FILE_ATTRIBUTE_READONLY;
    if (!update.FileAttributes) update.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    return SetFileInformationByHandle(file, FileBasicInfo, &update, sizeof(update)) ? ERROR_SUCCESS : GetLastError();
}

DWORD EnsureDirectory(const std::wstring& path) {
    if (IsDirectory(path) || CreateDirectoryW(NativePath(path).c_str(), nullptr)) return ERROR_SUCCESS;
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) return IsDirectory(path) ? ERROR_SUCCESS : ERROR_DIRECTORY;
    return error;
}

class Watch {
public:
    Watch(const std::wstring& path, DWORD filter) : filter_(filter) {
        directory_ = CreateFileW(NativePath(path).c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_ == INVALID_HANDLE_VALUE) return;
        operation_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation_.hEvent) return;
        Arm();
    }

    ~Watch() { Close(); }

    void Close() {
        if (armed_) {
            CancelIoEx(directory_, &operation_);
            DWORD transferred = 0;
            GetOverlappedResult(directory_, &operation_, &transferred, TRUE);
            armed_ = false;
        }
        if (operation_.hEvent) CloseHandle(operation_.hEvent);
        if (directory_ != INVALID_HANDLE_VALUE) CloseHandle(directory_);
        operation_.hEvent = nullptr;
        directory_ = INVALID_HANDLE_VALUE;
    }

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    HANDLE Event() const { return armed_ ? operation_.hEvent : nullptr; }

    bool Arm() {
        auto event = operation_.hEvent;
        ResetEvent(event);
        operation_ = {};
        operation_.hEvent = event;
        armed_ = ReadDirectoryChangesW(directory_, buffer_, sizeof(buffer_), FALSE, filter_,
            nullptr, &operation_, nullptr) != FALSE;
        return armed_;
    }

    template<class Function>
    void ReadChanges(Function record) {
        DWORD bytes = 0;
        bool complete = GetOverlappedResult(directory_, &operation_, &bytes, FALSE) != FALSE;
        armed_ = false;
        if (!complete) return;
        constexpr size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        for (size_t offset = 0; offset + header <= bytes;) {
            const auto& change = *reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer_ + offset);
            if (change.FileNameLength > bytes - offset - header) break;
            record(change.Action, std::wstring_view(change.FileName, change.FileNameLength / sizeof(wchar_t)));
            if (!change.NextEntryOffset) break;
            offset += change.NextEntryOffset;
        }
    }

private:
    HANDLE directory_ = INVALID_HANDLE_VALUE;
    OVERLAPPED operation_{};
    alignas(DWORD) BYTE buffer_[8192]{};
    DWORD filter_;
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
    std::unique_ptr<void, FindCloser> enumeration(search);
    do {
        std::wstring name(data.cFileName);
        if (name != L"." && name != L"..") {
            result.entries.push_back({std::move(name), data.ftLastWriteTime, data.dwFileAttributes});
        }
    } while (FindNextFileW(search, &data));
    DWORD error = GetLastError();
    enumeration.reset();
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

DWORD ClearReadOnly(const std::wstring& path) {
    HANDLE file = CreateFileW(NativePath(path).c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ClearReadOnlyHandle(file);
    CloseHandle(file);
    return error;
}

std::optional<FILE_ID_INFO> ReadOnlyIdentity(const std::wstring& path) {
    auto native = NativePath(path);
    if (!IsReadOnlyFile(GetFileAttributesW(native.c_str()))) return std::nullopt;
    HANDLE file = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ID_INFO identity{};
    bool found = FileIdentity(file, identity);
    CloseHandle(file);
    return found ? std::optional{identity} : std::nullopt;
}

std::optional<DWORD> ClearReadOnlyAfterMove(const std::wstring& source, const std::wstring& destination,
    const FILE_ID_INFO& original) {
    if (!Missing(NativePath(source))) return std::nullopt;
    HANDLE file = CreateFileW(NativePath(destination).c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ID_INFO current{};
    std::optional<DWORD> result;
    if (FileIdentity(file, current) && current.VolumeSerialNumber == original.VolumeSerialNumber &&
        std::memcmp(&current.FileId, &original.FileId, sizeof(current.FileId)) == 0) {
        result = ClearReadOnlyHandle(file);
    }
    CloseHandle(file);
    return result;
}

namespace {
bool SameIdentity(const FILE_ID_INFO& left, const FILE_ID_INFO& right) {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
        std::memcmp(&left.FileId, &right.FileId, sizeof(left.FileId)) == 0;
}

struct CloseFile {
    void operator()(HANDLE handle) const {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using File = std::unique_ptr<void, CloseFile>;

File OpenCopyFile(const std::wstring& path, DWORD access, DWORD share) {
    return File(CreateFileW(path.c_str(), access, share, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
}

std::optional<FILE_ID_INFO> IdentityAt(const std::wstring& native) {
    auto file = OpenCopyFile(native, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    FILE_ID_INFO id{};
    return file.get() != INVALID_HANDLE_VALUE && FileIdentity(file.get(), id) ? std::optional{id} : std::nullopt;
}

struct CopyCandidate {
    std::wstring name, source, destination;
    FILE_ID_INFO sourceId;
    std::optional<FILE_ID_INFO> existing;
    ULONGLONG retryUntil = 0;
    bool appeared = false;
};

bool EqualFileContents(HANDLE source, HANDLE destination, HANDLE stop) {
    LARGE_INTEGER sourceSize{}, destinationSize{};
    if (!GetFileSizeEx(source, &sourceSize) || !GetFileSizeEx(destination, &destinationSize) ||
        sourceSize.QuadPart != destinationSize.QuadPart) return false;
    std::array<BYTE, 65536> left, right;
    auto remaining = sourceSize.QuadPart;
    while (remaining > 0 && WaitForSingleObject(stop, 0) == WAIT_TIMEOUT) {
        DWORD a = 0, b = 0;
        DWORD count = static_cast<DWORD>(std::min<LONGLONG>(remaining, left.size()));
        if (!ReadFile(source, left.data(), count, &a, nullptr) ||
            !ReadFile(destination, right.data(), count, &b, nullptr) ||
            a != count || b != count || std::memcmp(left.data(), right.data(), count) != 0) return false;
        remaining -= count;
    }
    return remaining == 0;
}

bool CheckDesktopCopy(CopyCandidate& candidate, HANDLE stop) {
    auto destination = OpenCopyFile(candidate.destination, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ);
    if (destination.get() == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO newId{};
    FILE_BASIC_INFO basic{};
    if (!FileIdentity(destination.get(), newId) ||
        !GetFileInformationByHandleEx(destination.get(), FileBasicInfo, &basic, sizeof(basic))) return false;
    if (SameIdentity(newId, candidate.sourceId) ||
        (candidate.existing && SameIdentity(newId, *candidate.existing))) return true;
    if (!IsReadOnlyFile(basic.FileAttributes) || (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;

    auto source = OpenCopyFile(candidate.source, GENERIC_READ, FILE_SHARE_READ);
    FILE_ID_INFO sourceId{};
    if (source.get() == INVALID_HANDLE_VALUE || !FileIdentity(source.get(), sourceId) ||
        !SameIdentity(sourceId, candidate.sourceId)) return false;
    if (!EqualFileContents(source.get(), destination.get(), stop) || WaitForSingleObject(stop, 0) != WAIT_TIMEOUT) {
        return false;
    }
    if (ClearReadOnlyHandle(destination.get()) != ERROR_SUCCESS) return false;
    candidate.existing = newId;
    return true;
}
}

struct DesktopCopies::State {
    State(const std::wstring& source, const std::wstring& desktop, const std::vector<std::wstring>& names,
        const std::vector<size_t>& readOnly, bool waitForDrop)
        : watch(desktop, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE |
            FILE_NOTIFY_CHANGE_ATTRIBUTES),
          stop(CreateEventW(nullptr, TRUE, FALSE, nullptr)), activated(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
          waitForDrop(waitForDrop) {
        if (!watch.Event() || !stop || !activated) return;
        for (size_t index : readOnly) {
            const auto& name = names[index];
            auto path = Join(source, name);
            auto id = ReadOnlyIdentity(path);
            if (!id) continue;
            auto destination = NativePath(Join(desktop, name));
            auto existing = IdentityAt(destination);
            if (!existing && !Missing(destination)) continue;
            candidates.push_back({name, NativePath(path), std::move(destination), *id, existing});
        }
        if (!candidates.empty()) worker = std::thread([this] { Run(); watch.Close(); });
        else watch.Close();
    }

    ~State() {
        if (worker.joinable()) {
            SetEvent(stop.get());
            worker.join();
        }
    }

    void Activate() {
        activeUntil = GetTickCount64() + 300000;
        if (activated) SetEvent(activated.get());
    }

    void Run() {
        HANDLE events[] = {stop.get(), activated.get(), watch.Event()};
        DWORD timeout = INFINITE;
        for (;;) {
            DWORD result = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, timeout);
            if (result == WAIT_OBJECT_0 || result == WAIT_FAILED || (result == WAIT_TIMEOUT && timeout == 30000)) return;
            auto now = GetTickCount64();
            auto deadline = activeUntil.load();
            if (result == WAIT_OBJECT_0 + 2) {
                watch.ReadChanges([&](DWORD action, std::wstring_view name) {
                    if (now > deadline && (!waitForDrop || deadline != 0)) return;
                    for (auto& candidate : candidates) {
                        if (CompareStringOrdinal(candidate.name.data(), static_cast<int>(candidate.name.size()), name.data(),
                            static_cast<int>(name.size()), TRUE) != CSTR_EQUAL) continue;
                        if (action == FILE_ACTION_ADDED || action == FILE_ACTION_RENAMED_NEW_NAME) {
                            candidate.appeared = true;
                            candidate.retryUntil = now + 3000;
                        } else if (action == FILE_ACTION_MODIFIED && (candidate.appeared || candidate.existing)) {
                            candidate.retryUntil = now + 3000;
                        } else if (action == FILE_ACTION_REMOVED || action == FILE_ACTION_RENAMED_OLD_NAME) {
                            candidate.appeared = false;
                            candidate.retryUntil = 0;
                        }
                        break;
                    }
                });
                if (!watch.Arm()) return;
            }
            timeout = waitForDrop && deadline ? 30000 : INFINITE;
            if (now <= deadline && deadline != 0) {
                for (auto& candidate : candidates) {
                    if (candidate.retryUntil <= now || CheckDesktopCopy(candidate, stop.get())) {
                        candidate.retryUntil = 0;
                    } else {
                        timeout = 350;
                    }
                }
            } else if (waitForDrop && deadline != 0) {
                return;
            }
        }
    }

    Watch watch;
    File stop, activated;
    const bool waitForDrop;
    std::atomic<ULONGLONG> activeUntil{0};
    std::vector<CopyCandidate> candidates;
    std::thread worker;
};

DesktopCopies::DesktopCopies(const std::wstring& source, const std::wstring& desktop,
    const std::vector<std::wstring>& names, const std::vector<size_t>& readOnly, bool waitForDrop)
    : state_(std::make_unique<State>(source, desktop, names, readOnly, waitForDrop)) {}

DesktopCopies::~DesktopCopies() = default;
void DesktopCopies::Activate() { state_->Activate(); }

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
            ResetEvent(changed_);
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
