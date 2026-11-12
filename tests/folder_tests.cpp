#include "../src/folder.h"
#include "../src/list_row_sizing.h"
#include "../src/panel_canvas.h"
#include "../src/panel_placement.h"
#include "../src/selection_style.h"
#include "../src/settings.h"
#include "../src/shell.h"
#include "../src/startup.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        Check(unavailable(switched) && switched.entries.empty() &&
            !std::filesystem::exists(root / L"2027-01"), "fixed mode leaves missing paths alone");

        auto parent = root / L"attachments";
        auto automatic = parent / L"2027-01";
        monitor.SetPath(automatic.wstring(), 3, true);
        WaitFor(monitor, [&](const auto& snapshot) { return snapshot.generation == 3 && unavailable(snapshot); });
        Check(!std::filesystem::exists(parent), "automatic mode does not create missing parents");
        std::filesystem::create_directory(parent);
        WaitFor(monitor, [](const auto& snapshot) { return snapshot.generation == 3 && !snapshot.error; });
        Check(std::filesystem::is_directory(automatic), "automatic mode creates only the month directory");

        auto collision = parent / L"2027-02";
        CreateFileWithTime(collision, 4);
        monitor.SetPath(collision.wstring(), 4, true);
        auto failed = WaitFor(monitor, [](const auto& snapshot) { return snapshot.generation == 4; });
        Check(failed.error == ERROR_DIRECTORY && std::filesystem::is_regular_file(collision),
            "automatic mode preserves a file at the month path");
    }
    DestroyWindow(receiver);
}

void TestResources(const std::filesystem::path& root) {
    HWND receiver = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(receiver != nullptr, "create resource test receiver");
    // UxTheme initializes process-wide resources on its first use.
    {
        SelectionStyle warm;
        warm.Pixels(receiver, 160, 28, 96, true, false);
        warm.Pixels(receiver, 160, 28, 96, false, false);
    }
    auto process = GetCurrentProcess();
    DWORD handlesBefore = 0;
    Check(GetProcessHandleCount(process, &handlesBefore), "read baseline handle count");
    auto gdiBefore = GetGuiResources(process, GR_GDIOBJECTS);
    for (unsigned cycle = 0; cycle < 16; ++cycle) {
        folder::Monitor monitor(receiver);
        monitor.SetPath(root.wstring(), cycle + 1);
        WaitFor(monitor, [](const auto& snapshot) { return !snapshot.error; });
        PanelCanvas canvas;
        for (int width = 96; width < 112; ++width) {
            Check(canvas.Prepare(width, 64) != nullptr, "resize canvas");
        }
        canvas.Reset();
        Check(canvas.Prepare(112, 64) != nullptr, "restore canvas after releasing it");
        SelectionStyle selection;
        for (UINT dpi : {96u, 144u, 192u}) {
            Check(!selection.Pixels(receiver, 160, 28, dpi, true, false).empty(), "render focused selection");
            Check(!selection.Pixels(receiver, 160, 28, dpi, false, false).empty(), "render inactive selection");
            selection.Reset();
        }
    }
    DWORD handlesAfter = 0;
    Check(GetProcessHandleCount(process, &handlesAfter) && handlesAfter == handlesBefore,
        "directory monitor released its handles");
    Check(GetGuiResources(process, GR_GDIOBJECTS) == gdiBefore, "canvas released its GDI objects");
    DestroyWindow(receiver);
}

