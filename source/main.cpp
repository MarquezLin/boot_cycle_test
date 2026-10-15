#include "mainwindow.h"
#include "flow_dialog.h"
#include "flow_model.h"
#include "flow_runner.h"
#include "flow_samples.h"
#include "image_checks.h"
#include "easy_builder.h"
#include "easy_plan.h"
#include "text_check.h"
#include "ocr_reader.h"
#include "startup_manager.h"
#include <QApplication>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScreen>
#include <QTemporaryDir>
#include <QTimer>
#include <QMessageBox>
#include <QEventLoop>
#include <QLineEdit>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QVBoxLayout>
#include <QAccessible>
#include <QLabel>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
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

static int flowRunnerSelfTest()
{
    QTemporaryDir evidence;
    QDialog fixture; fixture.setWindowTitle("Flow runner verification"); fixture.setObjectName("FixtureWindow");
    auto *layout = new QVBoxLayout(&fixture);
    auto *start = new QPushButton("Flow Start", &fixture);
    start->setObjectName("FixtureStart");
    auto *value = new QLineEdit("received=0 frames", &fixture); value->setAccessibleName("Flow state");
    auto *preview = new QLabel(&fixture); preview->setFixedSize(220, 80);
    QImage bars(220, 80, QImage::Format_RGB32); bars.fill(Qt::black);
    preview->setPixmap(QPixmap::fromImage(bars));
    layout->addWidget(start); layout->addWidget(value); layout->addWidget(preview);
    QTimer animation; animation.setInterval(733);
    QObject::connect(&animation, &QTimer::timeout, &fixture, [preview, bars]() mutable {bars.invertPixels(); preview->setPixmap(QPixmap::fromImage(bars));});
    animation.start();
    QObject::connect(start, &QPushButton::clicked, &fixture, [value] {value->setText("received=64 frames");});
    fixture.show(); ShowWindow(reinterpret_cast<HWND>(fixture.winId()), SW_SHOWNORMAL);
    QDialog extraPreview; extraPreview.setWindowTitle("Flow additional preview"); extraPreview.resize(100, 80); extraPreview.show();
    QDialog ownedPreview(&fixture); ownedPreview.setWindowTitle("Flow owned preview"); ownedPreview.setModal(false); ownedPreview.show();
    QApplication::processEvents();
    extraPreview.move(20, 20); ownedPreview.move(20, 160);
    const QString fixtureExe = QCoreApplication::applicationFilePath();
    if (findFlowWindow(fixtureExe) || !findFlowWindow(fixtureExe, 0, fixture.windowTitle())
        || !findFlowWindow(fixtureExe, 0, ownedPreview.windowTitle())
        || !flowWindowError(fixtureExe).contains("多個視窗")) return 1;
    extraPreview.setWindowTitle(fixture.windowTitle()); QApplication::processEvents();
    if (findFlowWindow(fixtureExe, 0, fixture.windowTitle())) return 1;
    extraPreview.setWindowTitle("Flow additional preview");
    ownedPreview.hide();
    requestFlowForeground(static_cast<quintptr>(fixture.winId()));
    QAccessible::setActive(true); QApplication::processEvents();
    const HWND fixtureHandle = reinterpret_cast<HWND>(fixture.winId());
    RECT originalFrame{}, originalClient{}; MONITORINFO fixtureMonitor{sizeof(MONITORINFO)};
    if (!GetWindowRect(fixtureHandle, &originalFrame) || !GetClientRect(fixtureHandle, &originalClient)
        || !GetMonitorInfoW(MonitorFromWindow(fixtureHandle, MONITOR_DEFAULTTONEAREST), &fixtureMonitor)) return 1;
    SetWindowPos(fixtureHandle, nullptr, fixtureMonitor.rcWork.right - 40, fixtureMonitor.rcWork.bottom - 40,
        0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    QString placementError;
    const auto placedCapture = captureFlowWindow(static_cast<quintptr>(fixture.winId()), placementError);
    if (placedCapture.isNull() || placedCapture.width() != originalClient.right || placedCapture.height() != originalClient.bottom) {
        QFile report(QCoreApplication::applicationDirPath() + "/placement_check.json");
        if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject{{"error", placementError},
            {"width", placedCapture.width()}, {"height", placedCapture.height()},
            {"expectedWidth", int(originalClient.right)}, {"expectedHeight", int(originalClient.bottom)}}).toJson());
        return 1;
    }
    SetWindowPos(fixtureHandle, nullptr, originalFrame.left, originalFrame.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    FlowRunner runner; runner.flow.executable = QCoreApplication::applicationFilePath(); runner.evidenceDirectory = evidence.path();
    FlowStep wait; wait.type = "waitWindow"; wait.timeoutMs = 2000;
    FlowStep click; click.type = "clickUIA"; click.selector = "Flow Start";
    FlowStep read; read.type = "readUIA"; read.selector = "Flow state"; read.text = "received=([0-9]+)"; read.timeoutMs = 2000;
    read.condition = "gt"; read.expected = "0";
    FlowStep check; check.type = "assert"; check.condition = "gt"; check.expected = "0";
    FlowStep point; point.type = "clickPoint";
    const QPoint clientPoint = value->mapTo(&fixture, value->rect().center());
    point.region = QRect(qRound(clientPoint.x() * fixture.devicePixelRatioF()), qRound(clientPoint.y() * fixture.devicePixelRatioF()), 0, 0);
    FlowStep select; select.type = "hotkey"; select.text = "Ctrl+A";
    FlowStep input; input.type = "typeText"; input.text = "測試 123";
    FlowStep readInput; readInput.type = "readUIA"; readInput.selector = "Flow state"; readInput.condition = "equals"; readInput.expected = input.text; readInput.timeoutMs = 2000;
    FlowStep content; content.type = "checkImage"; content.condition = "contrast"; content.expected = "8"; content.timeoutMs = 3000;
    const QPoint previewPoint = preview->mapTo(&fixture, QPoint(0, 0));
    content.region = QRect(qRound(previewPoint.x() * fixture.devicePixelRatioF()), qRound(previewPoint.y() * fixture.devicePixelRatioF()),
        qRound(preview->width() * fixture.devicePixelRatioF()), qRound(preview->height() * fixture.devicePixelRatioF()));
    FlowStep motion = content; motion.condition = "change"; motion.expected = "2"; motion.timeoutMs = 3000;
    FlowStep pointRead = readInput; pointRead.selector = "@point"; pointRead.region = point.region;
    POINT pickedPoint{point.region.x(), point.region.y()}; ClientToScreen(fixtureHandle, &pickedPoint);
    if (flowWindowAtPoint(fixtureExe, QPoint(pickedPoint.x, pickedPoint.y)) != static_cast<quintptr>(fixture.winId())) return 1;
    FlowStep reuse; reuse.type = "launch";
    FlowStep settleClick; settleClick.type = "delay"; settleClick.timeoutMs = 250;
    runner.flow.steps = {reuse, reuse, wait, click, settleClick, read, check, point, select, input, readInput, pointRead, content, motion};
    for (auto &step : runner.flow.steps) step.windowTitle = fixture.windowTitle();
    runner.flow.steps[3].windowTitle = "Previous preview window no longer exists";
    runner.flow.steps[3].selector.clear(); runner.flow.steps[3].automationId = "QApplication.FixtureWindow.FixtureStart";
    bool recoveredWindow = false;
    QObject::connect(&runner, &FlowRunner::windowResolved, &fixture, [&](int index, const QString &title) {
        if (index == 3 && title == fixture.windowTitle()) recoveredWindow = true;
    });
    QEventLoop loop; bool success = false, readCorrect = false;
    QObject::connect(&runner, &FlowRunner::valueRead, &fixture, [&](const QString &text) {if (text == "64") readCorrect = true;});
    QObject::connect(&runner, &FlowRunner::progress, &fixture, [&](const QString &text) {
        QFile report(QCoreApplication::applicationDirPath() + "/flow_check.log");
        if (report.open(QIODevice::Append)) report.write(text.toUtf8() + '\n');
    });
    QObject::connect(&runner, &FlowRunner::completed, &fixture, [&](bool ok, bool stopped) {success = ok && !stopped;});
    QObject::connect(&runner, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &fixture, [&] {runner.stop();}); timeout.start(10000);
    runner.start(); loop.exec(); runner.wait();
    if (!success || !readCorrect || !recoveredWindow || value->text() != input.text) return 1;
    // A frozen solid image must not pass either content or motion checks.
    animation.stop(); preview->setPixmap(QPixmap::fromImage(bars)); QApplication::processEvents();
    FlowStep wrongButton; wrongButton.type = "readUIA"; wrongButton.selector = "Flow Start"; wrongButton.condition = "equals"; wrongButton.expected = "Running";
    for (auto frozenStep : {content, motion, wrongButton}) {
        frozenStep.timeoutMs = 900; frozenStep.windowTitle = fixture.windowTitle();
        FlowRunner frozen; frozen.flow = runner.flow; frozen.flow.steps = {frozenStep}; frozen.evidenceDirectory = evidence.path();
        bool rejected = false, observed = false;
        QObject::connect(&frozen, &FlowRunner::stepObserved, &fixture, [&](int, const QString &) {observed = true;});
        QObject::connect(&frozen, &FlowRunner::completed, &fixture, [&](bool ok, bool stopped) {rejected = !ok && !stopped;});
        QObject::connect(&frozen, &QThread::finished, &loop, &QEventLoop::quit);
        frozen.start(); loop.exec(); frozen.wait();
        if (!rejected || (frozenStep.type == "readUIA" && !observed)) return 1;
    }
    QDialog delayedWindow; delayedWindow.setWindowTitle("Delayed Preview"); delayedWindow.setFixedSize(240, 120);
    auto *delayedImage = new QLabel(&delayedWindow); delayedImage->setGeometry(0, 0, 240, 120);
    QImage delayedBars(240, 120, QImage::Format_RGB32); delayedBars.fill(Qt::black);
    QPainter delayedPainter(&delayedBars); delayedPainter.fillRect(QRect(120, 0, 120, 120), Qt::white); delayedPainter.end();
    delayedImage->setPixmap(QPixmap::fromImage(delayedBars));
    FlowRunner delayed; delayed.flow.executable = fixtureExe; delayed.evidenceDirectory = evidence.path();
    FlowStep delayedStep = content; delayedStep.windowTitle = delayedWindow.windowTitle(); delayedStep.region = QRect(0, 0, 240, 120); delayedStep.timeoutMs = 3000;
    delayed.flow.steps = {delayedStep}; bool delayedPass = false, waitingObserved = false;
    QObject::connect(&delayed, &FlowRunner::progress, &fixture, [&](const QString &text) {
        QFile report(QCoreApplication::applicationDirPath() + "/flow_check.log");
        if (report.open(QIODevice::Append)) report.write("Delayed preview: " + text.toUtf8() + '\n');
    });
    QObject::connect(&delayed, &FlowRunner::stepObserved, &fixture, [&](int, const QString &text) {if (text.contains("等待待測視窗")) waitingObserved = true;});
    QObject::connect(&delayed, &FlowRunner::completed, &fixture, [&](bool ok, bool stopped) {delayedPass = ok && !stopped;});
    QObject::connect(&delayed, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(500, &fixture, [&] {
        delayedWindow.show();
        SetWindowPos(reinterpret_cast<HWND>(delayedWindow.winId()), nullptr, fixtureMonitor.rcWork.right - 40, fixtureMonitor.rcWork.bottom - 40,
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    });
    delayed.start(); loop.exec(); delayed.wait(); delayedWindow.hide();
    if (!delayedPass || !waitingObserved) return 1;
    FlowRunner cancelled; cancelled.flow.executable = QCoreApplication::applicationFilePath(); cancelled.evidenceDirectory = evidence.path();
    FlowStep delay; delay.type = "delay"; delay.timeoutMs = 5000; cancelled.flow.steps = {delay};
    bool didStop = false;
    QObject::connect(&cancelled, &FlowRunner::completed, &fixture, [&](bool ok, bool stopped) {didStop = !ok && stopped;});
    QObject::connect(&cancelled, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(100, &fixture, [&] {cancelled.stop();}); cancelled.start(); loop.exec(); cancelled.wait();
    return didStop ? 0 : 1;
}

static QImage ocrFixture(const QString &text, bool dark = false)
{
    QImage image(480, 90, QImage::Format_RGB32); image.fill(dark ? QColor("#20242a") : Qt::white);
    QPainter painter(&image); painter.setRenderHint(QPainter::TextAntialiasing); painter.setPen(dark ? Qt::white : Qt::black);
    painter.setFont(QFont("Microsoft JhengHei UI", 30)); painter.drawText(image.rect().adjusted(15, 0, -15, 0), Qt::AlignVCenter, text); return image;
}
static int ocrSelfTest()
{
    const auto engine = detectOcrExecutable(); if (engine.isEmpty()) return 1;
    QTemporaryDir dir; QJsonArray records; bool success = true;
    for (int i = 0; i < 3; ++i) {
        const QString expected = i == 0 ? "Running" : i == 1 ? "Not Running" : "執行中";
        const auto path = dir.path() + "/text.png"; const auto image = prepareOcrImage(ocrFixture(expected, i == 1)); image.save(path);
        image.save(QCoreApplication::applicationDirPath() + QString("/ocr_fixture_%1.png").arg(i));
        QProcess process; process.start(engine, ocrArguments(engine, path, defaultOcrLanguage(engine)));
        const bool completed = process.waitForFinished(15000); if (!completed) {process.kill(); process.waitForFinished(1000);}
        const auto text = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
        QString normalized = text; normalized.remove(' '); QString cleanExpected = expected; cleanExpected.remove(' ');
        const bool ok = completed && process.exitCode() == 0 && normalized.compare(cleanExpected, Qt::CaseInsensitive) == 0;
        success = success && ok; records.append(QJsonObject{{"expected", expected}, {"recognized", text}, {"success", ok}});
    }
    QString parsed, error;
    success = success && evaluateFlowValue("running", "equalsIgnoreCase", "Running", parsed, error)
        && !evaluateFlowValue("Not Running", "equalsIgnoreCase", "Running", parsed, error);
    QFile file(QCoreApplication::applicationDirPath() + "/ocr_check.json");
    if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{{"success", success}, {"engine", engine}, {"results", records}}).toJson());
    return success ? 0 : 1;
}

int main(int argc, char **argv)
{
    int wideArgc = 0;
    wchar_t **wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (!wideArgv) return 1;
    if (wideArgc > 1 && std::wstring(wideArgv[1]) == L"--worker") {
        std::unique_ptr<QApplication> workerApplication;
        for (int i = 2; i < wideArgc; ++i)
            if (std::wstring(wideArgv[i]) == L"--flow-file") {workerApplication = std::make_unique<QApplication>(argc, argv); break;}
        const int result = runBootTest(wideArgc - 1, wideArgv + 1);
        LocalFree(wideArgv); return result;
    }
    LocalFree(wideArgv);
    QApplication app(argc, argv);
    QApplication::setApplicationName("GVFG_BootCycleTest");
    const QStringList args = app.arguments();
    if (args.contains("--flow-self-test")) return flowModelSelfTest() && flowSamplesSelfTest() && imageChecksSelfTest() && easyPlanSelfTest() ? 0 : 1;
    if (args.contains("--flow-insertion-self-test")) {
        FlowDialog dialog(QString{}); const bool passed = dialog.insertionSelfTest();
        dialog.show(); app.processEvents();
        dialog.grab().save(QCoreApplication::applicationDirPath() + "/insertion_preview.png");
        return passed ? 0 : 1;
    }
    if (args.contains("--easy-button-self-test") || args.contains("--easy-delay-self-test")) {
        const bool delayOnly = args.contains("--easy-delay-self-test");
        EasyBuilder dialog(nullptr, true);
        auto *buttonCheck = dialog.findChild<QAbstractButton *>(delayOnly ? "delayCheck" : "startCheck");
        auto *buttons = dialog.findChild<QDialogButtonBox *>("buttonBox");
        if (!buttonCheck || !buttons) return 1;
        buttonCheck->setChecked(true);
        if (delayOnly) {
            auto *seconds = dialog.findChild<QDoubleSpinBox *>("delaySpin");
            if (!seconds || !seconds->isEnabled()) return 1;
            seconds->setValue(1.5);
        }
        QTimer::singleShot(1000, &dialog, [] {
            for (auto *widget : QApplication::topLevelWidgets())
                if (auto *message = qobject_cast<QMessageBox *>(widget)) message->reject();
        });
        buttons->button(QDialogButtonBox::Ok)->click();
        const auto steps = easyPlanSteps(dialog.plan());
        return dialog.result() == QDialog::Accepted && steps.size() == 1
            && (!delayOnly || (steps.first().type == "delay" && steps.first().timeoutMs == 1500)) ? 0 : 1;
    }
    if (args.contains("--easy-preview")) {
        EasyBuilder dialog; dialog.show(); app.processEvents();
        if (!easyPlanSteps(dialog.plan()).isEmpty()) return 1;
        return dialog.grab().save(QCoreApplication::applicationDirPath() + "/easy_preview.png") ? 0 : 1;
    }
    if (args.contains("--ocr-self-test")) return ocrSelfTest();
    const int captureOption = args.indexOf("--capture-window");
    if (captureOption >= 0 && args.size() > captureOption + 2) {
        const QString exe = args[captureOption + 1], title = args[captureOption + 2];
        const auto window = findFlowWindow(exe, 0, title); QString error;
        if (window && args.contains("--fit-capture-window")) {
            const HWND native = reinterpret_cast<HWND>(window); RECT bounds{}; MONITORINFO monitor{sizeof(MONITORINFO)};
            if (GetWindowRect(native, &bounds) && GetMonitorInfoW(MonitorFromWindow(native, MONITOR_DEFAULTTONEAREST), &monitor)
                && bounds.right - bounds.left <= monitor.rcWork.right - monitor.rcWork.left
                && bounds.bottom - bounds.top <= monitor.rcWork.bottom - monitor.rcWork.top) {
                const int x = qBound(int(monitor.rcWork.left), int(bounds.left), int(monitor.rcWork.right - (bounds.right - bounds.left)));
                const int y = qBound(int(monitor.rcWork.top), int(bounds.top), int(monitor.rcWork.bottom - (bounds.bottom - bounds.top)));
                SetWindowPos(native, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        requestFlowForeground(window);
        QEventLoop settle; QTimer::singleShot(500, &settle, &QEventLoop::quit); settle.exec();
        const auto captured = captureFlowWindow(window, error);
        const QString directory = QCoreApplication::applicationDirPath();
        const bool saved = !captured.isNull() && captured.save(directory + "/capture_check.png");
        QFile report(directory + "/capture_check.json");
        if (report.open(QIODevice::WriteOnly)) report.write(QJsonDocument(QJsonObject{{"success", saved}, {"error", error},
            {"title", title}, {"width", captured.width()}, {"height", captured.height()}, {"contrast", imageContrast(captured)}}).toJson());
        return saved ? 0 : 1;
    }
    if (args.contains("--region-preview")) return regionPreview();
    if (args.contains("--text-preview")) {
        FlowStep step; step.name = "確認 Running"; step.condition = "equalsIgnoreCase"; step.expected = "Running";
        TextCheck dialog(ocrFixture("Running"), step, detectOcrExecutable(), defaultOcrLanguage(detectOcrExecutable()));
        dialog.show(); QEventLoop loop; QTimer::singleShot(3000, &loop, &QEventLoop::quit); loop.exec();
        return dialog.grab().save(QCoreApplication::applicationDirPath() + "/text_preview.png") ? 0 : 1;
    }
    if (args.contains("--flow-runner-self-test")) return flowRunnerSelfTest();
    if (args.contains("--flow-preview")) {
        FlowDialog dialog(QString{}); dialog.show(); app.processEvents();
        return dialog.grab().save(QCoreApplication::applicationDirPath() + "/flow_preview.png") ? 0 : 1;
    }
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
