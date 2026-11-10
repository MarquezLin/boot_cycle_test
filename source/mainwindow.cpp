#include "mainwindow.h"
#include "flow_dialog.h"
#include "relay_controller.h"
#include "flow_monitor.h"
#include "flow_runner.h"
#include <QJsonDocument>
#include "ui_mainwindow.h"
#include <QApplication>
#include <windows.h>
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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>()),
      m_startup(std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString())),
      m_dir(QCoreApplication::applicationDirPath())
{
    ui->setupUi(this);
    m_flowMonitor = new FlowMonitor(this);
    ui->monitorLayout->addWidget(m_flowMonitor);
    ui->sideTabs->hide();
    connect(ui->engineeringCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        ui->sideTabs->setVisible(enabled);m_flowMonitor->setEngineering(enabled);
        if (enabled) ui->workspaceSplitter->setSizes({780, 380});
    });
    connect(ui->operatorImportButton, &QPushButton::clicked, ui->importFlowButton, &QPushButton::click);
    ui->workspaceSplitter->setStretchFactor(0, 1);
    ui->workspaceSplitter->setStretchFactor(1, 0);
    ui->workspaceSplitter->setSizes({780, 380});
    ui->runButton->setStyleSheet("QPushButton {background: #2563eb; color: white; border: none; border-radius: 6px; font-weight: 600;} QPushButton:disabled {background: #cbd5e1; color: #64748b;}");
    ui->stopButton->setStyleSheet("QPushButton {color: #b52323; font-weight: 600;} QPushButton:disabled {color: #94a3b8;}");
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    ui->continueOnFailureCheck->setChecked(settings.value("Test/ContinueOnFailure", false).toBool());
    ui->bootModeCombo->setCurrentIndex(settings.value("Test/BootMode", "cold").toString() == "warm" ? 1 : 0);
    ui->cycleLimitSpin->setValue(settings.value("Test/CycleLimit", 0).toInt());
    const QString flowFile = settings.value("Test/FlowFile").toString();
    if (!flowFile.isEmpty() && !settings.contains("Test/FlowName"))
        m_bootFlowFile = QDir(m_dir).absoluteFilePath(flowFile);
    refreshBootFlowDisplay();
    connect(ui->flowButton, &QPushButton::clicked, this, [this] {
        FlowDialog dialog(ui->targetPathEdit->text(), this, m_bootFlowFile);
        dialog.exec();
        refreshBootFlowDisplay(true);
    });
    connect(ui->importFlowButton, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, "匯入開機後檢查流程", m_bootFlowFile, "測試流程 (*.json)");
        if (!file.isEmpty()) importBootFlow(file);
    });
    connect(ui->continueOnFailureCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!saveSettings()) {QSignalBlocker block(ui->continueOnFailureCheck); ui->continueOnFailureCheck->setChecked(!checked);}
        refreshModeNote();
    });
    connect(ui->relayButton, &QPushButton::clicked, this, [this] {showRelaySettings(m_dir, this); refreshModeNote();});
    refreshModeNote();
    connect(ui->bootModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (!saveSettings()) {
            QSignalBlocker blocker(ui->bootModeCombo);
            ui->bootModeCombo->setCurrentIndex(index == 0 ? 1 : 0);
        }
        refreshModeNote();
    });
    ui->dryButton->setToolTip("與編輯器共用腳本設定；是否在擷取前啟用預覽視窗，由影像步驟決定。");
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
    connect(&m_worker,&QProcess::started,this,[this]{
        // Grant the child while this UI still owns the user's start click.
        // Activating the target before starting the child loses that permission.
        const auto pid=static_cast<DWORD>(m_worker.processId());
        const bool granted=pid!=0 && AllowSetForegroundWindow(pid);
        showLine(granted?"Capture activation: worker foreground permission granted"
            :"Capture activation: foreground permission was not granted; acquisition will verify the actual target window");
    });
    connect(&m_worker, &QProcess::finished, this, &MainWindow::finishTest);
    connect(&m_worker, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        m_fail = true;
        showLine("FAIL: " + m_worker.errorString());
        m_flowMonitor->finish(false, false, m_worker.errorString());
        m_logTimer.stop();
        setRunning(false);
    });
    connect(&m_logTimer, &QTimer::timeout, this, &MainWindow::readLog);
    connect(&m_startupTimer, &QTimer::timeout, this, &MainWindow::refreshStartup);
    m_logTimer.setInterval(250);
    m_startupTimer.start(1000);
    setRunning(false);
    loadHistory();
    setStatus(targetAvailable() ? "準備就緒 — 請先試跑" : "尚未選擇流程 — 請先匯入 JSON");
}

