#pragma once

#include <windows.h>

#include <mutex>
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

class Monitor {
public:
    static constexpr UINT ready = WM_APP + 2;
    explicit Monitor(HWND receiver);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    void SetPath(std::wstring path, unsigned generation);
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
    std::optional<Snapshot> pending_;
    std::thread worker_;
};

}
