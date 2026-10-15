#include "mainwindow.h"
#include "startup_manager.h"
#include <QApplication>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QMessageBox>
#include <windows.h>
#include <shellapi.h>
#include <thread>

int runBootTest(int argc, wchar_t **argv);

static QString desktopServerName()
{
    wchar_t station[256]{}, desktop[256]{}; DWORD needed = 0;
    if (!GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, station, sizeof(station), &needed)
        || !GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &needed))
        return {};
    return "GVFG_BootCycleTest_" + QString::fromWCharArray(station) + '_' + QString::fromWCharArray(desktop);
}

static int startupSelfTest()
{
    QTemporaryDir folder(QCoreApplication::applicationDirPath() + "/startup_check_XXXXXX");
    if (!folder.isValid()) return 1;
    StartupManager manager(std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString()),
                           std::filesystem::path(folder.path().toStdWString()));
    if (manager.queryStartup().installed) return 1;
    QFile legacy(folder.path() + "/GVFG_Boot_Cycle_Test.cmd");
    if (!legacy.open(QIODevice::WriteOnly)) return 1;
    legacy.write("start \"\" \"" + QCoreApplication::applicationFilePath().toUtf8() + "\" --auto-run\r\n");
    legacy.close();
    std::wstring error;
    auto failed = [&error](const wchar_t *step) {
        QFile report(QCoreApplication::applicationDirPath() + "/startup_check_error.txt");
        if (report.open(QIODevice::WriteOnly)) report.write((QString::fromWCharArray(step) + ": " + QString::fromStdWString(error)).toUtf8());
        return 1;
    };
    if (!manager.queryStartup().valid || !manager.installStartup(error) || !manager.queryStartup().valid || legacy.exists()) return failed(L"initial install");
    StartupManager moved(std::filesystem::path(folder.path().toStdWString()) / "GVFG_BootCycleTest.exe",
                         std::filesystem::path(folder.path().toStdWString()));
    if (moved.queryStartup().valid || moved.queryStartup().text.find(L"路徑已變更") == std::wstring::npos
        || !moved.installStartup(error) || !moved.queryStartup().valid
        || manager.queryStartup().valid) return failed(L"moved install");
    const auto linkPath = std::filesystem::path(folder.path().toStdWString()) / "GVFG_Boot_Cycle_Test.lnk";
    HANDLE locked = CreateFileW(linkPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 1;
    const bool rejected = !manager.installStartup(error)
        && error.find(L"原有捷徑已保留") != std::wstring::npos;
    CloseHandle(locked);
    if (!rejected || !moved.queryStartup().valid) return failed(L"held lock");
    locked = CreateFileW(linkPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 1;
    std::thread releaseLock([locked] { Sleep(250); CloseHandle(locked); });
    const bool recovered = manager.installStartup(error);
    releaseLock.join();
    if (!recovered || !manager.queryStartup().valid) return failed(L"released lock");
    if (!legacy.open(QIODevice::WriteOnly)) return 1;
    legacy.write("obsolete duplicate"); legacy.close();
    if (!manager.queryStartup().installed || manager.queryStartup().valid) return 1;
    return manager.installStartup(error) && manager.queryStartup().valid && manager.uninstallStartup(error)
        && !manager.queryStartup().installed && manager.installStartup(error) && manager.queryStartup().valid
        && manager.uninstallStartup(error) && manager.uninstallStartup(error) ? 0 : 1;
}

int main(int argc, char **argv)
{
    int wideArgc = 0;
    wchar_t **wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (!wideArgv) return 1;
    if (wideArgc > 1 && std::wstring(wideArgv[1]) == L"--worker") {
        const int result = runBootTest(wideArgc - 1, wideArgv + 1);
        LocalFree(wideArgv); return result;
    }
    LocalFree(wideArgv);
    QApplication app(argc, argv);
    QApplication::setApplicationName("GVFG_BootCycleTest");
    const QStringList args = app.arguments();
    if (args.contains("--startup-self-test")) return startupSelfTest();
    if (args.contains("--ui-smoke-test")) {
        MainWindow window;
        window.show(); app.processEvents();
        return window.smokeTest() ? 0 : 1;
    }
    if (args.contains("--ui-preview")) {
        MainWindow window;
        window.show(); app.processEvents();
        return window.grab().save(QCoreApplication::applicationDirPath() + "/ui_preview.png") ? 0 : 1;
    }
    const QString serverName = desktopServerName();
    if (serverName.isEmpty()) { QMessageBox::critical(nullptr, "啟動失敗", "無法辨識目前的 Windows 桌面。"); return 1; }
    QLocalSocket client;
    client.connectToServer(serverName);
    if (client.waitForConnected(300)) {
        const HWND existing = FindWindowW(nullptr, L"GVFG 開機循環測試（冷／暖）");
        if (existing) { DWORD pid = 0; GetWindowThreadProcessId(existing, &pid); AllowSetForegroundWindow(pid); }
        client.write("show"); client.waitForBytesWritten(1000); return 0;
    }
    QLocalServer server;
    if (!server.listen(serverName)) { QMessageBox::critical(nullptr, "啟動失敗", server.errorString()); return 1; }
    MainWindow window;
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QLocalSocket::readyRead, &window, [socket, &window] {
                if (socket->readAll().contains("show")) window.showExisting();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            if (socket->bytesAvailable() && socket->readAll().contains("show")) window.showExisting();
        }
    });
    if (auto *screen = window.screen())
        window.resize(window.size().boundedTo(screen->availableGeometry().size() - QSize(32, 48)));
    window.show();
    if (args.contains("--auto-run")) QTimer::singleShot(0, &window, [&] { window.startTest(false, true); });
    return app.exec();
}
