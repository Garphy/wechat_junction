#include "desktop.h"
#include "folder.h"
#include "shell.h"

#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
constexpr wchar_t appName[] = L"桌面文件夹映射";
constexpr wchar_t windowClass[] = L"DesktopJunction.Panel";
constexpr wchar_t settingsKey[] = L"Software\\DesktopJunction";
constexpr UINT trayMessage = WM_APP + 3;
constexpr UINT showMessage = WM_APP + 4;
constexpr UINT chooseMessage = WM_APP + 5;
constexpr UINT_PTR monthTimer = 2;
constexpr int pinId = 200, chooseId = 201, listId = 202;

struct Settings {
    folder::Source source;
    RECT bounds{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool pinned = false;
};

struct StoredSettings {
    DWORD version;
    DWORD flags;
    RECT bounds;
    UINT dpi;
};

Settings LoadSettings() {
    Settings result;
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"Settings", RRF_RT_REG_BINARY,
        nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(StoredSettings) ||
        size > sizeof(StoredSettings) + 32768 * sizeof(wchar_t)) return result;
    std::vector<BYTE> bytes(size);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"Settings", RRF_RT_REG_BINARY,
        nullptr, bytes.data(), &size) != ERROR_SUCCESS || size < sizeof(StoredSettings) ||
        (size - sizeof(StoredSettings)) % sizeof(wchar_t)) return result;
    StoredSettings stored{};
    std::memcpy(&stored, bytes.data(), sizeof(stored));
    if (stored.version != 1) return result;
    std::wstring path((size - sizeof(stored)) / sizeof(wchar_t), L'\0');
    std::memcpy(path.data(), bytes.data() + sizeof(stored), size - sizeof(stored));
    if (path.find(L'\0') == std::wstring::npos) {
        if (auto source = folder::MakeSource(path, false)) result.source = std::move(*source);
    }
    result.source.monthly = (stored.flags & 1) != 0 && !result.source.path.empty();
    result.pinned = (stored.flags & 2) != 0;
    auto width = static_cast<long long>(stored.bounds.right) - stored.bounds.left;
    auto height = static_cast<long long>(stored.bounds.bottom) - stored.bounds.top;
    if (width > 0 && height > 0 && width < 65536 && height < 65536 &&
        stored.dpi >= 48 && stored.dpi <= 768) {
        result.bounds = stored.bounds;
        result.dpi = stored.dpi;
    }
    return result;
}

bool WriteSettings(const Settings& settings) {
    StoredSettings stored{1, (settings.source.monthly ? 1u : 0u) | (settings.pinned ? 2u : 0u),
        settings.bounds, settings.dpi};
    std::vector<BYTE> bytes(sizeof(stored) + settings.source.path.size() * sizeof(wchar_t));
    std::memcpy(bytes.data(), &stored, sizeof(stored));
    std::memcpy(bytes.data() + sizeof(stored), settings.source.path.data(), bytes.size() - sizeof(stored));
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settingsKey, 0, nullptr, 0, KEY_SET_VALUE,
        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    auto status = RegSetValueExW(key, L"Settings", 0, REG_BINARY, bytes.data(), static_cast<DWORD>(bytes.size()));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

RECT FitToScreen(RECT bounds) {
    HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return bounds;
    auto work = info.rcWork;
    auto width = std::min(bounds.right - bounds.left, work.right - work.left);
    auto height = std::min(bounds.bottom - bounds.top, work.bottom - work.top);
    auto x = std::clamp(bounds.left, work.left, work.right - width);
    auto y = std::clamp(bounds.top, work.top, work.bottom - height);
    return {x, y, x + width, y + height};
}

RECT DefaultBounds() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    auto dpi = GetDpiForSystem();
    int offset = MulDiv(24, dpi, 96);
    auto x = work.right - offset;
    auto y = work.top + offset;
    return FitToScreen({x - MulDiv(470, dpi, 96), y, x, y + MulDiv(460, dpi, 96)});
}