void TestSelectionCompositing() {
    SelectionStyle style;
    constexpr LONG width = 32, height = 20;
    constexpr size_t center = size_t(height / 2) * width + width / 2;
    auto active = style.Pixels(nullptr, width, height, 96, true, false);
    auto inactive = style.Pixels(nullptr, width, height, 96, false, false);
    Check(active[0] == 0 && (active[center] >> 24) > (inactive[center] >> 24) &&
        (active[center] >> 24) < 128, "fallback is rounded, translucent and stronger when focused");

    PanelCanvas canvas;
    HDC dc = canvas.Prepare(width, height);
    Check(dc != nullptr, "allocate selection test canvas");
    Check(SetPixelV(dc, width / 2, height / 2, RGB(230, 220, 210)), "draw foreground test pixel");
    canvas.NormalizeAlpha();
    RECT row{0, 0, width, height}, clip{0, 0, width, height - 1};
    canvas.CompositeBackground(row, clip, active);
    auto pixels = canvas.Pixels();
    Check(pixels[center] == 0xFFE6DCD2 && pixels[center + 1] == active[center + 1],
        "selection fills the background without tinting foreground pixels");
    Check(pixels[0] == canvasBackground && pixels.back() == canvasBackground,
        "selection preserves hit-test alpha at rounded corners and outside the viewport");
    auto contrast = style.Pixels(nullptr, width, height, 96, true, true);
    Check((contrast[center] >> 24) == 255, "high contrast uses solid system selection");
}

void TestRowSizing() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    Check(InitCommonControlsEx(&controls), "initialize native row sizing test");
    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_OWNERDATA |
        LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS, 0, 0, 420, 240, HWND_MESSAGE, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(list != nullptr, "create native list for row sizing");
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    column.cx = 400;
    Check(ListView_InsertColumn(list, 0, &column) >= 0, "create native list column");

    ListRowSizing sizing;
    auto shellImages = ImageList_Create(16, 16, ILC_COLOR32, 1, 0);
    Check(shellImages != nullptr, "create shell-sized image list");
    for (UINT dpi : {96u, 120u, 144u, 192u, 216u}) {
        int iconSize = MulDiv(16, dpi, 96);
        auto images = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 1, 0);
        Check(images != nullptr && ImageList_SetImageCount(images, 1), "create base image list");
        ListView_SetImageList(list, images, LVSIL_SMALL);
        ListView_SetItemCount(list, 4);
        RECT before{}, after{};
        Check(ListView_GetItemRect(list, 0, &before, LVIR_BOUNDS), "measure unpadded row");
        ListView_SetItemCount(list, 0);
        int padding = MulDiv(6, dpi, 96);
        int iconGap = MulDiv(6, dpi, 96);
        sizing.Update(list, shellImages, iconSize, padding, iconGap);
        ListView_SetItemCount(list, 4);
        Check(ListView_GetItemRect(list, 0, &after, LVIR_BOUNDS) &&
            (after.bottom - after.top) - (before.bottom - before.top) == padding && sizing.IconSize() == iconSize,
            "native row height increases by six logical pixels");
        RECT icon{}, label{};
        Check(ListView_GetItemRect(list, 0, &icon, LVIR_ICON) &&
            ListView_GetItemRect(list, 0, &label, LVIR_LABEL) && label.left - icon.left - sizing.IconSize() >= iconGap,
            "native label leaves six logical pixels after the drawn icon");
        ListView_SetImageList(list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(images);
    }
    DestroyWindow(list);
    ImageList_Destroy(shellImages);
}

void TestIconScaling() {
    constexpr COLORREF color = RGB(21, 74, 180);
    std::uint32_t pixels[16 * 16];
    std::fill_n(pixels, 16 * 16, 0xff154ab4);
    auto bitmap = CreateBitmap(16, 16, 1, 32, pixels);
    auto images = ImageList_Create(16, 16, ILC_COLOR32, 1, 0);
    Check(bitmap && images && ImageList_Add(images, bitmap, nullptr) == 0, "create scaled icon fixture");
    DeleteObject(bitmap);
    auto icon = ImageList_GetIcon(images, 0, ILD_TRANSPARENT);
    ImageList_Destroy(images);
    Check(icon != nullptr, "extract icon from a fixed-size image list");
    PanelCanvas canvas;
    for (int size : {20, 36}) {
        HDC dc = canvas.Prepare(48, 48);
        Check(dc && DrawIconEx(dc, 1, 2, icon, size, size, 0, nullptr, DI_NORMAL), "draw an icon at the panel DPI");
        Check(GetPixel(dc, size, size + 1) == color && GetPixel(dc, size + 1, size + 1) != color,
            "icon fills exactly the requested DPI-scaled area");
    }
    DestroyIcon(icon);
}

