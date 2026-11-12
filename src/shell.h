#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace folder { class DesktopCopies; }

namespace shell {

struct FolderChoice {
    std::wstring path;
    bool monthly;
};

struct TransferResult {
    HRESULT transfer = S_OK;
    HRESULT unlock = S_OK;
};

std::optional<FolderChoice> ChooseFolder(HWND owner, const std::wstring& initial, bool monthly);
void Open(HWND owner, const std::wstring& path);
int SmallIcon(const wchar_t* name, DWORD attributes, HIMAGELIST* imageList = nullptr);
TransferResult TransferToFolder(HWND owner, const std::wstring& directory,
    const std::vector<std::wstring>& names, const std::wstring& destination, DWORD effect);
inline constexpr UINT dropHoverChanged = WM_APP + 7;
IDropTarget* CreateFolderDropTarget(HWND owner, const std::wstring& directory, bool& busy);

class Actions {
public:
    explicit Actions(HWND owner) : owner_(owner) {}
    ~Actions();
    void Menu(const std::wstring& directory, const std::vector<std::wstring>& names, POINT point);
    void Locate(const std::wstring& directory, const std::vector<std::wstring>& names);
    void Copy(const std::wstring& directory, const std::vector<std::wstring>& names, bool cut,
        const std::vector<size_t>& readOnly = {});
    void ClipboardChanged();
    void Drag(const std::wstring& directory, const std::vector<std::wstring>& names,
        const std::vector<size_t>& readOnly);
    void Delete(const std::wstring& directory, const std::vector<std::wstring>& names);
    void Rename(const std::wstring& directory, const std::wstring& name);
    bool MenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    HWND owner_;
    IContextMenu* menu_ = nullptr;
    IDataObject* clipboardObject_ = nullptr;
    std::shared_ptr<folder::DesktopCopies> clipboardCopies_, dragCopies_;
    void ResetClipboardWatch();
};
}