class Panel {
public:
    explicit Panel(HINSTANCE instance) : instance_(instance), settings_(LoadSettings()) {}
    bool Create();
    void Start();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Notify(const NMHDR& header);

    bool CreateControls();
    int Dip(int value) const { return MulDiv(value, dpi_, 96); }
    void Layout();
    void UpdateFont();
    void Place(RECT bounds);
    void FitCurrent();
    void Save(bool report = false);
    void AddTray();
    void TrayMenu();
    void Show(bool activate);
    void Choose();
    void RefreshSource(bool force = false);
    void ApplyPending();
    void UpdateStatus();
    void SetListCount();
    void FillDisplayInfo(NMLVDISPINFOW& info);
    int Icon(const folder::Entry& entry);
    std::vector<std::wstring> Selected() const;
    void Context(int row, POINT point);
    void KeyboardContext();
    bool Key(WPARAM key);

    template<class Function>
    void Action(Function action) {
        if (busy_) return;
        busy_ = true;
        action();
        busy_ = false;
        ApplyPending();
    }

    HINSTANCE instance_;
    Settings settings_;
    HWND window_ = nullptr, list_ = nullptr, pin_ = nullptr, choose_ = nullptr, status_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT taskbarCreated_ = 0;
    HFONT font_ = nullptr;
    HICON icon_ = nullptr;
    bool trayAdded_ = false, busy_ = false, revealed_ = false, loading_ = false;
    unsigned generation_ = 0;
    std::wstring path_;
    folder::Snapshot snapshot_;
    std::unique_ptr<folder::Monitor> monitor_;
    std::unique_ptr<DesktopLayer> desktop_;
    std::unique_ptr<shell::Actions> actions_;
    std::unordered_map<std::wstring, int> icons_;
};

bool Panel::Create() {
    icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    WNDCLASSW wc{};
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = windowClass;
    if (!RegisterClassW(&wc)) return false;
    auto bounds = IsRectEmpty(&settings_.bounds) ? DefaultBounds() : FitToScreen(settings_.bounds);
    return CreateWindowExW(WS_EX_TOOLWINDOW, windowClass, appName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance_, this) != nullptr;
}

void Panel::Start() {
    actions_ = std::make_unique<shell::Actions>(window_);
    monitor_ = std::make_unique<folder::Monitor>(window_);
    desktop_ = std::make_unique<DesktopLayer>(window_);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    dpi_ = GetDpiForWindow(window_);
    if (!IsRectEmpty(&settings_.bounds) && settings_.dpi != dpi_) {
        RECT bounds = settings_.bounds;
        bounds.right = bounds.left + MulDiv(bounds.right - bounds.left, dpi_, settings_.dpi);
        bounds.bottom = bounds.top + MulDiv(bounds.bottom - bounds.top, dpi_, settings_.dpi);
        Place(FitToScreen(bounds));
    }
    AddTray();
    RefreshSource();
    Show(false);
    if (settings_.source.path.empty()) PostMessageW(window_, chooseMessage, 0, 0);
}