void TestPlacement() {
    RECT primary{0, 0, 3072, 1820};
    RECT secondary{3072, 0, 5632, 1390};
    RECT saved{1980, 108, 3015, 1143};
    POINT origin{primary.left, primary.top};
    auto fallback = placement::ScaleToWorkArea(saved, origin, 216, secondary, 120);
    RECT expectedFallback{4172, 60, 4747, 635};
    Check(EqualRect(&fallback, &expectedFallback), "missing 225 percent monitor falls back at 125 percent DPI");
    auto restored = placement::ScaleToWorkArea(saved, origin, 216, primary, 216);
    Check(EqualRect(&restored, &saved), "returning monitor restores the original size and position");

    RECT relocated{-3072, 60, 0, 1880};
    RECT expectedRelocated{-1092, 168, -57, 1203};
    auto moved = placement::ScaleToWorkArea(saved, origin, 216, relocated, 216);
    Check(EqualRect(&moved, &expectedRelocated), "monitor origin changes preserve the relative panel position");

    RECT smaller{0, 0, 800, 600};
    auto fitted = placement::ScaleToWorkArea(saved, origin, 216, smaller, 216);
    Check(EqualRect(&fitted, &smaller), "reduced work area keeps the panel on screen");
}

void TestSettings(const std::filesystem::path& root) {
    auto ini = root / L"DesktopJunction.ini";
    settings::Values original;
    original.source = {(root / L"微信=附件").wstring(), true};
    original.bounds = {-700, 48, -240, 580};
    original.dpi = 144;
    original.pinned = true;
    Check(settings::Write(ini.wstring(), original), "write portable INI");
    bool imported = true;
    auto loaded = settings::Load(ini.wstring(), imported);
    Check(!imported && loaded.source.path == original.source.path && loaded.source.monthly,
        "INI takes precedence and preserves the Unicode path and mapping mode");
    Check(EqualRect(&loaded.bounds, &original.bounds) && loaded.dpi == 144 && loaded.pinned &&
        loaded.monitor.empty() && !loaded.workOrigin, "legacy INI preserves bounds, DPI and pin");

    original.source.monthly = false;
    original.pinned = false;
    original.monitor = LR"(\\?\DISPLAY#MonitorA#PortA)";
    original.workOrigin = POINT{-2560, 48};
    Check(settings::Write(ini.wstring(), original), "replace existing INI");
    loaded = settings::Load(ini.wstring(), imported);
    Check(!loaded.source.monthly && !loaded.pinned, "INI reloads changed options");
    Check(loaded.monitor == original.monitor && loaded.workOrigin && loaded.workOrigin->x == -2560 &&
        loaded.workOrigin->y == 48, "INI preserves the monitor identity and its signed work area origin");
    Check(!std::filesystem::exists(ini.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp"),
        "save leaves no temporary file");
    Check(!settings::Write((root / L"missing" / ini.filename()).wstring(), original),
        "saving fails cleanly when the target directory is unavailable");
}

void TestStartupRegistration() {
    struct RegistryFixture {
        std::wstring path = L"Software\\DesktopJunction.StartupTest." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetTickCount64());
        ~RegistryFixture() { RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str()); }
    } fixture;
    const auto* key = fixture.path.c_str();
    HKEY temporary = nullptr;
    Check(RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_VOLATILE, KEY_SET_VALUE,
        nullptr, &temporary, nullptr) == ERROR_SUCCESS, "create isolated startup registration key");
    RegCloseKey(temporary);
    std::wstring exe = L"C:\\Program Files\\桌面映射\\DesktopJunction.exe";
    std::wstring other = L"D:\\Portable\\DesktopJunction.exe";
    Check(startup::IsEnabled(exe, key) == false, "startup defaults to unchecked");
    Check(startup::SetEnabled(exe, true, key) == ERROR_SUCCESS && startup::IsEnabled(exe, key) == true,
        "register and read back executable with spaces and Unicode");
    wchar_t command[261]{};
    DWORD bytes = sizeof(command);
    Check(RegGetValueW(HKEY_CURRENT_USER, key, L"DesktopJunction", RRF_RT_REG_SZ, nullptr, command, &bytes) ==
        ERROR_SUCCESS && command == L"\"" + exe + L"\"", "startup command quotes the absolute executable path");
    Check(startup::IsEnabled(other, key) == false && startup::SetEnabled(other, false, key) == ERROR_SUCCESS &&
        startup::IsEnabled(exe, key) == true, "a different executable does not unregister this copy");
    Check(startup::SetEnabled(other, true, key) == ERROR_SUCCESS && startup::IsEnabled(other, key) == true &&
        startup::IsEnabled(exe, key) == false, "re-enabling after relocation updates the startup command");
    Check(startup::SetEnabled(other, false, key) == ERROR_SUCCESS && startup::IsEnabled(other, key) == false,
        "unregister the startup value");
}

