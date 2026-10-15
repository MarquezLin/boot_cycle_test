#pragma once
#include <filesystem>
#include <string>

struct StartupState {
    bool installed = false;
    bool valid = false;
    std::wstring text;
};

class StartupManager {
public:
    explicit StartupManager(std::filesystem::path executable,
                            std::filesystem::path testFolder = {});
    StartupState queryStartup();
    bool installStartup(std::wstring &error);
    bool uninstallStartup(std::wstring &error);
private:
    bool startupFolder(std::filesystem::path &folder, std::wstring &error);
    std::filesystem::path m_dir;
    std::filesystem::path m_folderOverride;
};
