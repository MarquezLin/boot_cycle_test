#include "mainwindow.h"
#include "flow_dialog.h"
#include "ui_mainwindow.h"
#include <QApplication>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QDir>
#include <QDateTime>
#include <QUuid>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>()),
      m_startup(std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString())),
      m_dir(QCoreApplication::applicationDirPath())
{
    ui->setupUi(this);
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    ui->bootModeCombo->setCurrentIndex(settings.value("Test/BootMode", "cold").toString() == "warm" ? 1 : 0);
    ui->targetPathEdit->setText(settings.value("Test/TargetExe").toString());
    ui->useFlowCheck->setChecked(settings.value("Test/UseFlow", false).toBool());
    const QString flowFile = settings.value("Test/FlowFile").toString();
    if (!flowFile.isEmpty()) m_bootFlowFile = QDir(m_dir).absoluteFilePath(flowFile);
    m_bootFlowName = settings.value("Test/FlowName").toString();
    refreshBootFlowDisplay();
    connect(ui->browseTargetButton, &QPushButton::clicked, this, &MainWindow::browseTarget);
    connect(ui->flowButton, &QPushButton::clicked, this, [this] {
        FlowDialog dialog(ui->targetPathEdit->text(), this);
        TestFlow imported; QString error;
        if (ui->useFlowCheck->isChecked() && loadFlow(m_bootFlowFile, imported, error)) dialog.setEditingCopy(imported, m_bootFlowName);
        dialog.setBootImporter([this, &dialog](const TestFlow &flow) {return importBootFlow(flow, dialog.sourceFileName());}); dialog.exec();
    });
    connect(ui->importFlowButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, "匯入開機後檢查流程", {}, "測試流程 (*.json)");
        if (file.isEmpty()) return;
        TestFlow flow; QString error;
        if (!loadFlow(file, flow, error)) {QMessageBox::warning(this, "無法匯入流程", error); return;}
        importBootFlow(flow, QFileInfo(file).fileName());
    });
    connect(ui->useFlowCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!saveSettings()) {QSignalBlocker block(ui->useFlowCheck); ui->useFlowCheck->setChecked(!checked);}
    });
    refreshModeNote();
    connect(ui->bootModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!saveSettings()) {
            QSignalBlocker blocker(ui->bootModeCombo);
            ui->bootModeCombo->setCurrentIndex(index == 0 ? 1 : 0);
        }
        refreshModeNote();
    });
    connect(ui->dryButton, &QPushButton::clicked, this, [this] { startTest(true); });
    connect(ui->runButton, &QPushButton::clicked, this, [this] { startTest(false); });
    connect(ui->stopButton, &QPushButton::clicked, this, &MainWindow::requestStop);
    connect(ui->clearButton, &QPushButton::clicked, this, &MainWindow::clearHistory);
    connect(ui->folderButton, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_dir));
    });
    connect(ui->installButton, &QPushButton::clicked, this, [this] {
        if (!checkEnvironment()) return;
        if (!saveSettings()) return;
        std::wstring error;
        if (!m_startup.installStartup(error))
            QMessageBox::critical(this, "安裝失敗", QString::fromStdWString(error));
        refreshStartup();
    });
    connect(ui->uninstallButton, &QPushButton::clicked, this, [this] {
        std::wstring error;
        if (!m_startup.uninstallStartup(error))
            QMessageBox::critical(this, "解除失敗", QString::fromStdWString(error));
        refreshStartup();
    });
    connect(&m_worker, &QProcess::finished, this, &MainWindow::finishTest);
    connect(&m_worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_fail = true;
        showLine("FAIL: " + m_worker.errorString());
        m_logTimer.stop();
        setRunning(false);
    });
    connect(&m_logTimer, &QTimer::timeout, this, &MainWindow::readLog);
    connect(&m_startupTimer, &QTimer::timeout, this, &MainWindow::refreshStartup);
    m_logTimer.setInterval(250);
    m_startupTimer.start(1000);
    setRunning(false);
    loadHistory();
    setStatus(targetAvailable() ? "準備就緒 — 請先試跑" : "尚未選擇有效的待測 APP — 請按瀏覽");
}

