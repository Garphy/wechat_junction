#include "shell.h"

#include "folder.h"
#include "resource.h"

#include <memory>
#include <utility>

namespace shell {
namespace {
template<class T>
class Com {
public:
    Com() = default;
    ~Com() { if (value_) value_->Release(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** Out() { return &value_; }
    T* Get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
private:
    T* value_ = nullptr;
};

struct TaskFree {
    void operator()(void* p) const { CoTaskMemFree(p); }
};
using Pidl = std::unique_ptr<ITEMIDLIST, TaskFree>;

Pidl Parse(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return {};
    return Pidl(pidl);
}

void Error(HWND owner, const wchar_t* message, HRESULT code) {
    wchar_t status[16]{};
    swprintf_s(status, L"0x%08lX", static_cast<unsigned long>(code));
    MessageBoxW(owner, (std::wstring(message) + L"\n" + status).c_str(), L"桌面文件夹映射",
        MB_OK | MB_ICONWARNING);
}

class Selection {
public:
    Selection(const std::wstring& directory, const std::vector<std::wstring>& names)
        : parent(Parse(directory)) {
        if (!parent) return;
        for (const auto& name : names) {
            auto pidl = Parse(folder::Join(directory, name));
            if (!pidl) {
                children.clear();
                return;
            }
            children.push_back(ILFindLastID(pidl.get()));
            items_.push_back(std::move(pidl));
        }
    }

    template<class T>
    HRESULT Object(HWND owner, REFIID iid, T** out) {
        if (!parent || children.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        Com<IShellFolder> desktop;
        HRESULT hr = SHGetDesktopFolder(desktop.Out());
        if (FAILED(hr)) return hr;
        if (ILIsEmpty(parent.get())) {
            return desktop->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(),
                iid, nullptr, reinterpret_cast<void**>(out));
        }
        Com<IShellFolder> folder;
        hr = desktop->BindToObject(parent.get(), nullptr, IID_IShellFolder,
            reinterpret_cast<void**>(folder.Out()));
        if (FAILED(hr)) return hr;
        return folder->GetUIObjectOf(owner, static_cast<UINT>(children.size()), children.data(),
            iid, nullptr, reinterpret_cast<void**>(out));
    }

    Pidl parent;
    std::vector<PCUITEMID_CHILD> children;
private:
    std::vector<Pidl> items_;
};

class DragSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropSource) return E_NOINTERFACE;
        *result = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape || (keys & MK_RBUTTON)) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
private:
    ULONG references_ = 1;
};

INT_PTR CALLBACK RenameDialog(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_INITDIALOG) {
        auto& name = *reinterpret_cast<std::wstring*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        HWND edit = GetDlgItem(dialog, IDC_NEW_NAME);
        SetWindowTextW(edit, name.c_str());
        SendMessageW(edit, EM_SETLIMITTEXT, 255, 0);
        SetFocus(edit);
        auto dot = name.find_last_of(L'.');
        SendMessageW(edit, EM_SETSEL, 0, dot && dot != std::wstring::npos ? dot : -1);
        return FALSE;
    }
    if (message != WM_COMMAND) return FALSE;
    if (LOWORD(wParam) == IDOK) {
        wchar_t name[256]{};
        GetDlgItemTextW(dialog, IDC_NEW_NAME, name, ARRAYSIZE(name));
        std::wstring value(name);
        if (value.empty() || value == L"." || value == L".." || value.back() == L'.' ||
            value.back() == L' ' || value.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
            MessageBoxW(dialog, L"请输入有效的文件或文件夹名称。", L"重命名", MB_OK | MB_ICONWARNING);
            return TRUE;
        }
        *reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER)) = std::move(value);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (LOWORD(wParam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

HRESULT FileOperation(HWND owner, DWORD flags, Com<IFileOperation>& operation) {
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
        IID_IFileOperation, reinterpret_cast<void**>(operation.Out()));
    if (SUCCEEDED(hr)) hr = operation->SetOwnerWindow(owner);
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(flags);
    return hr;
}
}

