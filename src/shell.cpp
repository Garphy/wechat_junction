#include "shell.h"

#include "folder.h"
#include "resource.h"

#include <oleacc.h>
#include <oleauto.h>
#include <shldisp.h>
#include <sherrors.h>

#include <memory>
#include <system_error>
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

std::unique_ptr<wchar_t, TaskFree> DesktopPath() {
    PWSTR path = nullptr;
    SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path);
    return std::unique_ptr<wchar_t, TaskFree>(path);
}

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

bool ClassIs(HWND window, const wchar_t* name) {
    wchar_t actual[64]{};
    return GetClassNameW(window, actual, ARRAYSIZE(actual)) && wcscmp(actual, name) == 0;
}

HWND DesktopListIn(HWND host) {
    HWND view = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
    return view ? FindWindowExW(view, nullptr, WC_LISTVIEWW, nullptr) : nullptr;
}

struct DesktopListSearch {
    DWORD explorer;
    HWND list = nullptr;
};

BOOL CALLBACK FindDesktopList(HWND window, LPARAM data) {
    auto& search = *reinterpret_cast<DesktopListSearch*>(data);
    if (!ClassIs(window, L"Progman") && !ClassIs(window, L"WorkerW")) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.explorer) return TRUE;
    search.list = DesktopListIn(window);
    return !search.list;
}

bool BlankDesktopAt(HWND target, POINT point) {
    HWND root = GetAncestor(target, GA_ROOT);
    if (!ClassIs(root, L"Progman") && !ClassIs(root, L"WorkerW")) return false;
    DWORD explorer = 0, windowProcess = 0;
    HWND shell = GetShellWindow();
    GetWindowThreadProcessId(shell, &explorer);
    GetWindowThreadProcessId(root, &windowProcess);
    if (!explorer || explorer != windowProcess) return false;
    if (target != root && !ClassIs(target, L"SHELLDLL_DefView") && !ClassIs(target, WC_LISTVIEWW)) return false;
    HWND list = DesktopListIn(root);
    // Explorer can register the drop on a wallpaper host separate from the icon list.
    if (!list) {
        DesktopListSearch search{explorer};
        EnumWindows(FindDesktopList, reinterpret_cast<LPARAM>(&search));
        list = search.list;
    }
    if (!list || (ClassIs(target, WC_LISTVIEWW) && target != list)) return false;
    RECT bounds{};
    if (!GetWindowRect(list, &bounds) || !PtInRect(&bounds, point)) return false;

    Com<IAccessible> object;
    VARIANT child{};
    bool background = SUCCEEDED(AccessibleObjectFromWindow(list, OBJID_CLIENT, IID_IAccessible,
        reinterpret_cast<void**>(object.Out()))) && object &&
        object->accHitTest(point.x, point.y, &child) == S_OK &&
        child.vt == VT_I4 && child.lVal == CHILDID_SELF;
    VariantClear(&child);
    return background;
}

constexpr DWORD DesktopOperation(DWORD keys, DWORD feedback) {
    auto modifiers = keys & (MK_CONTROL | MK_SHIFT | MK_ALT);
    if (modifiers == MK_CONTROL) return DROPEFFECT_COPY;
    if ((modifiers & MK_ALT) || modifiers == (MK_CONTROL | MK_SHIFT)) return DROPEFFECT_NONE;
    return feedback == DROPEFFECT_MOVE || feedback == DROPEFFECT_COPY ? feedback : DROPEFFECT_NONE;
}

static_assert(DesktopOperation(MK_CONTROL, DROPEFFECT_NONE) == DROPEFFECT_COPY);
static_assert(DesktopOperation(MK_CONTROL, DROPEFFECT_MOVE) == DROPEFFECT_COPY);
static_assert(DesktopOperation(MK_CONTROL | MK_SHIFT, DROPEFFECT_COPY) == DROPEFFECT_NONE);
static_assert(DesktopOperation(0, DROPEFFECT_MOVE) == DROPEFFECT_MOVE);