void TestFolderDrop(const std::filesystem::path& root) {
    auto source = root / L"incoming";
    auto oldMonth = root / L"2026-11";
    auto newMonth = root / L"2026-12";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(oldMonth);
    std::filesystem::create_directories(newMonth);
    auto file = source / L"拖回.txt";
    CreateFileWithTime(file, 1);
    auto childFolder = source / L"子目录";
    std::filesystem::create_directory(childFolder);
    CreateFileWithTime(childFolder / L"附件.txt", 2);

    auto freePidl = [](ITEMIDLIST* pidl) { CoTaskMemFree(pidl); };
    std::unique_ptr<ITEMIDLIST, decltype(freePidl)> parent(ILCreateFromPathW(source.c_str()));
    std::unique_ptr<ITEMIDLIST, decltype(freePidl)> item(ILCreateFromPathW(file.c_str()));
    std::unique_ptr<ITEMIDLIST, decltype(freePidl)> directoryItem(ILCreateFromPathW(childFolder.c_str()));
    Check(parent && item && directoryItem, "resolve native incoming drag paths");
    PCUITEMID_CHILD children[] = {ILFindLastID(item.get()), ILFindLastID(directoryItem.get())};
    auto createData = [&](UINT count) {
        IDataObject* object = nullptr;
        Check(SUCCEEDED(SHCreateDataObject(parent.get(), count, children, nullptr, IID_IDataObject,
            reinterpret_cast<void**>(&object))), "create Explorer-style drag data");
        return std::unique_ptr<IDataObject, decltype([](IDataObject* data) { data->Release(); })>(object);
    };

    auto destroyOwner = [](HWND owner) { DestroyWindow(owner); };
    std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroyOwner)> owner(CreateWindowExW(WS_EX_TOOLWINDOW,
        L"STATIC", L"", WS_POPUP, 0, 0, 120, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr));
    Check(owner != nullptr, "create incoming drop window");
    std::wstring mapped = oldMonth.wstring();
    bool busy = false;
    std::unique_ptr<IDropTarget, decltype([](IDropTarget* drop) { drop->Release(); })>
        drop(shell::CreateFolderDropTarget(owner.get(), mapped, busy));
    Check(drop && SUCCEEDED(RegisterDragDrop(owner.get(), drop.get())), "register incoming drop window with OLE");
    auto revoke = [](HWND window) { RevokeDragDrop(window); };
    std::unique_ptr<std::remove_pointer_t<HWND>, decltype(revoke)> registration(owner.get());

    constexpr DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    POINTL point{10, 10};
    auto data = createData(1);
    DWORD effect = allowed;
    busy = true;
    Check(SUCCEEDED(drop->DragEnter(data.get(), MK_LBUTTON, point, &effect)) && effect == DROPEFFECT_NONE,
        "ignore drags that originated in the panel");
    busy = false;
    effect = allowed;
    Check(SUCCEEDED(drop->DragEnter(data.get(), MK_LBUTTON | MK_CONTROL, point, &effect)) &&
        effect == DROPEFFECT_COPY, "Ctrl drag into the panel previews a copy");
    mapped = newMonth.wstring();
    effect = allowed;
    Check(SUCCEEDED(drop->Drop(data.get(), MK_CONTROL, point, &effect)) && effect == DROPEFFECT_COPY &&
        std::filesystem::exists(file) && std::filesystem::exists(newMonth / file.filename()) &&
        !std::filesystem::exists(oldMonth / file.filename()), "copy goes to the directory mapped at drop time");

    data = createData(2);
    mapped = oldMonth.wstring();
    effect = allowed;
    Check(SUCCEEDED(drop->DragEnter(data.get(), MK_LBUTTON, point, &effect)) && effect == DROPEFFECT_MOVE,
        "native same-volume default previews a move");
    effect = allowed;
    Check(SUCCEEDED(drop->Drop(data.get(), 0, point, &effect)) && std::filesystem::exists(oldMonth / file.filename()) &&
        std::filesystem::exists(oldMonth / childFolder.filename() / L"附件.txt") &&
        !std::filesystem::exists(file) && !std::filesystem::exists(childFolder),
        "native same-volume drop moves a file and directory together");
}

