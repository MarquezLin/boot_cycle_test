#include "startup_manager.h"
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <algorithm>
#include <utility>
namespace fs = std::filesystem;
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};


StartupManager::StartupManager(fs::path executable, fs::path testFolder)
    : m_dir(executable.parent_path()), m_folderOverride(std::move(testFolder))
{
    m_dir.make_preferred();
}
bool StartupManager::startupFolder(fs::path &folder, std::wstring &error)
{
    if (!m_folderOverride.empty()) { folder = m_folderOverride; return true; }
    wchar_t path[MAX_PATH]{};
    const HRESULT hr = SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, path);
    if (FAILED(hr)) { error = L"無法取得 Windows 啟動資料夾，錯誤碼：" + std::to_wstring(hr); return false; }
    folder = path; return true;
}

StartupState StartupManager::queryStartup()
{
    ComScope com;
    if (FAILED(com.result)) return {false, false, L"無法初始化 Windows 啟動管理介面"};
    fs::path folder; std::wstring error;
    if (!startupFolder(folder, error)) return {false, false, error};
    std::error_code ec;
    const bool linkExists = fs::exists(folder / "GVFG_Boot_Cycle_Test.lnk", ec);
    if (ec) return {false, false, L"無法讀取自動啟動設定：" + std::to_wstring(ec.value())};
    const bool cmdExists = fs::exists(folder / "GVFG_Boot_Cycle_Test.cmd", ec);
    if (ec) return {false, false, L"無法讀取自動啟動設定：" + std::to_wstring(ec.value())};
    if (!linkExists && !cmdExists) return {false, false, L"自動啟動：未安裝"};
    if (linkExists && cmdExists) return {true, false, L"自動啟動：項目重複，請重新安裝"};
    bool valid = false;
    bool pathChanged = false;
    if (linkExists) {
        IShellLinkW *link = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
            IPersistFile *file = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
                if (SUCCEEDED(file->Load((folder / "GVFG_Boot_Cycle_Test.lnk").c_str(), STGM_READ))) {
                    wchar_t target[32768]{}, args[256]{};
                    if (SUCCEEDED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH))) {
                        pathChanged = _wcsicmp(target, (m_dir / "GVFG_BootCycleTest.exe").c_str()) != 0;
                        wchar_t workingDirectory[32768]{};
                        valid = !pathChanged && SUCCEEDED(link->GetArguments(args, 256))
                            && std::wstring(args) == L"--auto-run"
                            && SUCCEEDED(link->GetWorkingDirectory(workingDirectory, 32768))
                            && _wcsicmp(workingDirectory, m_dir.c_str()) == 0;
                    }
                }
                file->Release();
            }
            link->Release();
        }
    } else {
        std::ifstream input(folder / "GVFG_Boot_Cycle_Test.cmd", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::replace(text.begin(), text.end(), '/', '\\');
        // The earlier launcher used an ASCII absolute path. Migrate it on install.
        const auto exe = (m_dir / "GVFG_BootCycleTest.exe").u8string();
        valid = input.is_open() && text.find('"' + exe + '"') != std::string::npos
            && text.find("--auto-run") != std::string::npos;
    }
    return {true, valid, valid ? L"自動啟動：已安裝（登入後執行）"
                               : pathChanged ? L"自動啟動：路徑已變更，請按更新自動啟動"
                                             : L"自動啟動：路徑或設定不符，請按更新自動啟動"};
}

bool removeStartupFile(const fs::path &path, std::wstring &error)
{
    std::error_code ec;
    for (int attempt = 0; attempt < 11; ++attempt) {
        fs::remove(path, ec);
        if (!ec || (ec.value() != ERROR_SHARING_VIOLATION && ec.value() != ERROR_LOCK_VIOLATION) || attempt == 10) break;
        Sleep(100);
    }
    if (ec) {
        error = L"無法移除 " + path.filename().wstring() + L"，錯誤碼：" + std::to_wstring(ec.value());
        if (ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_LOCK_VIOLATION)
            error += L"。檔案正被其他程序使用，請關閉檔案總管的捷徑內容／預覽視窗後重試。";
        return false;
    }
    return true;
}