MainWindow::~MainWindow() = default;

bool MainWindow::warmMode() const { return ui->bootModeCombo->currentIndex() == 1; }

bool MainWindow::saveSettings()
{
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    settings.setValue("Test/BootMode", warmMode() ? "warm" : "cold");
    settings.setValue("Test/TargetExe", ui->targetPathEdit->text());
    settings.setValue("Test/UseFlow", ui->useFlowCheck->isChecked());
    settings.setValue("Test/FlowFile", m_bootFlowFile.isEmpty() ? QString() : QDir(m_dir).relativeFilePath(m_bootFlowFile));
    settings.setValue("Test/FlowName", m_bootFlowName);
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    QMessageBox::critical(this, "設定保存失敗", "無法保存模式與待測 APP 路徑，請檢查資料夾寫入權限。");
    return false;
}

void MainWindow::browseTarget()
{
    if (m_running) return;
    const QString selected = QFileDialog::getOpenFileName(this, "選擇待測 APP",
        ui->targetPathEdit->text(), "執行檔 (*.exe)");
    if (selected.isEmpty()) return;
    const QString previous = ui->targetPathEdit->text();
    ui->targetPathEdit->setText(QFileInfo(selected).absoluteFilePath());
    if (!saveSettings()) ui->targetPathEdit->setText(previous);
    setRunning(false);
    setStatus(targetAvailable() ? "準備就緒 — 請先試跑" : "待測 APP 無效 — 請重新選擇", targetAvailable() ? "idle" : "fail");
}

bool MainWindow::targetAvailable() const
{
    const QFileInfo target(ui->targetPathEdit->text());
    return !ui->targetPathEdit->text().isEmpty() && target.isAbsolute() && target.isFile()
        && target.suffix().compare("exe", Qt::CaseInsensitive) == 0;
}
bool MainWindow::validateBootFlow(QString &error) const
{
    if (!ui->useFlowCheck->isChecked()) return true;
    TestFlow flow;
    if (!loadFlow(m_bootFlowFile, flow, error) || !validateFlow(flow, error)) return false;
    if (QFileInfo(flow.executable).canonicalFilePath().compare(QFileInfo(ui->targetPathEdit->text()).canonicalFilePath(), Qt::CaseInsensitive) != 0) {
        error = "匯入流程的待測 APP 與主畫面不同，請重新匯入流程。"; return false;
    }
    return true;
}
void MainWindow::refreshBootFlowDisplay()
{
    ui->bootFlowPathEdit->setText(m_bootFlowFile.isEmpty() ? QString()
        : m_bootFlowName.isEmpty() ? QStringLiteral("已匯入流程（未記錄原檔名）") : m_bootFlowName);
    ui->bootFlowPathEdit->setToolTip(m_bootFlowFile.isEmpty() ? QString()
        : QStringLiteral("執行用流程快照：\n") + QDir::toNativeSeparators(m_bootFlowFile));
    ui->bootFlowPathEdit->setCursorPosition(0);
}

bool MainWindow::importBootFlow(const TestFlow &flow, const QString &sourceName)
{
    if (m_running) return false;
    QString error;
    if (!validateFlow(flow, error)) {QMessageBox::warning(this, "流程無法匯入", error); return false;}
    const QString folder = m_dir + "/boot_flows";
    const QString file = folder + "/flow_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")
        + '_' + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".json";
    if (!QDir().mkpath(folder) || !saveFlow(file, flow, error)) {QMessageBox::warning(this, "流程無法匯入", "無法保存匯入流程：" + error); return false;}
    const QString previousTarget = ui->targetPathEdit->text(), previousFile = m_bootFlowFile, previousName = m_bootFlowName;
    const bool previousUse = ui->useFlowCheck->isChecked(); QSignalBlocker blocker(ui->useFlowCheck);
    ui->targetPathEdit->setText(QFileInfo(flow.executable).absoluteFilePath()); m_bootFlowFile = file; m_bootFlowName = QFileInfo(sourceName).fileName(); refreshBootFlowDisplay(); ui->useFlowCheck->setChecked(true);
    if (!saveSettings()) {
        ui->targetPathEdit->setText(previousTarget); m_bootFlowFile = previousFile; m_bootFlowName = previousName; refreshBootFlowDisplay(); ui->useFlowCheck->setChecked(previousUse); QFile::remove(file); return false;
    }
    ui->countLabel->setText(QString("通用流程：已匯入 %1 個步驟").arg(flow.steps.size()));
    setRunning(false); setStatus("通用流程已匯入 — 請先試跑（不關機）"); return true;
}