class DragSource final : public IDropSource, public IDropSourceNotify {
public:
    explicit DragSource(bool handleDesktop) : handleDesktop_(handleDesktop) {}
    DWORD DesktopEffect() const { return desktopEffect_; }
    bool CopyAtRelease() const { return copyAtRelease_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *result = static_cast<IDropSource*>(this);
        } else if (iid == IID_IDropSourceNotify && handleDesktop_) {
            *result = static_cast<IDropSourceNotify*>(this);
        } else {
            return E_NOINTERFACE;
        }
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
        if (keys & MK_LBUTTON) return S_OK;
        POINT point{};
        DWORD operation = DesktopOperation(keys, feedback_);
        copyAtRelease_ = operation == DROPEFFECT_COPY;
        if (handleDesktop_ && operation != DROPEFFECT_NONE && GetCursorPos(&point) &&
            (BlankDesktopAt(target_, point) || BlankDesktopAt(WindowFromPoint(point), point))) {
            // Perform this drop ourselves so Shell returns the actual destination item, including renamed copies.
            desktopEffect_ = operation;
            return DRAGDROP_S_CANCEL;
        }
        return DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        feedback_ = effect & ~DROPEFFECT_SCROLL;
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
    HRESULT STDMETHODCALLTYPE DragEnterTarget(HWND window) override {
        target_ = window;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeaveTarget() override {
        target_ = nullptr;
        return S_OK;
    }
private:
    ULONG references_ = 1;
    bool handleDesktop_;
    HWND target_ = nullptr;
    DWORD feedback_ = DROPEFFECT_NONE;
    DWORD desktopEffect_ = DROPEFFECT_NONE;
    bool copyAtRelease_ = false;
};

class ClipboardCopyData final : public IDataObject, public IDataObjectAsyncCapability {
public:
    ClipboardCopyData(IDataObject* original, const std::shared_ptr<folder::DesktopCopies>& copies)
        : original_(original), copies_(copies) {
        original_->AddRef();
        original_->QueryInterface(IID_IDataObjectAsyncCapability, reinterpret_cast<void**>(async_.Out()));
    }
    ~ClipboardCopyData() { original_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *result = static_cast<IDataObject*>(this);
        } else if (iid == IID_IDataObjectAsyncCapability && async_) {
            *result = static_cast<IDataObjectAsyncCapability*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        auto count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        return Observe(format, original_->GetData(format, medium));
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* format, STGMEDIUM* medium) override {
        return Observe(format, original_->GetDataHere(format, medium));
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override { return original_->QueryGetData(format); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* input, FORMATETC* output) override {
        return original_->GetCanonicalFormatEtc(input, output);
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        return original_->SetData(format, medium, release);
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** formats) override {
        return original_->EnumFormatEtc(direction, formats);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* format, DWORD flags, IAdviseSink* sink, DWORD* connection) override {
        return original_->DAdvise(format, flags, sink, connection);
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD connection) override { return original_->DUnadvise(connection); }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** data) override { return original_->EnumDAdvise(data); }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override { return async_->SetAsyncMode(value); }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* value) override { return async_->GetAsyncMode(value); }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx* context) override { return async_->StartOperation(context); }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* running) override { return async_->InOperation(running); }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx* context, DWORD effects) override {
        return async_->EndOperation(result, context, effects);
    }

private:
    HRESULT Observe(const FORMATETC* format, HRESULT result) {
        static const auto shellIdList = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
        static const auto fileName = RegisterClipboardFormatW(CFSTR_FILENAMEW);
        if (SUCCEEDED(result) && format &&
            (format->cfFormat == CF_HDROP || format->cfFormat == shellIdList || format->cfFormat == fileName)) {
            if (auto copies = copies_.lock()) copies->Activate();
        }
        return result;
    }
    IDataObject* original_;
    Com<IDataObjectAsyncCapability> async_;
    std::weak_ptr<folder::DesktopCopies> copies_;
    ULONG references_ = 1;
};