MainWindow::~MainWindow() = default;

bool MainWindow::warmMode() const { return ui->bootModeCombo->currentIndex() == 1; }

bool MainWindow::saveSettings()
{
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    settings.setValue("Test/ContinueOnFailure", ui->continueOnFailureCheck->isChecked());
    settings.setValue("Test/BootMode", warmMode() ? "warm" : "cold");
    settings.setValue("Test/CycleLimit", ui->cycleLimitSpin->value());
    settings.remove("Test/TargetExe");
    settings.remove("Test/UseFlow");
    settings.setValue("Test/FlowFile", m_bootFlowFile.isEmpty() ? QString() : QDir(m_dir).relativeFilePath(m_bootFlowFile));
    settings.remove("Test/FlowName");
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    QMessageBox::critical(this, "設定保存失敗", "無法保存模式與待測 APP 路徑，請檢查資料夾寫入權限。");
    return false;
}

bool MainWindow::targetAvailable() const
{
    return QFileInfo(m_bootFlowFile).isFile();
}

bool MainWindow::validateBootFlow(QString &error) const
{
    if (m_bootFlowFile.isEmpty()) {error = "請先匯入已保存的 JSON 流程"; return false;}
    TestFlow flow;
    return loadFlow(m_bootFlowFile, flow, error) && validateFlow(flow, error);
}

void MainWindow::refreshBootFlowDisplay(bool preserveResults)
{
    // The monitor describes the completed run, not an unexecuted edited plan.
    // Reload the new plan only when starting another run or importing a flow.
    if(preserveResults && m_flowMonitor->hasResults()){
        ui->modeLabel->setText("保留上次測試結果；編輯內容將於下次測試套用");
        return;
    }

    ui->bootFlowPathEdit->setText(QFileInfo(m_bootFlowFile).fileName());
    ui->bootFlowPathEdit->setToolTip(QDir::toNativeSeparators(m_bootFlowFile));
    ui->bootFlowPathEdit->setCursorPosition(0);
    TestFlow flow;
    QString error;
    const bool loaded = loadFlow(m_bootFlowFile, flow, error);
    ui->targetPathEdit->setText(loaded ? flow.executable : QString());
    ui->targetPathEdit->setToolTip(loaded ? flow.executable : QString());
    m_flowMonitor->setPlan(loaded ? flowMonitorPlan(flow)
        : QJsonObject{{"error", m_bootFlowFile.isEmpty() ? QString("請先匯入 JSON 流程。") : error}});
    ui->pageTitle->setText(loaded && !flow.name.isEmpty() ? flow.name : QString("開機循環測試"));
    ui->pageTitle->setToolTip(loaded ? flow.name + (flow.version.isEmpty() ? QString() : " · 版本 " + flow.version) : QString());
    ui->countLabel->setText(loaded ? QString("%1 個測試項目%2").arg(m_flowMonitor->testItemCount()).arg(flow.version.isEmpty() ? QString() : " · 腳本版本 " + flow.version) : "請先載入測試腳本");
}

