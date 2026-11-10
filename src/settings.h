#pragma once

#include "folder.h"

namespace settings {

struct Values {
    folder::Source source;
    RECT bounds{};
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    bool pinned = false;
};

std::wstring FilePath();
Values Load(const std::wstring& path, bool& imported);
bool Write(const std::wstring& path, const Values& values);

}