std::shared_ptr<folder::DesktopCopies> WatchDesktopCopies(const std::wstring& directory, const wchar_t* desktop,
    const std::vector<std::wstring>& names, const std::vector<size_t>& readOnly, bool waitForDrop) {
    try {
        return std::make_shared<folder::DesktopCopies>(directory, desktop, names, readOnly, waitForDrop);
    } catch (const std::system_error&) {
        return {};
    }
}

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

class UnlockSink final : public IFileOperationProgressSink {
public:
    HRESULT Error() const { return error_; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IFileOperationProgressSink) return E_NOINTERFACE;
        *result = static_cast<IFileOperationProgressSink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE StartOperations() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override {
        return OnTransferred(hr, item);
    }
    HRESULT STDMETHODCALLTYPE PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* item) override {
        return OnTransferred(hr, item);
    }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    HRESULT OnTransferred(HRESULT transfer, IShellItem* item) {
        // Shell can report a completed same-volume move as DONT_PROCESS_CHILDREN.
        if (transfer != S_OK && transfer != COPYENGINE_S_DONT_PROCESS_CHILDREN) return S_OK;
        PWSTR raw = nullptr;
        HRESULT hr = item ? item->GetDisplayName(SIGDN_FILESYSPATH, &raw) : E_UNEXPECTED;
        std::unique_ptr<wchar_t, TaskFree> path(raw);
        if (SUCCEEDED(hr)) hr = path ? HRESULT_FROM_WIN32(folder::ClearReadOnly(path.get())) : E_UNEXPECTED;
        if (FAILED(hr) && SUCCEEDED(error_)) error_ = hr;
        return S_OK;
    }
    ULONG references_ = 1;
    HRESULT error_ = S_OK;
};
}

TransferResult TransferToFolder(HWND owner, const std::wstring& directory,
    const std::vector<std::wstring>& names, const std::wstring& destination, DWORD effect) {
    if (effect != DROPEFFECT_COPY && effect != DROPEFFECT_MOVE) return {E_INVALIDARG};
    Com<IShellItem> target;
    HRESULT hr = SHCreateItemFromParsingName(destination.c_str(), nullptr, IID_IShellItem,
        reinterpret_cast<void**>(target.Out()));
    Com<IFileOperation> operation;
    if (SUCCEEDED(hr)) hr = FileOperation(owner, owner ? FOF_ALLOWUNDO | FOFX_ADDUNDORECORD : FOF_NO_UI, operation);
    Com<UnlockSink> sink;
    *sink.Out() = new UnlockSink;
    for (const auto& name : names) {
        if (FAILED(hr)) break;
        auto path = folder::Join(directory, name);
        Com<IShellItem> item;
        hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(item.Out()));
        if (FAILED(hr)) break;
        auto callback = folder::IsReadOnlyFile(GetFileAttributesW(path.c_str())) ? sink.Get() : nullptr;
        hr = effect == DROPEFFECT_MOVE ? operation->MoveItem(item.Get(), target.Get(), nullptr, callback)
            : operation->CopyItem(item.Get(), target.Get(), nullptr, callback);
    }
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    return {hr, sink->Error()};
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

Actions::~Actions() {
    ResetClipboardWatch();
}

void Actions::ResetClipboardWatch() {
    if (!clipboardObject_) return;
    RemoveClipboardFormatListener(owner_);
    clipboardCopies_.reset();
    std::exchange(clipboardObject_, nullptr)->Release();
}

void Actions::ClipboardChanged() {
    if (clipboardObject_ && OleIsCurrentClipboard(clipboardObject_) == S_FALSE) ResetClipboardWatch();
}

