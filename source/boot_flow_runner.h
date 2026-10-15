#pragma once
#include <functional>
#include <string>

// 0 = passed, 1 = failed, 2 = stopped. Power actions stay in the boot worker.
int executeBootFlow(const std::wstring &file, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error);