bool Panel::CreateControls() {
    auto child = [&](const wchar_t* type, const wchar_t* title, DWORD style, int id) {
        return CreateWindowExW(0, type, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    choose_ = child(WC_BUTTONW, L"设置目录…", BS_PUSHBUTTON | WS_TABSTOP, chooseId);
    pin_ = child(WC_BUTTONW, L"置顶", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, pinId);
    status_ = child(WC_STATICW, L"", SS_LEFTNOWORDWRAP | SS_NOPREFIX | SS_ENDELLIPSIS, 0);
    list_ = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
        LVS_NOSORTHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP, listId);
    if (!choose_ || !pin_ || !status_ || !list_) return false;
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(list_, CCM_SETUNICODEFORMAT, TRUE, 0);
    HIMAGELIST images = nullptr;
    shell::SmallIcon(L"file", FILE_ATTRIBUTE_NORMAL, &images);
    if (images) ListView_SetImageList(list_, images, LVSIL_SMALL);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t*>(L"名称");
    ListView_InsertColumn(list_, 0, &column);
    column.pszText = const_cast<wchar_t*>(L"修改时间");
    ListView_InsertColumn(list_, 1, &column);
    SetWindowSubclass(list_, ListProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(pin_, BM_SETCHECK, settings_.pinned ? BST_CHECKED : BST_UNCHECKED, 0);
    dpi_ = GetDpiForWindow(window_);
    UpdateFont();
    Layout();
    return true;
}

void Panel::UpdateFont() {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi_)) return;
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font) return;
    for (HWND control : {choose_, pin_, status_, list_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (font_) DeleteObject(font_);
    font_ = font;
}

void Panel::Layout() {
    if (!list_ || IsIconic(window_)) return;
    RECT area{};
    GetClientRect(window_, &area);
    int padding = Dip(8);
    int toolbar = Dip(29);
    int footer = Dip(23);
    int width = std::max(0, int(area.right) - 2 * padding);
    MoveWindow(choose_, padding, padding, Dip(92), toolbar, TRUE);
    MoveWindow(pin_, area.right - padding - Dip(64), padding, Dip(64), toolbar, TRUE);
    int top = 2 * padding + toolbar;
    MoveWindow(list_, padding, top, width, std::max(0, int(area.bottom) - top - footer), TRUE);
    MoveWindow(status_, padding, area.bottom - footer + Dip(4), width, footer, TRUE);
    int dateWidth = Dip(143);
    ListView_SetColumnWidth(list_, 0, std::max(Dip(60), width - dateWidth - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_)));
    ListView_SetColumnWidth(list_, 1, dateWidth);
}

void Panel::Place(RECT bounds) {
    SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Panel::FitCurrent() {
    if (IsIconic(window_)) return;
    RECT bounds{};
    if (!GetWindowRect(window_, &bounds)) return;
    auto fitted = FitToScreen(bounds);
    if (!EqualRect(&bounds, &fitted)) Place(fitted);
}

void Panel::Save(bool report) {
    if (!IsIconic(window_)) {
        GetWindowRect(window_, &settings_.bounds);
        settings_.dpi = dpi_;
    }
    if (!WriteSettings(settings_) && report) {
        MessageBoxW(window_, L"设置已在本次运行生效，但无法保存到当前用户。", appName, MB_OK | MB_ICONWARNING);
    }
}

void Panel::AddTray() {
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window_;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = trayMessage;
    tray.hIcon = icon_;
    wcscpy_s(tray.szTip, appName);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
}

void Panel::TrayMenu() {
    if (busy_) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, 1, L"显示面板");
    AppendMenuW(menu, MF_STRING, 2, L"设置目录…");
    AppendMenuW(menu, MF_STRING, 3, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == 1) Show(true);
    if (command == 2) Choose();
    if (command == 3) DestroyWindow(window_);
}

void Panel::Show(bool activate) {
    FitCurrent();
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (activate) {
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
        revealed_ = SetForegroundWindow(window_) || GetForegroundWindow() == window_;
        if (revealed_) SetFocus(list_);
    }
    if (!revealed_) desktop_->Update(settings_.pinned);
}

void Panel::Choose() {
    if (!monitor_) return;
    Action([&] {
        auto initial = path_;
        for (;;) {
            auto choice = shell::ChooseFolder(window_, initial, settings_.source.monthly);
            if (!choice) return;
            auto source = folder::MakeSource(choice->path, choice->monthly);
            if (!source) {
                MessageBoxW(window_, L"自动换月需要选择名称形如 yyyy-MM 的月份目录，例如 2026-11。",
                    appName, MB_OK | MB_ICONWARNING);
                initial = choice->path;
                continue;
            }
            settings_.source = std::move(*source);
            Save(true);
            RefreshSource(true);
            return;
        }
    });
}

void Panel::RefreshSource(bool force) {
    if (!monitor_) return;
    KillTimer(window_, monthTimer);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (settings_.source.monthly) SetTimer(window_, monthTimer, folder::MillisecondsUntilMonth(now), nullptr);
    auto path = folder::Resolve(settings_.source, now);
    if (path != path_ || force) {
        if (path != path_) {
            snapshot_ = {};
            ListView_SetItemCount(list_, 0);
        }
        path_ = std::move(path);
        loading_ = !path_.empty();
        monitor_->SetPath(path_, ++generation_);
    }
    UpdateStatus();
}

