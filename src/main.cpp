#include "desktop.h"
#include "folder.h"
#include "list_row_sizing.h"
#include "panel_canvas.h"
#include "panel_placement.h"
#include "resource.h"
#include "selection_style.h"
#include "settings.h"
#include "shell.h"
#include "startup.h"

#include <windowsx.h>
#include <oleacc.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
constexpr wchar_t appName[] = L"桌面文件夹映射";
constexpr wchar_t windowClass[] = L"DesktopJunction.Panel";
constexpr UINT trayMessage = WM_APP + 3;
constexpr UINT showMessage = WM_APP + 4;
constexpr UINT chooseMessage = WM_APP + 5;
constexpr UINT paintMessage = WM_APP + 6;
constexpr UINT_PTR monthTimer = 2;
constexpr UINT_PTR placementTimer = 3;
constexpr int pinId = 200, chooseId = 201, listId = 202, closeId = 203;
void DrawPill(HDC dc, const RECT& area, int radius, COLORREF fill) {
    auto brush = SelectObject(dc, GetStockObject(DC_BRUSH));
    auto pen = SelectObject(dc, GetStockObject(NULL_PEN));
    SetDCBrushColor(dc, fill);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, radius, radius);
    SelectObject(dc, pen);
    SelectObject(dc, brush);
}

void DrawDesktopLabel(HDC dc, const wchar_t* text, RECT area, COLORREF color, UINT flags) {
    auto shadow = area;
    OffsetRect(&shadow, 1, 1);
    SetBkMode(dc, TRANSPARENT);
    unsigned brightness = 299u * GetRValue(color) + 587u * GetGValue(color) + 114u * GetBValue(color);
    SetTextColor(dc, brightness >= 128000 ? RGB(0, 0, 0) : RGB(255, 255, 255));
    DrawTextW(dc, text, -1, &shadow, flags);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &area, flags);
}

bool ReadMonitor(HMONITOR monitor, MONITORINFOEXW& info) {
    info.cbSize = sizeof(info);
    return monitor && GetMonitorInfoW(monitor, &info) && !IsRectEmpty(&info.rcWork);
}

const wchar_t* MonitorIdentity(const MONITORINFOEXW& info, DISPLAY_DEVICEW& device) {
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(info.szDevice, 0, &device, EDD_GET_DEVICE_INTERFACE_NAME) && device.DeviceID[0]) {
        return device.DeviceID;
    }
    return info.szDevice;
}

HMONITOR FindMonitor(const settings::Values& settings) {
    if (settings.monitor.empty()) return MonitorFromRect(&settings.bounds, MONITOR_DEFAULTTONULL);
    struct Search {
        const wchar_t* identity;
        HMONITOR found = nullptr;
    } search{settings.monitor.c_str()};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        if (!ReadMonitor(monitor, info)) return TRUE;
        DISPLAY_DEVICEW device{};
        if (CompareStringOrdinal(search.identity, -1, MonitorIdentity(info, device), -1, TRUE) != CSTR_EQUAL) {
            return TRUE;
        }
        search.found = monitor;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

void RememberMonitor(settings::Values& settings, const MONITORINFOEXW& info) {
    DISPLAY_DEVICEW device{};
    settings.monitor = MonitorIdentity(info, device);
    settings.workOrigin = POINT{info.rcWork.left, info.rcWork.top};
}

RECT DefaultBounds() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    auto dpi = GetDpiForSystem();
    int offset = MulDiv(24, dpi, 96);
    auto x = work.right - offset;
    auto y = work.top + offset;
    RECT bounds{x - MulDiv(470, dpi, 96), y, x, y + MulDiv(460, dpi, 96)};
    return IsRectEmpty(&work) ? bounds : placement::Fit(bounds, work);
}

struct FileIcon {
    int index = 0;
    HICON scaledSource = nullptr;

    FileIcon() = default;
    FileIcon(const FileIcon&) = delete;
    FileIcon& operator=(const FileIcon&) = delete;
    ~FileIcon() { if (scaledSource) DestroyIcon(scaledSource); }
};