bool MainWindow::checkEnvironment()
{
    QString error;
    if (!targetAvailable()) {
        error = "待測 APP 不存在或不是 EXE，請按瀏覽重新選擇：" + ui->targetPathEdit->text();
    } else if (!validateBootFlow(error)) {
        error = "開機後檢查流程無效：" + error;
    } else {
        QTemporaryFile probe(m_dir + "/.write_check_XXXXXX");
        if (!probe.open() || probe.write("check") != 5 || !probe.flush())
            error = "程式資料夾無法寫入，請搬到可寫入的位置：" + m_dir + "；" + probe.errorString();
        for (const QString &name : {QString("test_settings.ini"), QString("boot_cycle_detail.log"),
                                   QString("boot_cycle_results.csv"), QString("STOP"), QString("FAILED")}) {
            if (!error.isEmpty()) break;
            QFile file(m_dir + '/' + name);
            if (file.exists() && !file.open(QIODevice::ReadWrite))
                error = "無法寫入 " + name + "：" + file.errorString();
        }
    }
    if (error.isEmpty()) return true;
    setStatus("FAIL — 環境檢查未通過", "fail");
    ui->logEdit->appendPlainText(error);
    return false;
}

void MainWindow::refreshModeNote()
{
    ui->modeNote->setText(warmMode()
        ? "暖重開機：正式測試 PASS 後重新啟動 Windows，不呼叫外部控制器。"
        : "冷開機：UART 尚未設定，PASS 後只會關機，需外部恢復供電。");
}

void MainWindow::refreshStartup()
{
    const auto state = m_startup.queryStartup();
    QString text = QString::fromStdWString(state.text);
    if (state.valid && (QFile::exists(m_dir + "/STOP") || QFile::exists(m_dir + "/FAILED")))
        text += "；STOP／FAILED 存在，測試暫停";
    ui->startupLabel->setText(text);
    ui->installButton->setText(state.installed && !state.valid ? "更新自動啟動" : "安裝自動啟動");
    ui->installButton->setEnabled(!m_running && targetAvailable());
    ui->uninstallButton->setEnabled(!m_running && state.installed);
    ui->dryButton->setEnabled(!m_running && targetAvailable());
    ui->runButton->setEnabled(!m_running && targetAvailable());
}

void MainWindow::setRunning(bool running)
{
    m_running = running;
    ui->dryButton->setEnabled(!running);
    ui->runButton->setEnabled(!running);
    ui->stopButton->setEnabled(running);
    ui->bootModeCombo->setEnabled(!running);
    ui->browseTargetButton->setEnabled(!running);
    ui->targetPathEdit->setEnabled(!running);
    ui->clearButton->setEnabled(!running);
    ui->flowButton->setEnabled(!running);
    ui->bootFlowGroup->setEnabled(!running);
    refreshStartup();
}

void MainWindow::setStatus(const QString &text, const QString &state)
{
    ui->statusLabel->setText(text);
    const QString color = state == "pass" ? "#167744" : state == "fail" ? "#b52323"
        : state == "stopped" ? "#825619" : "#29445c";
    ui->statusLabel->setStyleSheet("color: " + color + ";");
}

