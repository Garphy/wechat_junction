#include "../src/folder.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {
void Check(bool pass, const char* message) {
    if (!pass) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path path;
    Fixture() {
        auto name = L"desktop-junction-tests-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        path = std::filesystem::temp_directory_path() / name;
        Check(std::filesystem::create_directory(path), "create isolated fixture");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void CreateFileWithTime(const std::filesystem::path& path, unsigned second) {
    auto file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, CREATE_NEW, 0, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create fixture file");
    SYSTEMTIME stamp{2026, 11, 0, 5, 5, 30, static_cast<WORD>(second), 0};
    FILETIME time{};
    bool success = SystemTimeToFileTime(&stamp, &time) && SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
    Check(success, "set deterministic modification time");
}

template<class Predicate>
folder::Snapshot WaitFor(folder::Monitor& monitor, Predicate predicate) {
    using namespace std::chrono_literals;
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto snapshot = monitor.Take(); snapshot && predicate(*snapshot)) return std::move(*snapshot);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
        std::this_thread::sleep_for(15ms);
    }
    throw std::runtime_error("timed out waiting for directory snapshot");
}

void Test(const std::filesystem::path& root) {
    auto month = root / L"2026-11";
    auto fixed = folder::MakeSource(month.wstring() + L"\\", false);
    auto monthly = folder::MakeSource(month.wstring(), true);
    Check(fixed && fixed->path == month && !fixed->monthly, "fixed source normalization");
    Check(monthly && monthly->path == root && monthly->monthly, "monthly source stores parent");
    Check(!folder::MakeSource((root / L"2026-13").wstring(), true), "reject invalid month");
    Check(!folder::MakeSource(L"relative\\2026-11", true), "reject relative path");
    SYSTEMTIME december{2026, 12, 0, 31, 23, 59, 59, 0};
    SYSTEMTIME january{2027, 1, 0, 1, 0, 0, 0, 0};
    Check(folder::Resolve(*fixed, january) == month, "fixed source remains fixed across years");
    Check(folder::Resolve(*monthly, december) == root / L"2026-12", "resolve December");
    Check(folder::Resolve(*monthly, january) == root / L"2027-01", "resolve January in new year");

    HWND receiver = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(receiver != nullptr, "create monitor message receiver");
    {
        folder::Monitor monitor(receiver);
        monitor.SetPath(month.wstring(), 1);
        auto unavailable = [](const folder::Snapshot& snapshot) {
            return snapshot.error == ERROR_FILE_NOT_FOUND || snapshot.error == ERROR_PATH_NOT_FOUND;
        };
        WaitFor(monitor, unavailable);

        std::filesystem::create_directory(month);
        WaitFor(monitor, [](const auto& snapshot) { return !snapshot.error && snapshot.entries.empty(); });
        CreateFileWithTime(month / L"b.txt", 1);
        CreateFileWithTime(month / L"a.txt", 1);
        CreateFileWithTime(month / L"微信附件.txt", 2);
        auto sorted = WaitFor(monitor, [](const auto& snapshot) { return snapshot.entries.size() == 3; });
        Check(sorted.entries[0].name == L"微信附件.txt" && sorted.entries[1].name == L"a.txt" &&
            sorted.entries[2].name == L"b.txt", "sort by modified time, then name");
        auto child = month / L"child";
        std::filesystem::create_directory(child);
        CreateFileWithTime(child / L"nested.txt", 3);
        WaitFor(monitor, [](const auto& snapshot) { return snapshot.entries.size() == 4; });
        auto archived = root / L"archive";
        std::filesystem::rename(month, archived);
        WaitFor(monitor, unavailable);
        std::filesystem::rename(archived, month);
        WaitFor(monitor, [](const auto& snapshot) { return snapshot.entries.size() == 4; });
        std::filesystem::rename(month / L"b.txt", month / L"renamed.txt");
        std::filesystem::remove(month / L"a.txt");
        WaitFor(monitor, [](const auto& snapshot) {
            return snapshot.entries.size() == 3 &&
                std::any_of(snapshot.entries.begin(), snapshot.entries.end(), [](const auto& entry) {
                    return entry.name == L"renamed.txt";
                });
        });
        monitor.SetPath((root / L"2027-01").wstring(), 2);
        auto switched = WaitFor(monitor, [](const auto& snapshot) { return snapshot.generation == 2; });
        Check(unavailable(switched) && switched.entries.empty(), "source switch does not publish stale rows");
    }
    DestroyWindow(receiver);
}
}

int main() {
    try {
        Fixture fixture;
        Test(fixture.path);
        std::cout << "Folder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
