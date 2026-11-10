#pragma once

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <optional>
#include <string>
#include <vector>

namespace shell {

struct FolderChoice {
    std::wstring path;
    bool monthly;
};

std::optional<FolderChoice> ChooseFolder(HWND owner, const std::wstring& initial, bool monthly);
void Open(HWND owner, const std::wstring& path);
int SmallIcon(const wchar_t* name, DWORD attributes, HIMAGELIST* imageList = nullptr);

class Actions {
public:
    explicit Actions(HWND owner) : owner_(owner) {}
    void Menu(const std::wstring& directory, const std::vector<std::wstring>& names, POINT point);
    void Locate(const std::wstring& directory, const std::vector<std::wstring>& names);
    void Copy(const std::wstring& directory, const std::vector<std::wstring>& names, bool cut);
    void Drag(const std::wstring& directory, const std::vector<std::wstring>& names);
    void Delete(const std::wstring& directory, const std::vector<std::wstring>& names);
    void Rename(const std::wstring& directory, const std::wstring& name);
    bool MenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    HWND owner_;
    IContextMenu* menu_ = nullptr;
};
}