void MainWindow::startTest(bool dry, bool automatic)
{
    if (m_running) return;
    if (automatic && (QFile::exists(m_dir + "/STOP") || QFile::exists(m_dir + "/FAILED"))) {
        setStatus("已停止 — STOP／FAILED 存在", "stopped");
        refreshStartup();
        return;
    }
    if (!checkEnvironment()) return;
    if (!automatic && !saveSettings()) return;
    const QFileInfo target(ui->targetPathEdit->text());
    if (!automatic) {
        for (const QString &name : {QString("STOP"), QString("FAILED")}) {
            QFile marker(m_dir + '/' + name);
            if (marker.exists() && !marker.remove()) {
                QMessageBox::critical(this, "無法開始", "無法清除 " + name + "：" + marker.errorString());
                return;
            }
        }
    }
    QFile log(m_dir + "/boot_cycle_detail.log");
    m_logOffset = log.exists() ? log.size() : 0;
    m_pending.clear();
    m_dry = dry; m_pass = m_fail = m_stopping = false;
    ui->modeLabel->setText(dry ? "試跑：不關機／不重開機"
        : warmMode() ? "正式測試：PASS 後重開機" : "正式測試：PASS 後關機");
    ui->countLabel->setText(ui->useFlowCheck->isChecked() ? "通用流程：等待檢查" : "CH0 audio：等待檢查");
    setStatus(dry ? "檢查中 — 準備執行檢查" : "檢查中 — 等待 20 秒再執行檢查");
    QStringList args{"--worker", "--target-exe", target.absoluteFilePath()};
    if (dry) args << "--dry-run";
    if (warmMode()) args << "--warm-reboot";
    if (ui->useFlowCheck->isChecked()) args << "--flow-file" << m_bootFlowFile;
    m_worker.setWorkingDirectory(m_dir);
    m_worker.setProgram(QCoreApplication::applicationFilePath());
    m_worker.setArguments(args);
    setRunning(true);
    m_logTimer.start();
    m_worker.start();
}

void MainWindow::requestStop()
{
    QFile marker(m_dir + "/STOP");
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || marker.write("Stop requested from Qt UI\r\n") < 0 || !marker.flush()) {
        QMessageBox::critical(this, "停止失敗", "無法建立 STOP：" + marker.errorString());
        return;
    }
    marker.close();
    m_stopping = true;
    setStatus("正在停止…", "stopped");
    ui->stopButton->setEnabled(false);
    refreshStartup();
}

void MainWindow::readLog()
{
    if (!m_running) return;
    QFile log(m_dir + "/boot_cycle_detail.log");
    if (!log.open(QIODevice::ReadOnly) || !log.seek(m_logOffset)) return;
    m_pending += log.readAll(); m_logOffset = log.pos();
    qsizetype newline;
    while ((newline = m_pending.indexOf('\n')) >= 0) {
        QByteArray line = m_pending.left(newline);
        if (line.endsWith('\r')) line.chop(1);
        showLine(QString::fromUtf8(line));
        countHistoryLine(QString::fromUtf8(line));
        m_pending.remove(0, newline + 1);
    }
    refreshStatistics();
}

void MainWindow::countHistoryLine(const QString &line)
{
    const qsizetype separator = line.indexOf(" | ");
    const QString message = separator >= 0 ? line.mid(separator + 3) : line;
    if (message == "Boot check started" || message == "Dry-run started") {
        if (message == "Boot check started") ++m_formalCount; else ++m_dryCount;
        m_historyResult = 0;
        ++m_results[0];
        return;
    }
    if (m_historyResult < 0) return;
    int result = m_historyResult;
    if (message.startsWith("FAIL:")) result = 2;
    else if ((message.startsWith("PASS: received=") || message.startsWith("PASS: flow=")) && result != 2) result = 1;
    else if ((message.startsWith("Stopped") || message.startsWith("STOP detected")
              || message.startsWith("Manual STOP detected")) && result == 0) result = 3;
    if (result != m_historyResult) {
        --m_results[m_historyResult]; ++m_results[result]; m_historyResult = result;
    }
}