class Panel {
public:
    explicit Panel(HINSTANCE instance) : instance_(instance), settingsPath_(settings::FilePath()),
        settings_(settings::Load(settingsPath_, importedSettings_)) {}
    ~Panel() {
        if (window_) DestroyWindow(window_);
        if (trayIcon_) DestroyIcon(trayIcon_);
    }
    bool Create();
    void Start();

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ListProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data);
    static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Notify(const NMHDR& header);

    bool CreateControls();
    int Dip(int value) const { return MulDiv(value, dpi_, 96); }
    int HeaderHeight() const { return Dip(32); }
    int FileIconSize() const { return Dip(16); }
    LRESULT HitTest(POINT point) const;
    void PaintFrame();
    void RequestPaint();
    void DrawToolbar(HDC dc, const RECT& area);
    void DrawRows(HDC dc);
    void DrawScrollbar(HDC dc);
    void UpdateAppearance();
    void RefreshSelectionStyle();
    void Layout();
    void FitColumn();
    void UpdateFont();
    void UpdateRowSpacing();
    void UpdateDpi(UINT dpi);
    void Place(RECT bounds);
    bool RestorePlacement();
    void SchedulePlacement();
    void RememberPlacement();
    void Save(bool report = false);
    void AddTray();
    void TrayMenu();
    void Show(bool activate);
    void Choose();
    void RefreshSource(bool force = false);
    void ApplyPending();
    void SetListCount();
    void FillDisplayInfo(NMLVDISPINFOW& info);
    void DrawRow(HDC dc, int row);
    FileIcon& Icon(const folder::Entry& entry);
    std::vector<std::wstring> Selected(std::vector<size_t>* readOnly = nullptr) const;
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
    std::wstring settingsPath_;
    bool importedSettings_ = false;
    settings::Values settings_;
    HWND window_ = nullptr, list_ = nullptr, pin_ = nullptr, choose_ = nullptr, close_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    UINT taskbarCreated_ = 0;
    HFONT font_ = nullptr, iconFont_ = nullptr;
    HICON icon_ = nullptr;
    HICON trayIcon_ = nullptr;
    HIMAGELIST fileImages_ = nullptr;
    int fileImageWidth_ = 0, fileImageHeight_ = 0;
    ListRowSizing rowSizing_;
    COLORREF textColor_ = RGB(255, 255, 255);
    PanelCanvas canvas_;
    SelectionStyle selectionStyle_;
    std::vector<RECT> selectedRows_;
    RECT selectionViewport_{};
    bool highContrast_ = false;
    SIZE layoutSize_{};
    HWND hoveredButton_ = nullptr;
    bool trayAdded_ = false, busy_ = false, revealed_ = false, paintPending_ = false;
    bool moving_ = false, placing_ = false;
    RECT moveStart_{};
    int placementRetries_ = 0;
    unsigned generation_ = 0;
    std::wstring path_;
    folder::Snapshot snapshot_;
    std::unique_ptr<folder::Monitor> monitor_;
    std::unique_ptr<DesktopLayer> desktop_;
    std::unique_ptr<shell::Actions> actions_;
    std::unordered_map<std::wstring, FileIcon> icons_;
};

bool Panel::Create() {
    icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_DESKTOP_JUNCTION));
    if (!icon_) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    LoadIconMetric(instance_, MAKEINTRESOURCEW(IDI_DESKTOP_JUNCTION), LIM_SMALL, &trayIcon_);
    WNDCLASSW wc{};
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = windowClass;
    if (!RegisterClassW(&wc)) return false;
    RECT bounds = DefaultBounds();
    if (!IsRectEmpty(&settings_.bounds)) {
        auto monitor = FindMonitor(settings_);
        MONITORINFOEXW info{};
        if (!ReadMonitor(monitor, info)) {
            ReadMonitor(MonitorFromRect(&settings_.bounds, MONITOR_DEFAULTTONEAREST), info);
        }
        if (!IsRectEmpty(&info.rcWork)) bounds = placement::Fit(settings_.bounds, info.rcWork);
    }
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, windowClass, appName,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance_, this) != nullptr;
}

void Panel::Start() {
    UpdateAppearance();
    actions_ = std::make_unique<shell::Actions>(window_);
    monitor_ = std::make_unique<folder::Monitor>(window_);
    desktop_ = std::make_unique<DesktopLayer>(window_);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (IsRectEmpty(&settings_.bounds)) RememberPlacement();
    AddTray();
    RefreshSource();
    Show(false);
    if (importedSettings_) Save(true);
    if (settings_.source.path.empty()) PostMessageW(window_, chooseMessage, 0, 0);
}

