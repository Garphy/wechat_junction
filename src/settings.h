#pragma once

#include "folder.h"

#include <optional>

namespace settings {

struct Values {
    folder::Source source;
    RECT bounds{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    std::wstring monitor;
    std::optional<POINT> workOrigin;
    bool pinned = false;
};

std::wstring FilePath();
std::wstring ExecutablePath();
Values Load(const std::wstring& path, bool& imported);
bool Write(const std::wstring& path, const Values& values);

}