void MainWindow::refreshStatistics()
{
    ui->statisticsLabel->setText(QString("累計 %1 次（正式 %2／試跑 %3）｜PASS %4｜FAIL %5｜停止 %6｜未完成 %7")
        .arg(m_formalCount + m_dryCount).arg(m_formalCount).arg(m_dryCount)
        .arg(m_results[1]).arg(m_results[2]).arg(m_results[3]).arg(m_results[0]));
}

void MainWindow::loadHistory()
{
    m_formalCount = m_dryCount = 0; m_results = {}; m_historyResult = -1; m_logOffset = 0;
    QFile log(m_dir + "/boot_cycle_detail.log");
    ui->logEdit->clear();
    if (log.open(QIODevice::ReadOnly)) {
        while (!log.atEnd()) countHistoryLine(QString::fromUtf8(log.readLine()).trimmed());
        // Bound display memory; all history still contributes to statistics.
        constexpr qint64 displayBytes = 512 * 1024;
        const qint64 offset = qMax(qint64(0), log.size() - displayBytes);
        log.seek(offset);
        if (offset > 0) log.readLine();
        ui->logEdit->setPlainText(QString::fromUtf8(log.readAll()));
        ui->logEdit->verticalScrollBar()->setValue(ui->logEdit->verticalScrollBar()->maximum());
        m_logOffset = log.size();
    } else if (log.exists()) {
        ui->logEdit->appendPlainText("無法讀取歷史紀錄：" + log.errorString());
    }
    m_pending.clear(); refreshStatistics();
}

bool MainWindow::clearRecordFiles(QString &error)
{
    for (const QString &name : {QString("boot_cycle_detail.log"), QString("boot_cycle_results.csv")}) {
        QFile file(m_dir + '/' + name);
        if (file.exists() && !file.remove()) {
            error = "無法清除 " + name + "：" + file.errorString();
            return false;
        }
    }
    return true;
}