std::optional<FolderChoice> ChooseFolder(HWND owner, const std::wstring& initial, bool monthly) {
    constexpr DWORD monthlyCheck = 1;
    Com<IFileDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_IFileDialog, reinterpret_cast<void**>(dialog.Out()));
    if (FAILED(hr)) {
        Error(owner, L"无法打开目录选择器。", hr);
        return std::nullopt;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    dialog->SetTitle(L"选择要映射的目录");
    dialog->SetOkButtonLabel(L"映射");
    Com<IFileDialogCustomize> customize;
    hr = dialog->QueryInterface(IID_IFileDialogCustomize, reinterpret_cast<void**>(customize.Out()));
    if (SUCCEEDED(hr)) hr = customize->AddCheckButton(monthlyCheck,
        L"自动切换微信月份（请选择 yyyy-MM 目录）", monthly);
    if (FAILED(hr)) {
        Error(owner, L"无法加载目录选择器的月份选项。", hr);
        return std::nullopt;
    }
    if (!initial.empty()) {
        Com<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_IShellItem,
            reinterpret_cast<void**>(item.Out())))) dialog->SetFolder(item.Get());
    }
    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    Com<IShellItem> result;
    if (SUCCEEDED(hr)) hr = dialog->GetResult(result.Out());
    PWSTR path = nullptr;
    if (SUCCEEDED(hr)) hr = result->GetDisplayName(SIGDN_FILESYSPATH, &path);
    std::unique_ptr<wchar_t, TaskFree> pathStorage(path);
    BOOL checked = FALSE;
    if (SUCCEEDED(hr)) hr = customize->GetCheckButtonState(monthlyCheck, &checked);
    if (FAILED(hr)) {
        Error(owner, L"无法读取所选目录。", hr);
        return std::nullopt;
    }
    return FolderChoice{path, checked != FALSE};
}

void Open(HWND owner, const std::wstring& path) {
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpFile = path.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) Error(owner, L"无法打开所选项目。", HRESULT_FROM_WIN32(GetLastError()));
}

int SmallIcon(const wchar_t* name, DWORD attributes, HIMAGELIST* imageList) {
    SHFILEINFOW info{};
    auto list = SHGetFileInfoW(name, attributes, &info, sizeof(info),
        SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (imageList) *imageList = reinterpret_cast<HIMAGELIST>(list);
    return list ? info.iIcon : 0;
}

void Actions::Menu(const std::wstring& directory, const std::vector<std::wstring>& names, POINT point) {
    constexpr UINT locate = 1, rename = 2, firstVerb = 100, lastVerb = 0x7fff;
    Selection selection(directory, names);
    Com<IContextMenu> context;
    HRESULT hr = selection.Object(owner_, IID_IContextMenu, context.Out());
    if (FAILED(hr)) {
        Error(owner_, L"无法获取所选项目的文件菜单。", hr);
        return;
    }
    HMENU popup = CreatePopupMenu();
    if (!popup) return;
    AppendMenuW(popup, MF_STRING, locate, L"在资源管理器中定位");
    if (names.size() == 1) AppendMenuW(popup, MF_STRING, rename, L"重命名");
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
    UINT flags = CMF_NORMAL | ((GetKeyState(VK_SHIFT) < 0) ? CMF_EXTENDEDVERBS : 0);
    hr = context->QueryContextMenu(popup, static_cast<UINT>(GetMenuItemCount(popup)), firstVerb, lastVerb, flags);
    menu_ = SUCCEEDED(hr) ? context.Get() : nullptr;
    UINT command = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, owner_, nullptr);
    menu_ = nullptr;
    DestroyMenu(popup);
    if (command == locate) {
        Locate(directory, names);
    } else if (command == rename) {
        Rename(directory, names.front());
    } else if (command >= firstVerb && command <= lastVerb && SUCCEEDED(hr)) {
        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = owner_;
        invoke.lpVerb = MAKEINTRESOURCEA(command - firstVerb);
        invoke.lpVerbW = MAKEINTRESOURCEW(command - firstVerb);
        invoke.lpDirectoryW = directory.c_str();
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = point;
        hr = context->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&invoke));
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) Error(owner_, L"文件操作未完成。", hr);
    }
}