void Actions::Copy(const std::wstring& directory, const std::vector<std::wstring>& names, bool cut,
    const std::vector<size_t>& readOnly) {
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
    if (SUCCEEDED(hr)) {
        auto desktop = !cut && !readOnly.empty() ? DesktopPath() : std::unique_ptr<wchar_t, TaskFree>{};
        auto copies = desktop ? WatchDesktopCopies(directory, desktop.get(), names, readOnly, false) : nullptr;
        Com<IDataObject> wrapped;
        if (copies) *wrapped.Out() = new ClipboardCopyData(data.Get(), copies);
        hr = OleSetClipboard(wrapped ? wrapped.Get() : data.Get());
        if (SUCCEEDED(hr)) {
            ResetClipboardWatch();
            if (wrapped && AddClipboardFormatListener(owner_)) {
                clipboardObject_ = wrapped.Get();
                clipboardObject_->AddRef();
                clipboardCopies_ = std::move(copies);
            }
        }
    }
    if (FAILED(hr)) Error(owner_, L"无法将所选项目放入剪贴板。", hr);
}

void Actions::Drag(const std::wstring& directory, const std::vector<std::wstring>& names,
    const std::vector<size_t>& readOnly) {
    Selection selection(directory, names);
    Com<IDataObject> data;
    HRESULT hr = selection.Object(owner_, IID_IDataObject, data.Out());
    if (SUCCEEDED(hr)) {
        auto desktop = readOnly.empty() ? std::unique_ptr<wchar_t, TaskFree>{} : DesktopPath();
        auto desktopPidl = desktop ? Parse(desktop.get()) : Pidl{};
        bool handleDesktop = desktopPidl && !ILIsEqual(selection.parent.get(), desktopPidl.get());
        struct MoveCandidate {
            std::wstring source, destination;
            FILE_ID_INFO identity;
        };
        std::vector<MoveCandidate> candidates;
        if (handleDesktop) {
            for (size_t index : readOnly) {
                const auto& name = names[index];
                auto from = folder::Join(directory, name);
                auto identity = folder::ReadOnlyIdentity(from);
                if (!identity) continue;
                auto to = folder::Join(desktop.get(), name);
                if (GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
                    candidates.push_back({std::move(from), std::move(to), *identity});
                }
            }
        }
        auto copyWatch = handleDesktop ? WatchDesktopCopies(directory, desktop.get(), names, readOnly, true) : nullptr;
        auto* source = new DragSource(handleDesktop);
        DWORD effect = DROPEFFECT_NONE;
        hr = DoDragDrop(data.Get(), source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        DWORD desktopEffect = source->DesktopEffect();
        bool copyAtRelease = source->CopyAtRelease();
        source->Release();
        if (hr == DRAGDROP_S_CANCEL && desktopEffect != DROPEFFECT_NONE) {
            auto result = TransferToFolder(owner_, directory, names, desktop.get(), desktopEffect);
            if (FAILED(result.transfer) && result.transfer != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
                Error(owner_, L"拖到桌面的文件转移未完成。", result.transfer);
            } else if (FAILED(result.unlock)) {
                Error(owner_, L"部分已转移到桌面的文件无法取消只读。", result.unlock);
            }
            return;
        }
        if (hr == DRAGDROP_S_DROP) {
            if (copyWatch && (effect == DROPEFFECT_COPY || copyAtRelease)) {
                copyWatch->Activate();
                dragCopies_ = std::move(copyWatch);
            }
            DWORD firstError = ERROR_SUCCESS;
            for (const auto& candidate : candidates) {
                auto status = folder::ClearReadOnlyAfterMove(candidate.source, candidate.destination, candidate.identity);
                if (status && firstError == ERROR_SUCCESS) firstError = *status;
            }
            if (firstError != ERROR_SUCCESS) {
                Error(owner_, L"部分已移动到桌面的文件无法取消只读。", HRESULT_FROM_WIN32(firstError));
            }
        }
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