bool StartupManager::installStartup(std::wstring &error)
{
    ComScope com;
    if (FAILED(com.result)) { error = L"無法初始化 Windows 啟動管理介面"; return false; }
    fs::path folder;
    if (!startupFolder(folder, error)) return false;
    if (!removeStartupFile(folder / "GVFG_Boot_Cycle_Test.installing.lnk", error)) return false;
    // Avoid rewriting an already correct shortcut (Explorer may be reading it).
    std::error_code ec;
    if (queryStartup().valid && fs::exists(folder / "GVFG_Boot_Cycle_Test.lnk", ec)
        && !ec && !fs::exists(folder / "GVFG_Boot_Cycle_Test.cmd", ec) && !ec) return true;
    // Keep the temporary .lnk out of the Startup folder's top level.
    // A same-volume temporary directory also allows atomic publication.
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempFileNameW(folder.c_str(), L"GVF", 0, tempPath)) {
        error = L"無法建立自動啟動暫存檔，錯誤碼：" + std::to_wstring(GetLastError());
        return false;
    }
    DeleteFileW(tempPath);
    if (!CreateDirectoryW(tempPath, nullptr)) {
        error = L"無法建立自動啟動暫存資料夾，錯誤碼：" + std::to_wstring(GetLastError());
        return false;
    }
    struct TemporaryShortcut {
        fs::path directory;
        fs::path path;
        ~TemporaryShortcut() { DeleteFileW(path.c_str()); RemoveDirectoryW(directory.c_str()); }
    } temporary{fs::path(tempPath), fs::path(tempPath) / "shortcut.lnk"};
    const fs::path &temp = temporary.path;
    IShellLinkW *link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (SUCCEEDED(hr)) {
        hr = link->SetPath((m_dir / "GVFG_BootCycleTest.exe").c_str());
        if (SUCCEEDED(hr)) hr = link->SetArguments(L"--auto-run");
        if (SUCCEEDED(hr)) hr = link->SetWorkingDirectory(m_dir.c_str());
        if (SUCCEEDED(hr)) hr = link->SetShowCmd(SW_SHOWNORMAL);
        IPersistFile *file = nullptr;
        if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_PPV_ARGS(&file));
        if (SUCCEEDED(hr)) { hr = file->Save(temp.c_str(), TRUE); file->Release(); }
        link->Release();
    }
    if (FAILED(hr)) {
        DeleteFileW(temp.c_str());
        error = L"無法安裝自動啟動，錯誤碼：" + std::to_wstring(hr); return false;
    }
    DWORD code = ERROR_SUCCESS;
    bool published = false;
    for (int attempt = 0; attempt < 11; ++attempt) {
        if (MoveFileExW(temp.c_str(), (folder / "GVFG_Boot_Cycle_Test.lnk").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            published = true; break;
        }
        code = GetLastError();
        // Windows can report ACCESS_DENIED for replacement of a locked target.
        if ((code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION
             && code != ERROR_ACCESS_DENIED) || attempt == 10) break;
        Sleep(100);
    }
    if (!published) {
        DeleteFileW(temp.c_str());
        error = L"無法保存自動啟動捷徑，錯誤碼：" + std::to_wstring(code);
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
            error += L"。捷徑正被其他程序使用；請關閉啟動資料夾的檔案總管／捷徑內容視窗後重試。原有捷徑已保留。";
        else if (code == ERROR_ACCESS_DENIED)
            error += L"。請確認啟動資料夾權限，並關閉正在使用捷徑的視窗後重試。原有捷徑已保留。";
        return false;
    }
    if (!removeStartupFile(folder / "GVFG_Boot_Cycle_Test.cmd", error)) return false;
    const auto state = queryStartup();
    if (!state.valid) { error = state.text; return false; }
    return true;
}

bool StartupManager::uninstallStartup(std::wstring &error)
{
    fs::path folder;
    if (!startupFolder(folder, error)) return false;
    if (!removeStartupFile(folder / "GVFG_Boot_Cycle_Test.lnk", error)
        || !removeStartupFile(folder / "GVFG_Boot_Cycle_Test.cmd", error)
        || !removeStartupFile(folder / "GVFG_Boot_Cycle_Test.installing.lnk", error)) return false;
    const auto state = queryStartup();
    if (state.installed || state.text != L"自動啟動：未安裝") { error = state.text; return false; }
    return true;
}


