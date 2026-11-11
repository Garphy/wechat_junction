#pragma once

#include <windows.h>

#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace folder {

struct Source {
    std::wstring path;
    bool monthly = false;
};

struct Entry {
    std::wstring name;
    FILETIME modified;
    DWORD attributes;
};

constexpr bool IsReadOnlyFile(DWORD attributes) {
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)) == FILE_ATTRIBUTE_READONLY;
}

struct Snapshot {
    std::wstring path;
    std::vector<Entry> entries;
    DWORD error = ERROR_SUCCESS;
    unsigned generation = 0;
};

std::optional<Source> MakeSource(const std::wstring& selected, bool monthly);
std::wstring Resolve(const Source& source, const SYSTEMTIME& local);
UINT MillisecondsUntilMonth(const SYSTEMTIME& local);
std::wstring Join(const std::wstring& path, const std::wstring& name);
Snapshot Read(const std::wstring& path);
DWORD ClearReadOnly(const std::wstring& path);
std::optional<FILE_ID_INFO> ReadOnlyIdentity(const std::wstring& path);
std::optional<DWORD> ClearReadOnlyAfterMove(const std::wstring& source, const std::wstring& destination,
    const FILE_ID_INFO& original);

class DesktopCopies {
public:
    DesktopCopies(const std::wstring& source, const std::wstring& desktop, const std::vector<std::wstring>& names,
        const std::vector<size_t>& readOnly, bool waitForDrop);
    ~DesktopCopies();
    void Activate();

private:
    struct State;
    std::unique_ptr<State> state_;
};

class Monitor {
public:
    static constexpr UINT ready = WM_APP + 2;
    explicit Monitor(HWND receiver);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void SetPath(std::wstring path, unsigned generation, bool createMissing = false);
    std::optional<Snapshot> Take();

private:
    void Run();
    void Publish(Snapshot snapshot);

    HWND receiver_;
    HANDLE changed_;
    HANDLE stop_;
    std::mutex mutex_;
    std::wstring path_;
    unsigned generation_ = 0;
    bool createMissing_ = false;
    std::optional<Snapshot> pending_;
    std::thread worker_;
};

}