void MainWindow::clearHistory()
{
    if (m_running) return;
    if (QMessageBox::question(this, "清除紀錄", "要刪除測試 CSV／log 並重設累計次數嗎？\n設定與 STOP／FAILED 停止標記會保留。",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    QString error;
    const bool cleared = clearRecordFiles(error);
    loadHistory();
    if (!cleared) {
        QMessageBox::critical(this, "紀錄未完全清除", error);
        return;
    }
    ui->countLabel->setText("CH0 audio：尚未檢查");
    ui->modeLabel->setText("尚未開始");
    setStatus("紀錄已清除 — 累計次數已歸零");
}

void MainWindow::showLine(const QString &line)
{
    ui->logEdit->appendPlainText(line);
    const qsizetype flowPrefix = line.indexOf(" | FLOW | ");
    if (flowPrefix >= 0) {
        if (!m_stopping && !m_fail && !m_pass) setStatus("流程檢查中 — " + line.mid(flowPrefix + 10));
        return;
    }
    static const QRegularExpression passPattern("PASS: received=([0-9]+)");
    const auto match = passPattern.match(line);
    if (line.contains("FAIL:")) {
        m_fail = true; setStatus("FAIL — 檢查失敗，已停止", "fail");
    } else if (line.contains("PASS: flow=")) {
        m_pass = true; ui->countLabel->setText("通用流程：全部步驟通過"); setStatus("PASS — 通用流程檢查通過", "pass");
    } else if (match.hasMatch()) {
        m_pass = true;
        ui->countLabel->setText("CH0 audio：已接收 " + match.captured(1) + " frames");
        setStatus("PASS — 已收到 CH0 audio frame", "pass");
    } else if (line.contains("Stopped") || line.contains("STOP detected")) {
        m_stopping = true; setStatus("已停止", "stopped");
    } else if (line.contains("Waiting 15 seconds")) {
        setStatus(warmMode() ? "PASS — 15 秒後重開機；可按停止" : "PASS — 15 秒後關機；可按停止", "pass");
    } else if (line.contains("Requesting Windows warm restart")) {
        setStatus("PASS — 正在要求 Windows 重開機", "pass");
    } else if (line.contains("Requesting normal Windows shutdown")) {
        setStatus("PASS — 正在要求 Windows 關機", "pass");
    } else if (line.contains("Waiting for application window")) {
        setStatus("檢查中 — 等待 APP 視窗");
    } else if (line.contains("Invoking Start")) {
        setStatus("檢查中 — 觸發 APP 的 Start");
    } else if (line.contains("Waiting for CH0 audio")) {
        setStatus("檢查中 — 等待 CH0 audio（最多 30 秒）");
    }
}

void MainWindow::finishTest(int exitCode, QProcess::ExitStatus status)
{
    readLog(); m_logTimer.stop(); setRunning(false);
    if (m_fail || exitCode != 0 || status == QProcess::CrashExit)
        setStatus("FAIL — 檢查或控制動作失敗", "fail");
    else if (m_stopping) setStatus(m_pass ? "已停止 — 本次檢查 PASS" : "已停止", "stopped");
    else if (m_pass && m_dry) setStatus("PASS — 試跑完成，不關機", "pass");
    else if (!m_pass) setStatus("未取得結果 — 請查看本次紀錄");
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_running) { requestStop(); event->ignore(); }
    else event->accept();
}

void MainWindow::showExisting()
{
    if (isMinimized()) showNormal(); else show();
    raise(); activateWindow();
}

bool MainWindow::smokeTest()
{
    const bool previousUseFlow = ui->useFlowCheck->isChecked();
    QSignalBlocker flowBlocker(ui->useFlowCheck); ui->useFlowCheck->setChecked(false);
    showLine("PASS: received=64");
    const bool pass = m_pass && ui->countLabel->text().contains("64 frames");
    showLine("FAIL: simulated smoke-test error");
    const bool fail = m_fail && ui->statusLabel->text().startsWith("FAIL");
    setRunning(true);
    const bool locked = !ui->bootModeCombo->isEnabled() && !ui->installButton->isEnabled()
        && !ui->uninstallButton->isEnabled() && !ui->browseTargetButton->isEnabled();
    setRunning(false);
    const bool idle = ui->dryButton->isEnabled() == targetAvailable()
        && ui->runButton->isEnabled() == targetAvailable() && !ui->stopButton->isEnabled();
    QSignalBlocker blocker(ui->bootModeCombo);
    ui->bootModeCombo->setCurrentIndex(1); refreshModeNote();
    showLine("Waiting 15 seconds before warm restart");
    const bool warm = ui->statusLabel->text().contains("重開機");
    ui->bootModeCombo->setCurrentIndex(0); refreshModeNote();
    showLine("Waiting 15 seconds before controller request and normal shutdown");
    const bool cold = ui->statusLabel->text().contains("後關機");
    const QString previous = ui->targetPathEdit->text();
    ui->targetPathEdit->clear(); setRunning(false);
    const bool unconfigured = !ui->dryButton->isEnabled() && !ui->runButton->isEnabled()
        && !ui->installButton->isEnabled() && ui->browseTargetButton->isEnabled()
        && !checkEnvironment() && m_worker.state() == QProcess::NotRunning;
    ui->targetPathEdit->setText(QCoreApplication::applicationFilePath()); setRunning(false);
    const bool configured = ui->dryButton->isEnabled() && ui->runButton->isEnabled()
        && ui->installButton->isEnabled() && checkEnvironment();
    ui->targetPathEdit->setText(previous); setRunning(false);
    const QString originalDir = m_dir;
    QTemporaryDir records(m_dir + "/records_check_XXXXXX");
    if (!records.isValid()) return false;
    m_dir = records.path();
    const QByteArray fixture = "2026-10-14 10:00:00 | Boot check started\n"
        "2026-10-14 10:00:01 | PASS: received=64\n"
        "2026-10-14 10:00:02 | FAIL: shutdown.exe failed\n"
        "2026-10-14 10:01:00 | Dry-run started\n"
        "2026-10-14 10:01:01 | PASS: received=64\n"
        "2026-10-14 10:02:00 | Boot check started\n"
        "2026-10-14 10:02:01 | Stopped before application launch\n"
        "2026-10-14 10:03:00 | Boot check started\n";
    bool fixtureWritten = true;
    for (const QString &name : {QString("boot_cycle_detail.log"), QString("boot_cycle_results.csv"),
                               QString("test_settings.ini"), QString("STOP"), QString("FAILED")}) {
        QFile file(m_dir + '/' + name);
        if (!file.open(QIODevice::WriteOnly) || file.write(name.endsWith(".log") ? fixture : QByteArray("preserve")) < 0)
            fixtureWritten = false;
    }
    loadHistory();
    const bool history = fixtureWritten && m_formalCount == 3 && m_dryCount == 1
        && m_results == std::array<quint64, 4>{1, 1, 1, 1} && ui->logEdit->toPlainText().contains("shutdown.exe failed");
    countHistoryLine("2026-10-15 12:00:00 | Dry-run started");
    countHistoryLine("2026-10-15 12:00:01 | FLOW | PASS: received=999");
    const bool partialNotCounted = m_results == std::array<quint64, 4>{2, 1, 1, 1};
    countHistoryLine("2026-10-15 12:00:02 | PASS: flow=example.json");
    const bool importedCounted = m_results == std::array<quint64, 4>{1, 2, 1, 1};
    setRunning(true);
    const bool clearLocked = !ui->clearButton->isEnabled();
    setRunning(false);
    QString clearError;
    const bool cleared = clearRecordFiles(clearError) && !QFile::exists(m_dir + "/boot_cycle_detail.log")
        && !QFile::exists(m_dir + "/boot_cycle_results.csv") && QFile::exists(m_dir + "/test_settings.ini")
        && QFile::exists(m_dir + "/STOP") && QFile::exists(m_dir + "/FAILED");
    loadHistory();
    const bool reset = m_formalCount == 0 && m_dryCount == 0 && m_results == std::array<quint64, 4>{};
    QTemporaryDir importedRecords(originalDir + "/flow_import_check_XXXXXX");
    if (!importedRecords.isValid()) return false;
    m_dir = importedRecords.path();
    const QString previousFlowFile = m_bootFlowFile, previousFlowName = m_bootFlowName;
    TestFlow imported; imported.executable = QCoreApplication::applicationFilePath();
    FlowStep delay; delay.type = "delay"; delay.name = "匯入測試"; delay.timeoutMs = 20; imported.steps = {delay};
    const bool importedOk = importBootFlow(imported, "test1.json");
    QString importedError; TestFlow saved;
    const bool importedValid = importedOk && validateBootFlow(importedError)
        && loadFlow(m_bootFlowFile, saved, importedError) && saved.steps.size() == 1 && saved.steps[0].name == delay.name;
    QSettings importedSettings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    const bool persisted = importedSettings.value("Test/UseFlow").toBool()
        && !QFileInfo(importedSettings.value("Test/FlowFile").toString()).isAbsolute()
        && importedSettings.value("Test/FlowName").toString() == "test1.json"
        && ui->bootFlowPathEdit->text() == "test1.json"
        && ui->bootFlowPathEdit->toolTip().contains(QDir::toNativeSeparators(m_bootFlowFile));
    m_pass = m_fail = false;
    showLine("2026-10-15 12:00:00 | FLOW | PASS: received=999");
    const bool noPartialPass = !m_pass && !m_fail;
    showLine("2026-10-15 12:00:01 | PASS: flow=example.json");
    const bool flowPass = m_pass && ui->countLabel->text().contains("全部步驟通過");
    m_dir = originalDir; ui->useFlowCheck->setChecked(previousUseFlow); m_bootFlowFile = previousFlowFile; m_bootFlowName = previousFlowName; refreshBootFlowDisplay();
    ui->targetPathEdit->setText(previous); loadHistory();
    return pass && fail && locked && idle && warm && cold && unconfigured && configured && history && clearLocked && cleared && reset
        && importedValid && persisted && noPartialPass && flowPass && partialNotCounted && importedCounted
        && ui->bootModeCombo->count() == 2;
}