bool Panel::CreateControls() {
    auto child = [&](const wchar_t* type, const wchar_t* title, DWORD style, int id) {
        return CreateWindowExW(0, type, title, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    };
    choose_ = child(WC_BUTTONW, L"设置", BS_PUSHBUTTON | WS_TABSTOP, chooseId);
    pin_ = child(WC_BUTTONW, L"置顶", BS_AUTOCHECKBOX | BS_PUSHLIKE | WS_TABSTOP, pinId);
    close_ = child(WC_BUTTONW, L"关闭", BS_PUSHBUTTON | WS_TABSTOP, closeId);
    list_ = child(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
        LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP, listId);
    if (!choose_ || !pin_ || !close_ || !list_) return false;
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(list_, CCM_SETUNICODEFORMAT, TRUE, 0);
    shell::SmallIcon(L"file", FILE_ATTRIBUTE_NORMAL, &fileImages_);
    if (fileImages_) {
        ImageList_GetIconSize(fileImages_, &fileImageWidth_, &fileImageHeight_);
        ListView_SetImageList(list_, fileImages_, LVSIL_SMALL);
    }
    LVCOLUMNW column{};
    column.mask = LVCF_WIDTH;
    ListView_InsertColumn(list_, 0, &column);
    SetWindowSubclass(list_, ListProc, 1, reinterpret_cast<DWORD_PTR>(this));
    HWND tips = CreateWindowExW(WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, window_, nullptr, instance_, nullptr);
    const std::pair<HWND, const wchar_t*> buttons[] = {
        {choose_, L"设置目录"}, {pin_, L"切换置顶"}, {close_, L"关闭"},
    };
    for (const auto& [button, description] : buttons) {
        SetWindowSubclass(button, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        if (tips) {
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = window_;
            tool.uId = reinterpret_cast<UINT_PTR>(button);
            tool.lpszText = const_cast<wchar_t*>(description);
            SendMessageW(tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    SendMessageW(pin_, BM_SETCHECK, settings_.pinned ? BST_CHECKED : BST_UNCHECKED, 0);
    dpi_ = GetDpiForWindow(window_);
    UpdateFont();
    UpdateRowSpacing();
    RefreshSelectionStyle();
    Layout();
    return true;
}

void Panel::UpdateFont() {
    LOGFONTW symbol{};
    symbol.lfHeight = -Dip(14);
    symbol.lfQuality = NONANTIALIASED_QUALITY;
    wcscpy_s(symbol.lfFaceName, L"Segoe MDL2 Assets");
    if (HFONT font = CreateFontIndirectW(&symbol)) {
        if (iconFont_) DeleteObject(iconFont_);
        iconFont_ = font;
    }
    LOGFONTW label{};
    if (!SystemParametersInfoForDpi(SPI_GETICONTITLELOGFONT, sizeof(label), &label, 0, dpi_)) return;
    // Solid glyph pixels keep the desktop color key out of the text edges.
    label.lfQuality = NONANTIALIASED_QUALITY;
    HFONT font = CreateFontIndirectW(&label);
    if (!font) return;
    for (HWND control : {choose_, pin_, close_, list_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    if (font_) DeleteObject(font_);
    font_ = font;
}

void Panel::UpdateRowSpacing() {
    rowSizing_.Update(list_, fileImages_, FileIconSize(), Dip(6), Dip(6));
}

void Panel::UpdateAppearance() {
    COLORREF color = DesktopLayer::TextColor();
    if (std::exchange(textColor_, color) != color) RequestPaint();
}

void Panel::RefreshSelectionStyle() {
    HIGHCONTRASTW contrast{sizeof(contrast), 0, nullptr};
    highContrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    selectionStyle_.Reset();
    RequestPaint();
}

LRESULT Panel::HitTest(POINT point) const {
    ScreenToClient(window_, &point);
    RECT area{};
    GetClientRect(window_, &area);
    if (!PtInRect(&area, point)) return HTNOWHERE;
    int grip = Dip(5);
    bool left = point.x < grip;
    bool right = point.x >= area.right - grip;
    if (point.y < grip) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
    if (point.y >= area.bottom - grip) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (point.y < HeaderHeight() && ChildWindowFromPointEx(window_, point, CWP_SKIPINVISIBLE) == window_) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

void Panel::PaintFrame() {
    if (!list_ || !IsWindowVisible(window_) || IsIconic(window_)) return;
    RECT area{};
    GetClientRect(window_, &area);
    HDC dc = canvas_.Prepare(area.right, area.bottom);
    if (!dc) return;
    FrameRect(dc, &area, GetSysColorBrush(COLOR_3DSHADOW));
    DrawToolbar(dc, area);
    selectedRows_.clear();
    DrawRows(dc);
    DrawScrollbar(dc);
    canvas_.NormalizeAlpha();
    bool focused = GetFocus() == list_ && GetForegroundWindow() == window_;
    for (const auto& row : selectedRows_) {
        auto pixels = selectionStyle_.Pixels(list_, row.right - row.left, row.bottom - row.top,
            dpi_, focused, highContrast_);
        canvas_.CompositeBackground(row, selectionViewport_, pixels);
    }
    canvas_.Present(window_);
}

void Panel::RequestPaint() {
    if (!paintPending_ && IsWindowVisible(window_)) {
        paintPending_ = PostMessageW(window_, paintMessage, 0, 0) != FALSE;
    }
}

void Panel::DrawToolbar(HDC dc, const RECT& area) {
    RECT title{Dip(10), 0, area.right - Dip(104), HeaderHeight()};
    HGDIOBJ old = font_ ? SelectObject(dc, font_) : nullptr;
    const wchar_t* label = snapshot_.error ? L"目录暂不可用" :
        settings_.source.monthly ? L"微信附件" : L"桌面文件夹";
    DrawDesktopLabel(dc, label, title, textColor_,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    for (HWND button : {choose_, pin_, close_}) {
        RECT bounds{};
        GetWindowRect(button, &bounds);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
        bool pinned = button == pin_ && settings_.pinned;
        bool pressed = (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
        bool hover = button == hoveredButton_;
        if (pressed || hover || pinned) {
            COLORREF fill = button == close_ ? RGB(155, 49, 56) : pinned ? RGB(37, 87, 130) : RGB(49, 54, 62);
            DrawPill(dc, bounds, Dip(8), fill);
        }
        if (GetFocus() == button) DrawFocusRect(dc, &bounds);
        const wchar_t* text = L"×";
        HGDIOBJ previous = nullptr;
        if (button != close_) {
            text = button == pin_ ? L"置顶" : L"设置";
            if (iconFont_) {
                previous = SelectObject(dc, iconFont_);
                if (previous) text = button == choose_ ? L"\uE713" : pinned ? L"\uE77A" : L"\uE718";
            }
        }
        DrawDesktopLabel(dc, text, bounds, (pressed || hover || pinned) ? RGB(255, 255, 255) : textColor_,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (previous) SelectObject(dc, previous);
    }
    if (old) SelectObject(dc, old);
}

void Panel::DrawRows(HDC dc) {
    RECT client{};
    GetClientRect(list_, &client);
    POINT origin{};
    MapWindowPoints(list_, window_, &origin, 1);
    selectionViewport_ = client;
    OffsetRect(&selectionViewport_, origin.x, origin.y);
    int saved = SaveDC(dc);
    if (!saved) return;
    SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
    IntersectClipRect(dc, client.left, client.top, client.right, client.bottom);
    int first = std::max(0, ListView_GetTopIndex(list_));
    int last = std::min(static_cast<int>(snapshot_.entries.size()), first + ListView_GetCountPerPage(list_) + 1);
    for (int row = first; row < last; ++row) DrawRow(dc, row);
    RestoreDC(dc, saved);
}

void Panel::DrawScrollbar(HDC dc) {
    SCROLLBARINFO info{};
    info.cbSize = sizeof(info);
    if (!GetScrollBarInfo(list_, OBJID_VSCROLL, &info) ||
        (info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN | STATE_SYSTEM_UNAVAILABLE)) ||
        info.xyThumbBottom <= info.xyThumbTop) return;
    auto track = info.rcScrollBar;
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&track), 2);
    int width = std::max(2, Dip(4));
    LONG left = (track.left + track.right - width) / 2;
    RECT thumb{left, track.top + info.xyThumbTop, left + width, track.top + info.xyThumbBottom};
    FrameRect(dc, &thumb, GetSysColorBrush(COLOR_3DSHADOW));
    InflateRect(&thumb, -1, -1);
    DrawPill(dc, thumb, width, RGB(210, 215, 222));
}

void Panel::Layout() {
    if (!list_ || IsIconic(window_)) return;
    RECT area{};
    GetClientRect(window_, &area);
    layoutSize_ = {area.right, area.bottom};
    int padding = Dip(8);
    int buttonHeight = Dip(22);
    int buttonTop = (HeaderHeight() - buttonHeight) / 2;
    int width = std::max(0, int(area.right) - 2 * padding);
    int right = area.right - padding;
    MoveWindow(close_, right - Dip(24), buttonTop, Dip(24), buttonHeight, TRUE);
    MoveWindow(pin_, right - Dip(57), buttonTop, Dip(30), buttonHeight, TRUE);
    MoveWindow(choose_, right - Dip(90), buttonTop, Dip(30), buttonHeight, TRUE);
    int top = HeaderHeight();
    MoveWindow(list_, padding, top, width, std::max(0, int(area.bottom) - top - padding), TRUE);
    FitColumn();
    RequestPaint();
}

void Panel::FitColumn() {
    RECT client{};
    GetClientRect(list_, &client);
    int width = std::max(0L, client.right - client.left);
    if (ListView_GetColumnWidth(list_, 0) != width) ListView_SetColumnWidth(list_, 0, width);
}

void Panel::Place(RECT bounds) {
    RECT current{};
    if (GetWindowRect(window_, &current) && EqualRect(&current, &bounds)) return;
    placing_ = true;
    SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
        bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    placing_ = false;
}

void Panel::UpdateDpi(UINT dpi) {
    if (!dpi || dpi_ == dpi) return;
    dpi_ = dpi;
    if (!list_) return;
    selectionStyle_.Reset();
    UpdateFont();
    UpdateRowSpacing();
    Layout();
}

bool Panel::RestorePlacement() {
    if (moving_ || placing_ || IsIconic(window_) || IsRectEmpty(&settings_.bounds)) return true;
    auto target = FindMonitor(settings_);
    bool preferredAvailable = target != nullptr;
    if (!target) target = MonitorFromRect(&settings_.bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW info{};
    if (!ReadMonitor(target, info)) return false;
    if (preferredAvailable && settings_.monitor.empty()) {
        RememberMonitor(settings_, info);
        Save();
    }

    // Move onto the target before asking for its effective per-window DPI.
    if (MonitorFromWindow(window_, MONITOR_DEFAULTTONULL) != target) {
        Place(placement::Fit(settings_.bounds, info.rcWork));
    }
    if (MonitorFromWindow(window_, MONITOR_DEFAULTTONULL) != target) return false;
    UpdateDpi(GetDpiForWindow(window_));
    auto origin = settings_.workOrigin.value_or(POINT{info.rcWork.left, info.rcWork.top});
    Place(placement::ScaleToWorkArea(settings_.bounds, origin, settings_.dpi, info.rcWork, dpi_));
    return preferredAvailable;
}

void Panel::SchedulePlacement() {
    placementRetries_ = 2;
    SetTimer(window_, placementTimer, 300, nullptr);
}

void Panel::RememberPlacement() {
    RECT bounds{};
    if (!GetWindowRect(window_, &bounds)) return;
    MONITORINFOEXW info{};
    if (!ReadMonitor(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), info)) return;
    bounds = placement::Fit(bounds, info.rcWork);
    Place(bounds);
    UpdateDpi(GetDpiForWindow(window_));
    settings_.bounds = bounds;
    settings_.dpi = dpi_;
    RememberMonitor(settings_, info);
}

void Panel::Save(bool report) {
    if (!settings::Write(settingsPath_, settings_) && report) {
        MessageBoxW(window_, L"无法写入 EXE 同目录的 DesktopJunction.ini。请确认目录可写。",
            appName, MB_OK | MB_ICONWARNING);
    }
}

void Panel::AddTray() {
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window_;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = trayMessage;
    tray.hIcon = trayIcon_ ? trayIcon_ : icon_;
    wcscpy_s(tray.szTip, appName);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
}

void Panel::TrayMenu() {
    if (busy_) return;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    constexpr UINT show = 1, choose = 2, exit = 3, autoStartCommand = 4;
    auto executable = settings::ExecutablePath();
    auto autoStart = startup::IsEnabled(executable);
    AppendMenuW(menu, MF_STRING, show, L"显示面板");
    AppendMenuW(menu, MF_STRING, choose, L"设置目录…");
    AppendMenuW(menu, MF_STRING | (!autoStart ? MF_GRAYED : *autoStart ? MF_CHECKED : MF_UNCHECKED),
        autoStartCommand, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, exit, L"退出");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(window_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, window_, nullptr);
    PostMessageW(window_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command == show) Show(true);
    if (command == choose) Choose();
    if (command == autoStartCommand && autoStart) {
        auto status = startup::SetEnabled(executable, !*autoStart);
        if (status != ERROR_SUCCESS) {
            auto message = L"无法更新开机自启设置。Windows 错误 " + std::to_wstring(status);
            MessageBoxW(window_, message.c_str(), appName, MB_OK | MB_ICONWARNING);
        }
    }
    if (command == exit) DestroyWindow(window_);
}

void Panel::Show(bool activate) {
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (!RestorePlacement()) SchedulePlacement();
    RequestPaint();
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
        monitor_->SetPath(path_, ++generation_, settings_.source.monthly);
        auto title = path_.empty() ? std::wstring(appName) : std::wstring(appName) + L"  ·  " + path_;
        SetWindowTextW(window_, title.c_str());
        RequestPaint();
    }
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
    RequestPaint();
}

void Panel::SetListCount() {
    ListView_SetItemCountEx(list_, static_cast<int>(snapshot_.entries.size()), LVSICF_NOINVALIDATEALL);
    FitColumn();
}

FileIcon& Panel::Icon(const folder::Entry& entry) {
    bool directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto dot = entry.name.find_last_of(L'.');
    auto extension = directory ? L"*" : dot == std::wstring::npos ? L"" : entry.name.substr(dot);
    auto found = icons_.find(extension);
    if (found != icons_.end()) return found->second;
    if (icons_.size() >= 256 && !directory) extension.clear();
    auto [item, inserted] = icons_.try_emplace(extension);
    if (inserted) item->second.index = shell::SmallIcon((L"file" + extension).c_str(),
        directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL);
    return item->second;
}

void Panel::FillDisplayInfo(NMLVDISPINFOW& info) {
    LVITEMW& item = info.item;
    if (item.iItem < 0 || size_t(item.iItem) >= snapshot_.entries.size()) return;
    const auto& entry = snapshot_.entries[item.iItem];
    if (item.mask & LVIF_IMAGE) item.iImage = rowSizing_.HasPlaceholder() ? 0 : Icon(entry).index;
    if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0) {
        lstrcpynW(item.pszText, entry.name.c_str(), item.cchTextMax);
    }
}

void Panel::DrawRow(HDC dc, int row) {
    const auto& entry = snapshot_.entries[row];
    auto state = ListView_GetItemState(list_, row, LVIS_SELECTED | LVIS_FOCUSED);
    bool selected = (state & LVIS_SELECTED) != 0;
    RECT label{}, icon{};
    ListView_GetItemRect(list_, row, &label, LVIR_LABEL);
    ListView_GetItemRect(list_, row, &icon, LVIR_ICON);
    auto images = fileImages_;
    if (images) {
        icon.top += rowSizing_.IconOffset();
        int size = rowSizing_.HasPlaceholder() ? rowSizing_.IconSize() : fileImageWidth_;
        auto& fileIcon = Icon(entry);
        bool drawn;
        if (size == fileImageWidth_ && size == fileImageHeight_) {
            drawn = ImageList_Draw(images, fileIcon.index, dc, icon.left, icon.top, ILD_TRANSPARENT) != FALSE;
        } else {
            if (!fileIcon.scaledSource) fileIcon.scaledSource = ImageList_GetIcon(images, fileIcon.index, ILD_TRANSPARENT);
            drawn = fileIcon.scaledSource &&
                DrawIconEx(dc, icon.left, icon.top, fileIcon.scaledSource, size, size, 0, nullptr, DI_NORMAL);
        }
        if (drawn && folder::IsReadOnlyFile(entry.attributes)) {
            LONG x = icon.left + size - Dip(10), y = icon.top + size - Dip(11);
            auto pen = SelectObject(dc, GetStockObject(DC_PEN));
            auto brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            SetDCPenColor(dc, RGB(31, 37, 44));
            RoundRect(dc, x + Dip(2), y, x + Dip(8), y + Dip(8), Dip(4), Dip(4));
            SetDCPenColor(dc, RGB(255, 228, 152));
            RoundRect(dc, x + Dip(3), y + Dip(1), x + Dip(7), y + Dip(8), Dip(3), Dip(3));
            SelectObject(dc, brush);
            SelectObject(dc, pen);
            DrawPill(dc, {x, y + Dip(4), x + Dip(10), y + Dip(11)}, Dip(3), RGB(31, 37, 44));
            DrawPill(dc, {x + Dip(1), y + Dip(5), x + Dip(9), y + Dip(10)}, Dip(2), RGB(255, 228, 152));
            SetPixelV(dc, x + Dip(5), y + Dip(7), RGB(31, 37, 44));
        }
    }
    HGDIOBJ old = font_ ? SelectObject(dc, font_) : nullptr;
    DrawDesktopLabel(dc, entry.name.c_str(), label, selected && highContrast_ ? GetSysColor(COLOR_HIGHLIGHTTEXT) : textColor_,
        DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (old) SelectObject(dc, old);
    if (!selected && !(GetFocus() == list_ && (state & LVIS_FOCUSED))) return;
    RECT bounds{};
    if (!ListView_GetItemRect(list_, row, &bounds, LVIR_BOUNDS)) return;
    if (selected) {
        LONG gap = Dip(1);
        selectedRows_.push_back({selectionViewport_.left + Dip(2), selectionViewport_.top + bounds.top + gap,
            selectionViewport_.right - Dip(2), selectionViewport_.top + bounds.bottom - gap});
    } else {
        DrawFocusRect(dc, &bounds);
    }
}

std::vector<std::wstring> Panel::Selected(std::vector<size_t>* readOnly) const {
    std::vector<std::wstring> names;
    for (int i = -1; (i = ListView_GetNextItem(list_, i, LVNI_SELECTED)) >= 0;) {
        if (size_t(i) >= snapshot_.entries.size()) continue;
        const auto& entry = snapshot_.entries[i];
        if (readOnly && folder::IsReadOnlyFile(entry.attributes)) readOnly->push_back(names.size());
        names.push_back(entry.name);
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
    std::vector<size_t> readOnly;
    auto names = Selected(key == 'C' ? &readOnly : nullptr);
    auto path = snapshot_.path;
    if (!names.empty()) Action([&] {
        switch (key) {
        case VK_RETURN: shell::Open(window_, folder::Join(path, names.front())); break;
        case VK_DELETE: actions_->Delete(path, names); break;
        case VK_F2: if (names.size() == 1) actions_->Rename(path, names.front()); break;
        case 'C': actions_->Copy(path, names, false, readOnly); break;
        case 'X': actions_->Copy(path, names, true); break;
        }
    });
    return true;
}

LRESULT Panel::Notify(const NMHDR& header) {
    if (header.hwndFrom != list_) return 0;
    if (header.code == LVN_ITEMCHANGED || header.code == LVN_ODSTATECHANGED) RequestPaint();
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
        std::vector<size_t> readOnly;
        auto names = Selected(&readOnly);
        auto path = snapshot_.path;
        if (!names.empty()) Action([&] { actions_->Drag(path, names, readOnly); });
    }
    return 0;
}

LRESULT CALLBACK Panel::ListProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& panel = *reinterpret_cast<Panel*>(data);
    if (message == WM_KEYDOWN && panel.Key(wParam)) return 0;
    if (message == WM_CHAR && wParam == VK_RETURN) return 0;
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    auto result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (message == WM_VSCROLL || message == WM_MOUSEWHEEL || message == WM_KEYDOWN ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        panel.RequestPaint();
    }
    return result;
}

LRESULT CALLBACK Panel::ButtonProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& panel = *reinterpret_cast<Panel*>(data);
    if (message == WM_MOUSEMOVE && panel.hoveredButton_ != hwnd) {
        panel.hoveredButton_ = hwnd;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tracking);
        panel.RequestPaint();
    } else if (message == WM_MOUSELEAVE && panel.hoveredButton_ == hwnd) {
        panel.hoveredButton_ = nullptr;
        panel.RequestPaint();
    }
    if (message == WM_ERASEBKGND) return TRUE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    auto result = DefSubclassProc(hwnd, message, wParam, lParam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == BM_SETSTATE || message == BM_SETCHECK) {
        panel.RequestPaint();
    }
    return result;
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
        UpdateAppearance();
        PostMessageW(window_, DesktopLayer::changed, TRUE, 0);
        return 0;
    }
    switch (message) {
    case WM_CREATE: return CreateControls() ? 0 : -1;
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return HitTest({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window_, &paint);
        EndPaint(window_, &paint);
        return 0;
    }
    case paintMessage:
        paintPending_ = false;
        PaintFrame();
        return 0;
    case WM_ERASEBKGND: return TRUE;
    case WM_SHOWWINDOW:
        if (!wParam) {
            canvas_.Reset();
            selectionStyle_.Reset();
        }
        break;
    case WM_GETMINMAXINFO: {
        auto& limits = *reinterpret_cast<MINMAXINFO*>(lParam);
        limits.ptMinTrackSize = {Dip(280), Dip(180)};
        return 0;
    }
    case WM_SIZE:
        if (layoutSize_.cx != LOWORD(lParam) || layoutSize_.cy != HIWORD(lParam)) Layout();
        if (wParam == SIZE_MINIMIZED) PostMessageW(window_, DesktopLayer::changed, TRUE, 0);
        return 0;
    case WM_DPICHANGED:
        UpdateDpi(HIWORD(wParam));
        if (!placing_) {
            if (moving_) Place(*reinterpret_cast<const RECT*>(lParam));
            else SchedulePlacement();
        }
        return 0;
    case WM_DISPLAYCHANGE:
        SchedulePlacement();
        return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETICONTITLELOGFONT) {
            UpdateFont();
            UpdateRowSpacing();
            Layout();
        }
        if (!wParam || wParam == SPI_SETDESKWALLPAPER || wParam == SPI_SETHIGHCONTRAST) UpdateAppearance();
        if (wParam == SPI_SETHIGHCONTRAST) RefreshSelectionStyle();
        if (wParam == SPI_SETWORKAREA) SchedulePlacement();
        if (lParam && !wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"intl")) RefreshSource();
        break;
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        UpdateAppearance();
        RefreshSelectionStyle();
        break;
    case WM_ENTERSIZEMOVE:
        moving_ = GetWindowRect(window_, &moveStart_) != FALSE;
        return 0;
    case WM_EXITSIZEMOVE: {
        RECT bounds{};
        if (std::exchange(moving_, false) && GetWindowRect(window_, &bounds) && !EqualRect(&moveStart_, &bounds)) {
            RememberPlacement();
            Save();
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) revealed_ = false;
        RequestPaint();
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
        } else if (wParam == placementTimer) {
            KillTimer(window_, placementTimer);
            if (!RestorePlacement() && placementRetries_ > 0) {
                --placementRetries_;
                SetTimer(window_, placementTimer, 2000, nullptr);
            }
        }
        return 0;
    case WM_TIMECHANGE:
        RefreshSource();
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            SchedulePlacement();
            RefreshSource();
        }
        return TRUE;
    case folder::Monitor::ready:
        ApplyPending();
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (actions_) actions_->ClipboardChanged();
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
            RequestPaint();
            revealed_ = false;
            desktop_->Update(settings_.pinned);
            Save(true);
        } else if (LOWORD(wParam) == chooseId && HIWORD(wParam) == BN_CLICKED) {
            Choose();
        } else if (LOWORD(wParam) == closeId && HIWORD(wParam) == BN_CLICKED) {
            PostMessageW(window_, WM_CLOSE, 0, 0);
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
    case WM_NCDESTROY: {
        HWND window = std::exchange(window_, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (font_) DeleteObject(font_);
        if (iconFont_) DeleteObject(iconFont_);
        return DefWindowProcW(window, message, wParam, lParam);
    }
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