void Panel::ApplyPending() {
    if (busy_ || !monitor_) return;
    auto next = monitor_->Take();
    if (!next || next->generation != generation_) return;
    auto selected = Selected();
    std::unordered_set<std::wstring> selection(selected.begin(), selected.end());
    int focused = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    std::wstring focusName;
    if (focused >= 0 && size_t(focused) < snapshot_.entries.size()) focusName = snapshot_.entries[focused].name;

    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    snapshot_ = std::move(*next);
    SetListCount();
    for (int i = 0; i < static_cast<int>(snapshot_.entries.size()) && (!selection.empty() || !focusName.empty()); ++i) {
        const auto& name = snapshot_.entries[i].name;
        if (selection.erase(name)) ListView_SetItemState(list_, i, LVIS_SELECTED, LVIS_SELECTED);
        if (name == focusName) {
            ListView_SetItemState(list_, i, LVIS_FOCUSED, LVIS_FOCUSED);
            focusName.clear();
        }
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, FALSE);
    loading_ = false;
    UpdateStatus();
}

void Panel::UpdateStatus() {
    std::wstring status;
    if (path_.empty()) {
        status = L"请设置要映射的目录";
    } else if (loading_) {
        status = L"正在读取  ";
    } else if (snapshot_.error == ERROR_SUCCESS) {
        status = std::to_wstring(snapshot_.entries.size()) + L" 项  ·  ";
    } else if (settings_.source.monthly &&
        (snapshot_.error == ERROR_FILE_NOT_FOUND || snapshot_.error == ERROR_PATH_NOT_FOUND)) {
        status = L"等待本月目录  ·  ";
    } else {
        status = L"目录不可用（" + std::to_wstring(snapshot_.error) + L"）  ·  ";
    }
    status += path_;
    SetWindowTextW(status_, status.c_str());
    auto title = path_.empty() ? std::wstring(appName) : std::wstring(appName) + L"  ·  " + path_;
    SetWindowTextW(window_, title.c_str());
}

void Panel::SetListCount() {
    ListView_SetItemCountEx(list_, static_cast<int>(snapshot_.entries.size()), LVSICF_NOINVALIDATEALL);
}

int Panel::Icon(const folder::Entry& entry) {
    bool directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto dot = entry.name.find_last_of(L'.');
    auto extension = directory ? L"*" : dot == std::wstring::npos ? L"" : entry.name.substr(dot);
    auto found = icons_.find(extension);
    if (found != icons_.end()) return found->second;
    if (icons_.size() >= 256 && !directory) extension.clear();
    auto [item, inserted] = icons_.try_emplace(extension);
    if (inserted) item->second = shell::SmallIcon((L"file" + extension).c_str(),
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    return item->second;
}

void Panel::FillDisplayInfo(NMLVDISPINFOW& info) {
    LVITEMW& item = info.item;
    if (item.iItem < 0 || size_t(item.iItem) >= snapshot_.entries.size()) return;
    const auto& entry = snapshot_.entries[item.iItem];
    if ((item.mask & LVIF_IMAGE) && item.iSubItem == 0) item.iImage = Icon(entry);
    if (!(item.mask & LVIF_TEXT) || !item.pszText || item.cchTextMax <= 0) return;
    if (item.iSubItem == 0) {
        lstrcpynW(item.pszText, entry.name.c_str(), item.cchTextMax);
    } else {
        SYSTEMTIME utc{}, local{};
        wchar_t date[64]{}, time[32]{};
        if (FileTimeToSystemTime(&entry.modified, &utc) && SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &local)) {
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, ARRAYSIZE(date), nullptr);
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, time, ARRAYSIZE(time));
        }
        lstrcpynW(item.pszText, (std::wstring(date) + L" " + time).c_str(), item.cchTextMax);
    }
}