void TestReadOnlyTransfer(const std::filesystem::path& root) {
    auto source = root / L"transfer-source";
    auto target = root / L"transfer-target";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(target);
    auto name = L"只读附件.txt";
    auto input = source / name;
    auto output = target / name;
    CreateFileWithTime(input, 1);
    Check(SetFileAttributesW(input.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
        "mark fixture read-only");
    auto snapshot = folder::Read(source.wstring());
    Check(snapshot.entries.size() == 1 && folder::IsReadOnlyFile(snapshot.entries.front().attributes),
        "read-only badge uses enumeration attributes");

    auto copy = shell::TransferToFolder(nullptr, source.wstring(), {name}, target.wstring(), DROPEFFECT_COPY);
    Check(SUCCEEDED(copy.transfer) && SUCCEEDED(copy.unlock) && std::filesystem::exists(output),
        "copy read-only file through the desktop drop operation");
    auto copied = GetFileAttributesW(output.c_str());
    Check((copied & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE)) == FILE_ATTRIBUTE_ARCHIVE &&
        folder::IsReadOnlyFile(GetFileAttributesW(input.c_str())), "copy unlocks destination and preserves source");
    std::filesystem::remove(output);

    auto move = shell::TransferToFolder(nullptr, source.wstring(), {name}, target.wstring(), DROPEFFECT_MOVE);
    Check(SUCCEEDED(move.transfer) && SUCCEEDED(move.unlock) && std::filesystem::exists(output) &&
        !std::filesystem::exists(input), "move read-only file through the desktop drop operation");
    auto moved = GetFileAttributesW(output.c_str());
    Check((moved & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE)) == FILE_ATTRIBUTE_ARCHIVE,
        "move clears only the read-only bit");

    CreateFileWithTime(input, 2);
    Check(SetFileAttributesW(input.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
        "mark native drag fixture read-only");
    auto identity = folder::ReadOnlyIdentity(input.wstring());
    Check(identity.has_value(), "read native drag file identity");
    std::filesystem::remove(output);
    Check(CopyFileW(input.c_str(), output.c_str(), TRUE), "copy native drag fixture");
    Check(!folder::ClearReadOnlyAfterMove(input.wstring(), output.wstring(), *identity) &&
        folder::IsReadOnlyFile(GetFileAttributesW(output.c_str())), "native copy is not treated as a move");

    auto displaced = source / L"elsewhere.txt";
    Check(MoveFileW(input.c_str(), displaced.c_str()), "move fixture elsewhere");
    Check(!folder::ClearReadOnlyAfterMove(input.wstring(), output.wstring(), *identity) &&
        folder::IsReadOnlyFile(GetFileAttributesW(output.c_str())), "unrelated same-name destination stays read-only");
    Check(folder::ClearReadOnly(output.wstring()) == ERROR_SUCCESS, "release copied fixture");
    std::filesystem::remove(output);
    Check(MoveFileW(displaced.c_str(), output.c_str()), "move original fixture to destination");
    Check(folder::ClearReadOnlyAfterMove(input.wstring(), output.wstring(), *identity) == ERROR_SUCCESS &&
        !folder::IsReadOnlyFile(GetFileAttributesW(output.c_str())), "confirmed native move clears read-only");
}

