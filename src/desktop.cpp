#include "desktop.h"

#include <commctrl.h>
#include <cwchar>

namespace {
HWND eventTarget;

bool HasClass(HWND window, const wchar_t* name) {
    wchar_t actual[64]{};
    return GetClassNameW(window, actual, ARRAYSIZE(actual)) && !wcscmp(actual, name);
}

BOOL CALLBACK FindIconHost(HWND window, LPARAM result) {
    if (!HasClass(window, L"Progman") && !HasClass(window, L"WorkerW")) return TRUE;
    if (!FindWindowExW(window, nullptr, L"SHELLDLL_DefView", nullptr)) return TRUE;
    *reinterpret_cast<HWND*>(result) = window;
    return FALSE;
}

bool IsTopmost(HWND window) {
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void CALLBACK OnDesktopEvent(HWINEVENTHOOK, DWORD, HWND, LONG object, LONG child, DWORD, DWORD) {
    if (eventTarget && object == OBJID_WINDOW && child == CHILDID_SELF) {
        PostMessageW(eventTarget, DesktopLayer::changed, TRUE, 0);
    }
}

void CALLBACK OnShellOrder(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    if (eventTarget && (HasClass(window, L"Progman") || HasClass(window, L"WorkerW") ||
        HasClass(window, L"SHELLDLL_DefView"))) {
        PostMessageW(eventTarget, DesktopLayer::changed, TRUE, 0);
    }
}
}

DesktopLayer::DesktopLayer(HWND panel) : panel_(panel) {
    eventTarget = panel;
    reference_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (reference_) SetWindowPos(reference_, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    foreground_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, OnDesktopEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    minimize_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        nullptr, OnDesktopEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

DesktopLayer::~DesktopLayer() {
    eventTarget = nullptr;
    if (foreground_) UnhookWinEvent(foreground_);
    if (minimize_) UnhookWinEvent(minimize_);
    if (shellOrder_) UnhookWinEvent(shellOrder_);
    if (reference_) DestroyWindow(reference_);
}

void DesktopLayer::Settle() {
    // Explorer may finish rearranging its windows after sending the foreground event.
    SetTimer(panel_, settleTimer, 160, nullptr);
}

COLORREF DesktopLayer::TextColor() {
    HWND host = nullptr;
    EnumWindows(FindIconHost, reinterpret_cast<LPARAM>(&host));
    if (host) {
        HWND view = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        HWND icons = view ? FindWindowExW(view, nullptr, WC_LISTVIEWW, nullptr) : nullptr;
        DWORD_PTR color = 0;
        if (icons && SendMessageTimeoutW(icons, LVM_GETTEXTCOLOR, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &color) && color <= 0xFFFFFF) {
            return static_cast<COLORREF>(color);
        }
    }
    return RGB(255, 255, 255);
}

void DesktopLayer::Update(bool pinned) {
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    HWND foreground = GetForegroundWindow();
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(foreground, &foregroundProcess);
    HWND shell = GetShellWindow();
    DWORD shellProcess = 0;
    if (shell) GetWindowThreadProcessId(shell, &shellProcess);
    if (shellProcess_ != shellProcess) {
        if (shellOrder_) UnhookWinEvent(shellOrder_);
        shellProcess_ = shellProcess;
        shellOrder_ = shellProcess ? SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER,
            nullptr, OnShellOrder, shellProcess, 0, WINEVENT_OUTOFCONTEXT) : nullptr;
    }
    HWND iconHost = nullptr;
    EnumWindows(FindIconHost, reinterpret_cast<LPARAM>(&iconHost));
    HWND desktop = iconHost ? iconHost : shell;
    bool shellRaised = false;
    if (reference_ && desktop && IsWindowVisible(desktop)) {
        for (HWND window = GetWindow(desktop, GW_HWNDNEXT); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == reference_) {
                shellRaised = true;
                break;
            }
        }
    }
    bool shellForeground = foreground && (foreground == shell || foreground == iconHost ||
        HasClass(foreground, L"Shell_TrayWnd") || HasClass(foreground, L"Shell_SecondaryTrayWnd"));
    if (shellRaised && (shellForeground || !foreground || foregroundProcess == GetCurrentProcessId())) {
        showingDesktop_ = true;
    } else if (!shellRaised || (foreground && !shellForeground && foregroundProcess != GetCurrentProcessId())) {
        showingDesktop_ = false;
    }

    // Use topmost temporarily while Explorer raises the desktop above normal windows.
    if (pinned || showingDesktop_) {
        if (IsIconic(panel_)) ShowWindow(panel_, SW_SHOWNOACTIVATE);
        if (!IsTopmost(panel_)) SetWindowPos(panel_, HWND_TOPMOST, 0, 0, 0, 0, flags);
        return;
    }

    if (IsTopmost(panel_)) SetWindowPos(panel_, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    if (!desktop) {
        SetWindowPos(panel_, HWND_BOTTOM, 0, 0, 0, 0, flags);
        return;
    }
    HWND previous = GetWindow(desktop, GW_HWNDPREV);
    if (previous != panel_) {
        HWND after = previous && !IsTopmost(previous) ? previous : HWND_TOP;
        SetWindowPos(panel_, after, 0, 0, 0, 0, flags);
    }
}