std::vector<std::wstring> Panel::Selected() const {
    std::vector<std::wstring> names;
    for (int i = -1; (i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0;) {
        if (size_t(i) < snapshot_.entries.size()) names.push_back(snapshot_.entries[i].name);
    }
    return names;
}

void Panel::Context(int row, POINT point) {
    if (row < 0 || busy_) return;
    if (!(ListView_GetItemState(list_, row, LVIS_SELECTED) & LVIS_SELECTED)) {
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    auto names = Selected();
    auto path = snapshot_.path;
    if (!names.empty()) Action([&] { actions_->Menu(path, names, point); });
}

void Panel::KeyboardContext() {
    int row = ListView_GetNextItem(list_, -1, LVNI_FOCUSED);
    if (row < 0) row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    RECT bounds{};
    if (row >= 0 && ListView_GetItemRect(list_, row, &bounds, LVIR_LABEL)) {
        POINT point{bounds.left, bounds.bottom};
        ClientToScreen(list_, &point);
        Context(row, point);
    }
}

bool Panel::Key(WPARAM key) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool shift = GetKeyState(VK_SHIFT) < 0;
    if (key == VK_F5) {
        RefreshSource(true);
        return true;
    }
    if (key == VK_APPS || (key == VK_F10 && shift)) {
        KeyboardContext();
        return true;
    }
    if (ctrl && key == 'A') {
        ListView_SetItemState(list_, -1, LVIS_SELECTED, LVIS_SELECTED);
        return true;
    }
    if (key != VK_RETURN && key != VK_DELETE && key != VK_F2 && !(ctrl && (key == 'C' || key == 'X'))) return false;
    auto names = Selected();
    auto path = snapshot_.path;
    if (!names.empty()) Action([&] {
        switch (key) {
        case VK_RETURN: shell::Open(window_, folder::Join(path, names.front())); break;
        case VK_DELETE: actions_->Delete(path, names); break;
        case VK_F2: if (names.size() == 1) actions_->Rename(path, names.front()); break;
        case 'C': actions_->Copy(path, names, false); break;
        case 'X': actions_->Copy(path, names, true); break;
        }
    });
    return true;
}

LRESULT Panel::Notify(const NMHDR& header) {
    if (header.hwndFrom != list_) return 0;
    if (header.code == LVN_GETDISPINFOW) {
        FillDisplayInfo(*reinterpret_cast<NMLVDISPINFOW*>(const_cast<NMHDR*>(&header)));
    } else if (header.code == NM_DBLCLK) {
        auto row = reinterpret_cast<const NMITEMACTIVATE&>(header).iItem;
        if (row >= 0 && size_t(row) < snapshot_.entries.size()) {
            auto path = folder::Join(snapshot_.path, snapshot_.entries[row].name);
            Action([&] { shell::Open(window_, path); });
        }
    } else if (header.code == NM_RCLICK) {
        const auto& click = reinterpret_cast<const NMITEMACTIVATE&>(header);
        POINT point = click.ptAction;
        ClientToScreen(list_, &point);
        Context(click.iItem, point);
        return TRUE;
    } else if (header.code == LVN_BEGINDRAG) {
        auto names = Selected();
        auto path = snapshot_.path;
        if (!names.empty()) Action([&] { actions_->Drag(path, names); });
    }
    return 0;
}

LRESULT CALLBACK Panel::ListProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& panel = *reinterpret_cast<Panel*>(data);
    if (message == WM_KEYDOWN && panel.Key(wParam)) return 0;
    if (message == WM_CHAR && wParam == VK_RETURN) return 0;
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK Panel::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        panel->window_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    auto panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return panel ? panel->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT Panel::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreated_ && message == taskbarCreated_) {
        AddTray();
        PostMessageW(window_, DesktopLayer::changed, TRUE, 0);
        return 0;
    }
    switch (message) {
    case WM_CREATE: return CreateControls() ? 0 : -1;
    case WM_GETMINMAXINFO: {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
        limits.ptMinTrackSize = {Dip(280), Dip(180)};
        return 0;
    }
    case WM_SIZE:
        Layout();
        if (wParam == SIZE_MINIMIZED) PostMessageW(window_, DesktopLayer::changed, TRUE, 0);
        return 0;
    case WM_DPICHANGED:
        dpi_ = HIWORD(wParam);
        UpdateFont();
        Place(FitToScreen(*reinterpret_cast<RECT*>(lParam)));
        Layout();
        return 0;
    case WM_DISPLAYCHANGE:
        FitCurrent();
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) FitCurrent();
        if (lParam && !wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"intl")) RefreshSource();
        break;
    case WM_EXITSIZEMOVE:
        FitCurrent();
        Save();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) revealed_ = false;
        PostMessageW(window_, DesktopLayer::changed, 0, 0);
        break;
    case DesktopLayer::changed:
        if (desktop_ && IsWindowVisible(window_)) {
            if (!revealed_ || settings_.pinned) desktop_->Update(settings_.pinned);
            if (wParam) desktop_->Settle();
        }
        return 0;
    case WM_TIMER:
        if (wParam == DesktopLayer::settleTimer) {
            KillTimer(window_, DesktopLayer::settleTimer);
            if (IsWindowVisible(window_) && !revealed_) desktop_->Update(settings_.pinned);
        } else if (wParam == monthTimer) {
            RefreshSource();
        }
        return 0;
    case WM_TIMECHANGE:
        RefreshSource();
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) RefreshSource();
        return TRUE;
    case folder::Monitor::ready:
        ApplyPending();
        return 0;
    case WM_NOTIFY:
        return Notify(*reinterpret_cast<const NMHDR*>(lParam));
    case WM_INITMENUPOPUP:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_MENUCHAR: {
        LRESULT result = 0;
        if (actions_ && actions_->MenuMessage(message, wParam, lParam, result)) return result;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == pinId && HIWORD(wParam) == BN_CLICKED && desktop_) {
            settings_.pinned = SendMessageW(pin_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            revealed_ = false;
            desktop_->Update(settings_.pinned);
            Save(true);
        } else if (LOWORD(wParam) == chooseId && HIWORD(wParam) == BN_CLICKED) {
            Choose();
        }
        return 0;
    case trayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) TrayMenu();
        if (lParam == WM_LBUTTONDBLCLK) Show(true);
        return 0;
    case showMessage:
        Show(true);
        return 0;
    case chooseMessage:
        Choose();
        return 0;
    case WM_CLOSE:
        if (busy_) return 0;
        Save();
        if (trayAdded_) ShowWindow(window_, SW_HIDE);
        else DestroyWindow(window_);
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) Save();
        return 0;
    case WM_DESTROY: {
        if (monitor_) Save();
        desktop_.reset();
        monitor_.reset();
        if (trayAdded_) {
            NOTIFYICONDATAW tray{};
            tray.cbSize = sizeof(tray);
            tray.hWnd = window_;
            tray.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &tray);
        }
        PostQuitMessage(0);
        return 0;
    }
    case WM_NCDESTROY:
        if (font_) DeleteObject(font_);
        SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\DesktopJunction.Panel");
    if (!single) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND panel = FindWindowW(windowClass, nullptr)) {
            DWORD process = 0;
            GetWindowThreadProcessId(panel, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(panel, showMessage, 0, 0);
        }
        CloseHandle(single);
        return 0;
    }
    HRESULT ole = OleInitialize(nullptr);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    int result = 1;
    if (SUCCEEDED(ole)) {
        try {
            Panel panel(instance);
            if (InitCommonControlsEx(&controls) && panel.Create()) {
                panel.Start();
                MSG message{};
                BOOL received;
                while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                result = received < 0 ? 1 : static_cast<int>(message.wParam);
            }
        } catch (const std::exception&) {
            result = 1;
        }
        OleFlushClipboard();
        OleUninitialize();
    }
    CloseHandle(single);
    if (result) MessageBoxW(nullptr, L"无法启动桌面文件夹映射。", appName, MB_OK | MB_ICONERROR);
    return result;
}
