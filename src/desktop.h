#pragma once

#include <windows.h>

class DesktopLayer {
public:
    static constexpr UINT changed = WM_APP + 1;
    static constexpr UINT_PTR settleTimer = 1;

    explicit DesktopLayer(HWND panel);
    ~DesktopLayer();
    DesktopLayer(const DesktopLayer&) = delete;
    DesktopLayer& operator=(const DesktopLayer&) = delete;

    void Update(bool pinned);
    void Settle();

private:
    HWND panel_;
    HWND reference_ = nullptr;
    DWORD shellProcess_ = 0;
    HWINEVENTHOOK foreground_ = nullptr;
    HWINEVENTHOOK minimize_ = nullptr;
    HWINEVENTHOOK shellOrder_ = nullptr;
    bool showingDesktop_ = false;
};