bool Actions::MenuMessage(UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result) {
    if (!menu_) return false;
    Com<IContextMenu3> third;
    if (SUCCEEDED(menu_->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(third.Out())))) {
        return third->HandleMenuMsg2(message, wParam, lParam, &result) == S_OK;
    }
    Com<IContextMenu2> second;
    if (message != WM_MENUCHAR &&
        SUCCEEDED(menu_->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(second.Out())))) {
        result = 0;
        return second->HandleMenuMsg(message, wParam, lParam) == S_OK;
    }
    return false;
}

void Actions::Locate(const std::wstring& directory, const std::vector<std::wstring>& names) {
    Selection selection(directory, names);
    HRESULT hr = selection.parent && selection.children.size() == names.size()
        ? SHOpenFolderAndSelectItems(selection.parent.get(), static_cast<UINT>(selection.children.size()),
            selection.children.data(), 0)
        : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (FAILED(hr)) Error(owner_, L"无法在资源管理器中定位。", hr);
}

void Actions::Copy(const std::wstring& directory, const std::vector<std::wstring>& names, bool cut) {
    Selection selection(directory, names);
    Com<IDataObject> data;
    HRESULT hr = selection.Object(owner_, IID_IDataObject, data.Out());
    if (SUCCEEDED(hr)) {
        auto memory = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        auto format = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        auto* effect = memory ? static_cast<DWORD*>(GlobalLock(memory)) : nullptr;
        if (effect && format) {
            *effect = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(memory);
            FORMATETC descriptor{static_cast<CLIPFORMAT>(format), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            medium.tymed = TYMED_HGLOBAL;
            medium.hGlobal = memory;
            hr = data->SetData(&descriptor, &medium, TRUE);
            if (FAILED(hr)) GlobalFree(memory);
        } else {
            if (effect) GlobalUnlock(memory);
            if (memory) GlobalFree(memory);
            hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) hr = OleSetClipboard(data.Get());
    if (FAILED(hr)) Error(owner_, L"无法将所选项目放入剪贴板。", hr);
}

void Actions::Drag(const std::wstring& directory, const std::vector<std::wstring>& names) {
    Selection selection(directory, names);
    Com<IDataObject> data;
    HRESULT hr = selection.Object(owner_, IID_IDataObject, data.Out());
    if (SUCCEEDED(hr)) {
        auto* source = new DragSource;
        DWORD effect = DROPEFFECT_NONE;
        hr = DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        source->Release();
    }
    if (FAILED(hr)) Error(owner_, L"无法拖出所选项目。", hr);
}

void Actions::Delete(const std::wstring& directory, const std::vector<std::wstring>& names) {
    Com<IFileOperation> operation;
    HRESULT hr = FileOperation(owner_, FOF_ALLOWUNDO | FOF_WANTNUKEWARNING | FOFX_RECYCLEONDELETE, operation);
    for (const auto& name : names) {
        if (FAILED(hr)) break;
        Com<IShellItem> item;
        hr = SHCreateItemFromParsingName(folder::Join(directory, name).c_str(), nullptr, IID_IShellItem,
            reinterpret_cast<void**>(item.Out()));
        if (SUCCEEDED(hr)) hr = operation->DeleteItem(item.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) Error(owner_, L"删除未完成。", hr);
}

void Actions::Rename(const std::wstring& directory, const std::wstring& name) {
    auto updated = name;
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_RENAME), owner_, RenameDialog,
        reinterpret_cast<LPARAM>(&updated)) != IDOK || name == updated) return;
    Com<IFileOperation> operation;
    HRESULT hr = FileOperation(owner_, FOF_ALLOWUNDO, operation);
    Com<IShellItem> item;
    if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(folder::Join(directory, name).c_str(), nullptr,
        IID_IShellItem, reinterpret_cast<void**>(item.Out()));
    if (SUCCEEDED(hr)) hr = operation->RenameItem(item.Get(), updated.c_str(), nullptr);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) Error(owner_, L"重命名未完成。", hr);
}
}