void TestNativeDesktopCopies(const std::filesystem::path& root) {
    using namespace std::chrono_literals;
    auto source = root / L"native-copy-source";
    auto desktop = root / L"native-copy-desktop";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(desktop);
    const wchar_t* name = L"只读副本.txt";
    auto input = source / name;
    auto output = desktop / name;
    auto write = [](const auto& path, const char* contents) {
        std::ofstream file(path, std::ios::binary);
        file << contents;
        file.close();
        Check(bool(file) && SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
            "create read-only copy fixture");
    };
    auto readOnly = [&] { return folder::IsReadOnlyFile(GetFileAttributesW(output.c_str())); };
    auto waitForUnlock = [&] {
        auto deadline = std::chrono::steady_clock::now() + 5s;
        while (readOnly() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
        Check(!readOnly() && std::filesystem::exists(output), "verified native copy unlocks destination");
        Check(folder::IsReadOnlyFile(GetFileAttributesW(input.c_str())), "native copy preserves source read-only");
        std::filesystem::remove(output);
    };
    write(input, "real contents");
    {
        folder::DesktopCopies clipboard(source.wstring(), desktop.wstring(), {name}, {0}, false);
        clipboard.Activate();
        Check(CopyFileW(input.c_str(), output.c_str(), TRUE), "native clipboard copy");
        waitForUnlock();
    }
    {
        folder::DesktopCopies clipboard(source.wstring(), desktop.wstring(), {name}, {0}, false);
        clipboard.Activate();
        {
            std::ofstream file(output, std::ios::binary);
            file << "real contents";
        }
        std::this_thread::sleep_for(150ms);
        Check(SetFileAttributesW(output.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE),
            "native copy applies attributes after creating destination");
        waitForUnlock();
    }
    {
        folder::DesktopCopies drag(source.wstring(), desktop.wstring(), {name}, {0}, true);
        Check(CopyFileW(input.c_str(), output.c_str(), TRUE), "native drag copy");
        std::this_thread::sleep_for(100ms);
        Check(readOnly(), "drag observer waits for OLE copy completion");
        drag.Activate();
        waitForUnlock();
    }
    {
        folder::DesktopCopies clipboard(source.wstring(), desktop.wstring(), {name}, {0}, false);
        clipboard.Activate();
        write(output, "fake contents");
        std::this_thread::sleep_for(500ms);
        Check(readOnly(), "same-name unrelated contents remain read-only");
        Check(folder::ClearReadOnly(output.wstring()) == ERROR_SUCCESS, "release unrelated fixture");
        std::filesystem::remove(output);
    }
    {
        write(output, "real contents");
        folder::DesktopCopies clipboard(source.wstring(), desktop.wstring(), {name}, {0}, false);
        clipboard.Activate();
        Check(SetFileAttributesW(output.c_str(), FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN),
            "modify attributes of pre-existing destination");
        std::this_thread::sleep_for(500ms);
        Check(readOnly(), "pre-existing file with identical contents stays read-only");
    }
    Check(folder::ClearReadOnly(output.wstring()) == ERROR_SUCCESS &&
        folder::ClearReadOnly(input.wstring()) == ERROR_SUCCESS, "release native copy fixtures");
}
}

int main() {
    try {
        Fixture fixture;
        Test(fixture.path);
        TestResources(fixture.path);
        TestSelectionCompositing();
        TestRowSizing();
        TestIconScaling();
        TestPlacement();
        TestSettings(fixture.path);
        TestStartupRegistration();
        TestNativeDesktopCopies(fixture.path);
        HRESULT ole = OleInitialize(nullptr);
        Check(SUCCEEDED(ole), "initialize Shell transfer tests");
        try {
            TestFolderDrop(fixture.path);
            TestReadOnlyTransfer(fixture.path);
        } catch (...) {
            OleUninitialize();
            throw;
        }
        OleUninitialize();
        std::cout << "Folder, resource, settings, and transfer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