bool MainWindow::importBootFlow(const QString &file)
{
    if (m_running) return false;
    TestFlow flow;
    QString error;
    if (!loadFlow(file, flow, error) || !validateFlow(flow, error)) {
        QMessageBox::warning(this, "流程無法匯入", error);
        return false;
    }
    const QString previous = m_bootFlowFile;
    m_bootFlowFile = QFileInfo(file).absoluteFilePath();
    if (!saveSettings()) {
        m_bootFlowFile = previous;
        return false;
    }
    refreshBootFlowDisplay();
    ui->countLabel->setText(QString("已載入 %1 個測試項目%2").arg(m_flowMonitor->testItemCount())
        .arg(flow.version.isEmpty() ? QString() : " · 腳本版本 " + flow.version));
    setRunning(false);
    setStatus("JSON 流程已匯入 — 請先試跑（不關機）");
    return true;
}

bool MainWindow::checkEnvironment()
{
    QString error;
    if (!validateBootFlow(error)) {
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
    const auto relay = loadRelaySettings(m_dir);
    if (error.isEmpty() && !warmMode() && relay.enabled) relaySettingsValid(relay, error);
    if (error.isEmpty()) return true;
    setStatus("FAIL — 環境檢查未通過", "fail");
    ui->logEdit->appendPlainText(error);
    return false;
}

void MainWindow::refreshModeNote()
{
    const auto relay = loadRelaySettings(m_dir);
    if (!warmMode() && relay.enabled) {
        ui->modeNote->setText(QString("RS485 冷開機：檢查%1後等待 %2 秒，再直接斷電 %3 秒後復電；不執行 Windows 正常關機。")
            .arg(ui->continueOnFailureCheck->isChecked() ? "完成（含 FAIL）" : "PASS")
            .arg(relay.delaySeconds).arg(relay.ticks / 10.0));
        m_flowMonitor->setTailDescription(ui->modeNote->text());
        return;
    }
    const QString outcome = ui->continueOnFailureCheck->isChecked() ? "檢查完成（含 FAIL）" : "PASS";
    ui->modeNote->setText(QString(warmMode()
        ? "暖重開機：正式測試 PASS 後重新啟動 Windows，不呼叫外部控制器。"
        : "冷開機：UART 尚未設定，PASS 後只會關機，需外部恢復供電。").replace("PASS", outcome));
    m_flowMonitor->setTailDescription(ui->modeNote->text());
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
    ui->relayButton->setEnabled(!running);
    ui->continueOnFailureCheck->setEnabled(!running);
    ui->cycleLimitSpin->setEnabled(!running);
    ui->targetPathEdit->setEnabled(!running);
    ui->clearButton->setEnabled(!running);
    ui->flowButton->setEnabled(!running);
    ui->bootFlowGroup->setEnabled(!running);
    ui->operatorImportButton->setEnabled(!running);
    refreshStartup();
}

void MainWindow::setStatus(const QString &text, const QString &state)
{
    QString displayed = text;
    if (!ui->engineeringCheck->isChecked()) {
        if (text.startsWith("FAIL —")) displayed = "FAIL — 測試未通過，請查看下方項目與排查提示";
        else if (text.startsWith("流程檢查中 —")) displayed = "測試進行中 — 請依下方項目提示操作";
    }
    ui->statusLabel->setText(displayed);
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
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    quint64 lastFormalCycle = settings.value("Run/LastFormalCycle", 0).toULongLong();
    if (!dry && (!automatic || !settings.contains("Run/LastFormalCycle"))) {
        loadHistory();
        lastFormalCycle = ui->cycleLimitSpin->value() == 0 ? 0
            : m_formalCount + static_cast<quint64>(ui->cycleLimitSpin->value());
        settings.setValue("Run/LastFormalCycle", lastFormalCycle);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            setStatus("FAIL — 無法保存正式測試輪數", "fail");
            return;
        }
    }
    refreshBootFlowDisplay();
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
    m_flowMonitor->setTailDescription(dry ? "本次為試跑：保存報告，不送電源命令、不關機。" : ui->modeNote->text());
    m_flowMonitor->begin(dry ? "試跑準備中；依腳本執行，不執行電源動作" : "啟動等待 20 秒，之後執行 JSON 流程");
    ui->modeLabel->setText(dry ? "試跑：不關機／不重開機"
        : warmMode() ? "正式測試：暖重開機" : "正式測試：冷開機");
    ui->countLabel->setText("JSON 流程：等待檢查");
    setStatus(dry ? "檢查中 — 準備執行檢查" : "檢查中 — 等待 20 秒再執行檢查");
    QStringList args{"--worker", "--target-exe", target.absoluteFilePath()};
    if (ui->continueOnFailureCheck->isChecked()) args << "--continue-on-failure";
    if (dry) args << "--dry-run";
    if (warmMode()) args << "--warm-reboot";
    args << "--flow-file" << m_bootFlowFile;
    if (!dry && lastFormalCycle != 0)
        args << "--last-formal-cycle" << QString::number(lastFormalCycle);
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
    m_flowMonitor->requestStop();
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
    for (const QString &name : {QString("boot_cycle_report.html"), QString("boot_cycle_detail.log"),
                               QString("boot_cycle_results.csv")}) {
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
    if (QMessageBox::question(this, "清除紀錄", "要刪除測試 CSV、log、HTML 報告並重設累計次數嗎？\n設定與 STOP／FAILED 停止標記會保留。",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    QString error;
    const bool cleared = clearRecordFiles(error);
    loadHistory();
    if (!cleared) {
        QMessageBox::critical(this, "紀錄未完全清除", error);
        return;
    }
    ui->countLabel->setText("JSON 流程：尚未檢查");
    ui->modeLabel->setText("尚未開始");
    refreshBootFlowDisplay();
    setStatus("紀錄已清除 — 累計次數已歸零");
}

void MainWindow::showLine(const QString &line)
{
    const qsizetype eventPrefix = line.indexOf(" | FLOW_EVENT ");
    if (eventPrefix >= 0) {
        const auto document = QJsonDocument::fromJson(line.mid(eventPrefix + 14).toUtf8());
        if (document.isObject()) {
            const auto event = document.object();m_flowMonitor->applyEvent(event);
            if (event["event"] == "plan") {
                if (!event["name"].toString().isEmpty()) ui->pageTitle->setText(event["name"].toString());
                ui->countLabel->setText(QString("本次共 %1 個測試項目%2").arg(m_flowMonitor->testItemCount())
                    .arg(event["version"].toString().isEmpty() ? QString() : " · 腳本版本 " + event["version"].toString()));
            }
        }
        return;
    }
    ui->logEdit->appendPlainText(line);
    const qsizetype flowPrefix = line.indexOf(" | FLOW | ");
    if (flowPrefix >= 0) {
        if (!m_stopping && !m_fail && !m_pass) setStatus("流程檢查中 — " + line.mid(flowPrefix + 10));
        return;
    }
    static const QRegularExpression passPattern("PASS: received=([0-9]+)");
    const auto match = passPattern.match(line);
    if (line.contains("FAIL:")) {
        m_fail = true; setStatus("FAIL — " + line.mid(line.indexOf("FAIL:") + 5), "fail");
    } else if (line.contains("CONTINUE_AFTER_FAILURE:")) {
        setStatus(m_dry ? "FAIL — 已記錄；試跑不關機" : "FAIL — 已記錄，將繼續下一輪", "fail");
    } else if (line.contains("PASS: flow=")) {
        m_flowMonitor->phase(m_dry ? "全部檢查通過，保存本次試跑報告" : "全部檢查通過，準備後續動作");
        m_pass = true; ui->countLabel->setText("所有測試項目已通過"); setStatus("PASS — 全部測試項目通過", "pass");
    } else if (match.hasMatch()) {
        m_pass = true;
        ui->countLabel->setText("CH0 audio：已接收 " + match.captured(1) + " frames");
        setStatus("PASS — 已收到 CH0 audio frame", "pass");
    } else if (line.contains("Stopped") || line.contains("STOP detected")) {
        m_stopping = true; setStatus("已停止", "stopped");
    } else if (line.contains("RELAY COUNTDOWN:")) {
        m_flowMonitor->phase("斷電前等待中；現在仍可停止");
        setStatus((m_fail ? QString("FAIL 已記錄 — ") : QString()) + "斷電前倒數中；可按停止", m_fail ? "fail" : "idle");
    } else if (line.contains("RELAY TX")) {
        m_flowMonitor->phase("已送出切換命令；不代表已確認實際斷電或復電");
        setStatus("已送出繼電器切換命令；模組計時復電", m_fail ? "fail" : "idle");
    } else if (line.contains("Waiting 15 seconds")) {
        m_flowMonitor->phase(warmMode() ? "等待 15 秒後要求 Windows 重開機" : "等待 15 秒後要求 Windows 關機");
        setStatus((m_fail ? QString("FAIL 已記錄 — ") : QString("PASS — "))
            + (warmMode() ? "15 秒後重開機；可按停止" : "15 秒後關機；可按停止"), m_fail ? "fail" : "pass");
    } else if (line.contains("Requesting Windows warm restart")) {
        m_flowMonitor->phase("已要求 Windows 重開機；不代表已完成重啟");
        setStatus(m_fail ? "FAIL 已記錄 — 正在要求 Windows 重開機" : "PASS — 正在要求 Windows 重開機", m_fail ? "fail" : "pass");
    } else if (line.contains("Requesting normal Windows shutdown")) {
        m_flowMonitor->phase("已要求 Windows 關機；不代表已完成關機");
        setStatus(m_fail ? "FAIL 已記錄 — 正在要求 Windows 關機" : "PASS — 正在要求 Windows 關機", m_fail ? "fail" : "pass");
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
    if (exitCode == 0 && status == QProcess::NormalExit && !m_dry) {
        QFile marker(m_dir + "/STOP");
        if (marker.open(QIODevice::ReadOnly) && marker.readLine().trimmed() == "Cycle limit reached") {
            setStatus("已達設定輪數 — 測試報告已產出", m_fail ? "fail" : "stopped");
            m_flowMonitor->phase("已達設定輪數；保存報告，不執行電源動作");
            m_flowMonitor->finish(true, false, "已達設定輪數；測試報告已產出");
            return;
        }
    }
    if (m_fail && m_stopping) setStatus("已停止 — 本次檢查 FAIL，已記錄", "fail");
    else if (m_fail && m_dry) setStatus("FAIL — 試跑完成，已記錄，不關機", "fail");
    else if (exitCode == 0 && status == QProcess::NormalExit && !m_dry && !warmMode()
             && loadRelaySettings(m_dir).enabled && !m_stopping)
        setStatus(m_fail ? "FAIL 已記錄 — 模組已接受斷電／復電命令" : "模組已接受斷電／復電命令", m_fail ? "fail" : "pass");
    else if (m_fail && exitCode == 0 && status == QProcess::NormalExit)
        setStatus("FAIL — 已記錄，已要求電源動作以繼續下一輪", "fail");
    else if (m_fail || exitCode != 0 || status == QProcess::CrashExit)
        setStatus("FAIL — 檢查或控制動作失敗", "fail");
    else if (m_stopping) setStatus(m_pass ? "已停止 — 本次檢查 PASS" : "已停止", "stopped");
    else if (m_pass && m_dry) setStatus("PASS — 試跑完成，不關機", "pass");
    else if (!m_pass) setStatus("未取得結果 — 請查看本次紀錄");
    m_flowMonitor->finish(exitCode == 0 && status == QProcess::NormalExit && (m_pass || m_fail),
        m_stopping, ui->statusLabel->text());
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
    const bool operatorDefault = !ui->engineeringCheck->isChecked() && ui->sideTabs->isHidden();
    const bool previousContinue = ui->continueOnFailureCheck->isChecked();
    QSignalBlocker policyBlocker(ui->continueOnFailureCheck);
    showLine("PASS: received=64");
    const bool pass = m_pass && ui->countLabel->text().contains("64 frames");
    showLine("FAIL: simulated smoke-test error");
    const bool fail = m_fail && ui->statusLabel->text().startsWith("FAIL");
    setRunning(true);
    const bool locked = !ui->bootModeCombo->isEnabled() && !ui->installButton->isEnabled()
        && !ui->uninstallButton->isEnabled() && !ui->cycleLimitSpin->isEnabled()
        && !ui->continueOnFailureCheck->isEnabled() && !ui->operatorImportButton->isEnabled();
    ui->engineeringCheck->setChecked(true);
    const bool engineeringLocked = ui->sideTabs->isVisible() && !ui->bootModeCombo->isEnabled();
    ui->engineeringCheck->setChecked(false);
    setRunning(false);
    const bool idle = ui->dryButton->isEnabled() == targetAvailable()
        && ui->runButton->isEnabled() == targetAvailable() && !ui->stopButton->isEnabled();
    QSignalBlocker blocker(ui->bootModeCombo);
    ui->bootModeCombo->setCurrentIndex(1); refreshModeNote();
    showLine("Waiting 15 seconds before warm restart");
    const bool warm = ui->statusLabel->text().contains("重開機")
        && ui->statusLabel->text().startsWith("FAIL 已記錄");
    ui->bootModeCombo->setCurrentIndex(0); refreshModeNote();
    showLine("Waiting 15 seconds before controller request and normal shutdown");
    const bool cold = ui->statusLabel->text().contains("後關機");
    const QString previousFlowFile = m_bootFlowFile;
    m_bootFlowFile.clear();
    setRunning(false);
    const bool unconfigured = !ui->dryButton->isEnabled() && !ui->runButton->isEnabled()
        && !ui->installButton->isEnabled() && ui->importFlowButton->isEnabled()
        && !checkEnvironment() && m_worker.state() == QProcess::NotRunning;
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
    ui->continueOnFailureCheck->setChecked(true);
    TestFlow imported; imported.executable = QCoreApplication::applicationFilePath();
    FlowStep delay; delay.type = "delay"; delay.name = "匯入測試"; delay.timeoutMs = 20; imported.steps = {delay};
    imported.name = "分類保存後的工廠測試"; imported.version = "1.2.1";
    imported.testItems = {{"saved", "確認設備", "保持連接", "檢查中", "檢查電源"}};
    imported.steps[0].testItemId = "saved";
    QString importedError; TestFlow saved;
    const QString source = m_dir + "/test1.json";
    const bool importedOk = saveFlow(source, imported, importedError) && importBootFlow(source);
    const bool importedValid = importedOk && validateBootFlow(importedError)
        && loadFlow(m_bootFlowFile, saved, importedError) && saved.steps.size() == 1 && saved.steps[0].name == delay.name
        && ui->pageTitle->text() == imported.name && m_flowMonitor->testItemCount() == 1
        && saved.testItems.size() == 1 && saved.steps[0].testItemId == "saved";
    QSettings importedSettings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    const bool persisted = importedSettings.value("Test/ContinueOnFailure").toBool()
        && !importedSettings.contains("Test/UseFlow")
        && !QFileInfo(importedSettings.value("Test/FlowFile").toString()).isAbsolute()
        && importedSettings.value("Test/FlowFile").toString() == "test1.json"
        && !QDir(m_dir + "/boot_flows").exists()
        && ui->bootFlowPathEdit->text() == "test1.json"
        && ui->bootFlowPathEdit->toolTip().contains(QDir::toNativeSeparators(m_bootFlowFile));
    m_pass = m_fail = false;
    showLine("2026-10-15 12:00:00 | FLOW | PASS: received=999");
    const bool noPartialPass = !m_pass && !m_fail;
    showLine("2026-10-15 12:00:01 | PASS: flow=example.json");
    const bool flowPass = m_pass && ui->countLabel->text().contains("所有測試項目已通過");
    ui->continueOnFailureCheck->setChecked(previousContinue);
    m_dir = originalDir; m_bootFlowFile = previousFlowFile; refreshBootFlowDisplay();
    loadHistory();
    const bool monitor = m_flowMonitor->selfTest();
    refreshBootFlowDisplay(true);
    const bool resultRetained=m_flowMonitor->hasResults() && m_flowMonitor->rowState(2)=="fail";
    refreshBootFlowDisplay();
    const bool nextPlanCleared=!m_flowMonitor->hasResults();
    return operatorDefault && engineeringLocked && monitor && resultRetained && nextPlanCleared && pass && fail && locked && idle && warm && cold && unconfigured && history && clearLocked && cleared && reset
        && importedValid && persisted && noPartialPass && flowPass && partialNotCounted && importedCounted
        && ui->bootModeCombo->count() == 2;
}

bool MainWindow::monitorPreview(const QString &state)
{
    TestFlow flow;
    FlowStep launch; launch.type = "launch"; launch.name = "啟動待測 APP";
    FlowStep wait; wait.type = "waitWindow"; wait.name = "等待主視窗"; wait.timeoutMs = 60000;
    FlowStep click; click.type = "clickUIA"; click.name = "按下 CH0 Start"; click.selector = "Start";
    FlowStep read; read.type = "readUIA"; read.name = "確認 CH0 audio 收到 frame";
    read.condition = "gt"; read.expected = "0"; read.timeoutMs = 30000;
    flow.steps = {launch, wait, click, read};
    flow.name = "擷取卡出廠測試"; flow.version = "1.0";
    flow.testItems = {{"app", "啟動測試程式", "確認設備已接妥，保持連線。", "正在啟動並等待程式就緒。", "請確認待測程式與設備驅動已安裝。"},
        {"start", "開始訊號擷取", "保持訊號線連接，不需操作。", "正在啟動擷取。", "請檢查設備與訊號源。"},
        {"audio", "確認音訊資料接收", "請保持訊號源開啟及 HDMI 線連接，不需操作。", "正在等待音訊資料，最長約 30 秒。", "確認訊號源已開啟音訊輸出，並檢查 HDMI 線。仍失敗時請通知工程人員。"}};
    flow.steps[0].testItemId = flow.steps[1].testItemId = "app";
    flow.steps[2].testItemId = "start"; flow.steps[3].testItemId = "audio";
    ui->pageTitle->setText(flow.name);ui->countLabel->setText("腳本版本 1.0 · 畫面示範");
    m_flowMonitor->setPlan(flowMonitorPlan(flow));
    m_flowMonitor->setTailDescription("試跑：保存報告，不關機。");
    ui->bootFlowPathEdit->setText("示範流程.json");ui->targetPathEdit->setText("待測 APP.exe");
    ui->modeLabel->setText("畫面示範 · 未執行測試");
    m_flowMonitor->begin("準備完成");
    for(int i=0;i<3;++i) {
        showLine(QString("demo | FLOW_EVENT {\"event\":\"start\",\"index\":%1}").arg(i));
        showLine(QString("demo | FLOW_EVENT {\"event\":\"finish\",\"index\":%1,\"result\":\"PASS\",\"value\":\"已完成\"}").arg(i));
    }
    showLine("demo | FLOW_EVENT {\"event\":\"start\",\"index\":3}");
    showLine("demo | FLOW_EVENT {\"event\":\"observed\",\"index\":3,\"value\":\"目前 received=0 frames；預期 > 0，持續等待中\"}");
    if(state == "fail") {
        showLine("demo | FLOW_EVENT {\"event\":\"finish\",\"index\":3,\"result\":\"FAIL\",\"value\":\"30 秒內 received 仍為 0；尚未符合 > 0\"}");
        m_flowMonitor->finish(false, false, "檢查失敗");setStatus("FAIL — 停在步驟 4：尚未收到 CH0 audio frame", "fail");
    } else if(state == "stopped") {
        m_flowMonitor->requestStop();m_flowMonitor->finish(false, true, "操作者停止測試");setStatus("已停止 — 保留步驟 4 的最後狀態", "stopped");
    } else setStatus("測試進行中 — 確認音訊資料接收");
    ui->countLabel->setText("已通過 2 / 3 個測試項目 · 腳本版本 1.0 · 示範資料，未執行實際測試");
    QApplication::processEvents();
    return grab().save(m_dir + "/monitor_" + state + ".png");
}
