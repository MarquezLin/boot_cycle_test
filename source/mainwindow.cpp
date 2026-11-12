#include "mainwindow.h"
#include "flow_dialog.h"
#include "relay_controller.h"
#include "flow_monitor.h"
#include "flow_runner.h"
#include "boot_flow_runner.h"
#include <QJsonDocument>
#include "ui_mainwindow.h"
#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
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
#include <QUuid>
#include <QUrlQuery>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QHostInfo>
#include <QJsonObject>
#include <QDir>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(std::make_unique<Ui::MainWindow>()),
      m_startup(std::filesystem::path(QCoreApplication::applicationFilePath().toStdWString())),
      m_dir(QCoreApplication::applicationDirPath())
{
    ui->setupUi(this);
    ui->modeNote->hide();
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
    ui->cycleLimitSpin->setKeyboardTracking(false);
    ui->cycleLimitSpin->setToolTip("變更輪數會儲存到目前的 JSON 腳本。");
    connect(ui->cycleLimitSpin, &QSpinBox::valueChanged, this, [this](int limit) {
        if (m_bootFlowFile.isEmpty()) return;
        QString error;
        if (!saveFlowCycleLimit(m_bootFlowFile, limit, error)) {
            refreshBootFlowDisplay(true);
            QMessageBox::warning(this, "無法儲存正式測試輪數", error);
        }
    });
    connect(ui->flowButton, &QPushButton::clicked, this, [this] {
        FlowDialog dialog(ui->targetPathEdit->text(), this, m_bootFlowFile);
        dialog.exec();
        if (m_running) return;
        const QString selectedFile = dialog.selectedFile();
        if (!selectedFile.isEmpty() && QFileInfo(selectedFile) != QFileInfo(m_bootFlowFile)) {
            importBootFlow(selectedFile);
            return;
        }
        refreshBootFlowDisplay(true);
        setRunning(false);
    });
    connect(ui->importFlowButton, &QPushButton::clicked, this, [this] {
        QString examples=QDir::cleanPath(QDir(m_dir).absoluteFilePath("../examples"));
        if(!QDir(examples).exists())examples=QDir::cleanPath(QDir(m_dir).absoluteFilePath("../../../examples"));
        if(!QDir(examples).exists())examples=m_dir;
        const QString file = QFileDialog::getOpenFileName(this, "載入測試腳本", examples, "測試流程 (*.json)");
        if (!file.isEmpty()) importBootFlow(file);
    });
    connect(ui->continueOnFailureCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (!saveSettings()) {QSignalBlocker block(ui->continueOnFailureCheck); ui->continueOnFailureCheck->setChecked(!checked);}
        refreshModeNote();
    });
    connect(ui->relayButton, &QPushButton::clicked, this, [this] {
        showRelayAgentSettings(m_dir, this, [this] { return discoveredAgents(); });
        refreshModeNote();
    });
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
    connect(ui->stopButton, &QPushButton::clicked, this, [this] { requestStop(); });
    connect(ui->clearButton, &QPushButton::clicked, this, &MainWindow::manageReports);
    connect(ui->resetStatisticsButton, &QPushButton::clicked, this, &MainWindow::resetStatistics);
    connect(ui->exportReportButton, &QPushButton::clicked, this, &MainWindow::manageReports);
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
    connect(&m_logTimer, &QTimer::timeout, this, &MainWindow::readLog);
    connect(&m_startupTimer, &QTimer::timeout, this, &MainWindow::refreshStartup);
    m_logTimer.setInterval(250);
    m_startupTimer.start(1000);
    setRunning(false);
    loadHistory();
    setStatus(targetAvailable()?"準備就緒 — 請先試跑":m_bootFlowFile.isEmpty()?"尚未選擇流程 — 請先匯入 JSON":"腳本已載入，可查看與編輯；待測 APP 或執行設定尚未就緒");
}

void MainWindow::startRelaySession()
{
    m_relayClock.start();
    m_discovery = new QUdpSocket(this);
    m_discovery->bind(QHostAddress(QHostAddress::AnyIPv4), quint16(0));
    m_relayNetwork = new QNetworkAccessManager(this);
    m_relayNetwork->setProxy(QNetworkProxy::NoProxy);
    connect(m_discovery, &QUdpSocket::readyRead, this, [this] {
        while (m_discovery->hasPendingDatagrams()) {
            const auto packet = m_discovery->receiveDatagram(256);
            if (packet.senderPort() != relayDiscoveryPort) continue;
            const auto reply = QJsonDocument::fromJson(packet.data()).object();
            const QHostAddress address(reply.value("address").toString());
            const int port = reply.value("port").toInt();
            if (reply.value("protocol").toString() != QLatin1String(relayDiscoveryRequest)
                || address.protocol() != QAbstractSocket::IPv4Protocol || address != packet.senderAddress()
                || port < 1 || port > 65535) continue;
            m_discoveredAgents[QString("http://%1:%2").arg(address.toString()).arg(port)] = m_relayClock.elapsed();
        }
    });
    auto *heartbeat = new QTimer(this);
    connect(heartbeat, &QTimer::timeout, this, &MainWindow::pollRelayAgent);
    heartbeat->start(2000);
    pollRelayAgent();
}

MainWindow::~MainWindow() = default;

QStringList MainWindow::discoveredAgents() const
{
    QStringList result;
    if (!m_relayClock.isValid()) return result;
    for (auto it = m_discoveredAgents.cbegin(); it != m_discoveredAgents.cend(); ++it)
        if (m_relayClock.elapsed() - it.value() < 7000) result.append(it.key());
    result.sort();
    return result;
}

QString MainWindow::relayAgentUrl() const
{
    if (m_running && !m_runningAgent.isEmpty()) return m_runningAgent;
    const auto configured = loadRelaySettings(m_dir).agentUrl;
    if (!configured.isEmpty()) return configured;
    const auto discovered = discoveredAgents();
    return discovered.size() == 1 ? discovered.first() : QString();
}

void MainWindow::pollRelayAgent()
{
    const auto now = m_relayClock.elapsed();
    for (auto it = m_discoveredAgents.begin(); it != m_discoveredAgents.end();) {
        if (now - it.value() >= 7000) it = m_discoveredAgents.erase(it);
        else ++it;
    }
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp)) continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol
                || entry.ip().isLoopback() || entry.prefixLength() < 1 || entry.prefixLength() > 30) continue;
            const QHostAddress broadcast(entry.ip().toIPv4Address() | ~entry.netmask().toIPv4Address());
            m_discovery->writeDatagram(relayDiscoveryRequest, broadcast, relayDiscoveryPort);
        }
    }
    if (m_relayPolling) return;
    const QString agent = relayAgentUrl();
    auto settings = loadRelaySettings(m_dir);
    settings.agentUrl = agent;
    QString error;
    if (!relayAgentSettingsValid(settings, error)) return;

    QUrl url(agent);
    url.setPath("/report");
    QUrlQuery query;
    query.addQueryItem("name", QHostInfo::localHostName().left(64));
    QString state = "idle";
    if (m_shutdownRequested) state = "shutdown";
    else if (m_running) state = m_stopping ? "stopping" : m_relayReconnecting ? "reconnecting" : "running";
    else if (m_stopping) state = "stopped";
    else if (m_fail) state = "failed";
    else if (m_pass) state = "passed";
    query.addQueryItem("state", state);
    const QSettings run(m_dir + "/test_settings.ini", QSettings::IniFormat);
    const bool hasRun = run.contains("Run/LastFormalCycle");
    const auto last = run.value("Run/LastFormalCycle").toULongLong();
    const auto base = run.value("Run/FirstFormalCount").toULongLong();
    if (!hasRun) {
        query.addQueryItem("tested", "0");
        query.addQueryItem("target", QString::number(ui->cycleLimitSpin->value()));
    } else if (run.contains("Run/FirstFormalCount") && base <= m_formalCount && (last == 0 || last >= base)) {
        query.addQueryItem("tested", QString::number(m_formalCount - base));
        query.addQueryItem("target", QString::number(last == 0 ? 0 : last - base));
    }
    if (m_lastRelayCommandAgent == agent) {
        query.addQueryItem("ack", m_lastRelayCommand);
        query.addQueryItem("result", m_lastRelayResult);
    }
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(2000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = m_relayNetwork->post(request, QByteArray());
    m_relayPolling = true;
    connect(reply, &QNetworkReply::finished, this, [this, reply, agent] {
        reply->deleteLater();
        m_relayPolling = false;
        if (reply->error() != QNetworkReply::NoError || relayAgentUrl() != agent) return;
        const auto data = QJsonDocument::fromJson(reply->readAll()).object();
        const QString id = data.value("id").toString();
        const QString command = data.value("command").toString();
        if (id.isEmpty() || (command != "start" && command != "stop")
            || (m_lastRelayCommandAgent == agent && m_lastRelayCommand == id)) return;
        m_lastRelayCommandAgent = agent;
        m_lastRelayCommand = id;
        m_lastRelayResult = "failed";
        if (command == "start") {
            if (!m_running) startTest(false);
            if (m_running && !m_stopping && !m_dry) m_lastRelayResult = "started";
        } else {
            requestStop(true);
            if (m_stopping) m_lastRelayResult = "stopping";
        }
    });
}

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
    QString error;return validateBootFlow(error);
}

bool MainWindow::validateBootFlow(QString &error) const
{
    if (m_bootFlowFile.isEmpty()) {error = "請先匯入已保存的 JSON 流程"; return false;}
    TestFlow flow;
    return loadFlow(m_bootFlowFile, flow, error) && validateFlow(flow, error);
}

void MainWindow::refreshBootFlowDisplay(bool preserveResults)
{
    TestFlow flow;
    QString error;
    const bool loaded = loadFlow(m_bootFlowFile, flow, error);
    {
        const QSignalBlocker blocker(ui->cycleLimitSpin);
        const QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
        ui->cycleLimitSpin->setValue(loaded && flow.formalCycleLimit >= 0
            ? flow.formalCycleLimit : settings.value("Test/CycleLimit", 0).toInt());
    }
    ui->bootFlowPathEdit->setText(QFileInfo(m_bootFlowFile).fileName());
    ui->bootFlowPathEdit->setToolTip(QDir::toNativeSeparators(m_bootFlowFile));
    ui->bootFlowPathEdit->setCursorPosition(0);
    ui->targetPathEdit->setText(loaded ? flow.executable : QString());
    ui->targetPathEdit->setToolTip(loaded ? flow.executable : QString());
    ui->targetPathEdit->setCursorPosition(0);
    // The monitor describes the completed run until another run starts or a flow is imported.
    if (preserveResults && m_flowMonitor->hasResults()) {
        ui->modeLabel->setText("保留上次測試結果；編輯內容將於下次測試套用");
        return;
    }
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
    if (!loadFlow(file, flow, error)) {
        QMessageBox::warning(this, "流程無法匯入", error);return false;
    }
    QString selectedFile=QFileInfo(file).absoluteFilePath();bool targetChanged=false;
    const QString dutDirectory=QDir(m_dir).absoluteFilePath("../dut");
    if(flow.executable.isEmpty() || !QFileInfo(flow.executable).isFile()){
        const auto candidates=QDir(dutDirectory).entryInfoList({"*.exe"},QDir::Files|QDir::Readable,QDir::Name);
        if(candidates.size()==1){flow.executable=candidates.first().absoluteFilePath();flow.workingDirectory.clear();targetChanged=true;}
    }
    QString readinessError;const bool executableReady=validateFlow(flow,readinessError);
    if(targetChanged && !saveFlow(selectedFile,flow,error)){
        selectedFile=QFileDialog::getSaveFileName(this,"原腳本無法寫入，請另存本機腳本",QFileInfo(file).absolutePath()+"/local_test.json","測試腳本 (*.json)");
        if(selectedFile.isEmpty())return false;
        if(!selectedFile.endsWith(".json",Qt::CaseInsensitive))selectedFile+=".json";
        if(!saveFlow(selectedFile,flow,error)){QMessageBox::warning(this,"無法保存腳本",error);return false;}
    }
    const QString previous = m_bootFlowFile;
    m_bootFlowFile = selectedFile;
    if (!saveSettings()) {
        m_bootFlowFile = previous;
        return false;
    }
    refreshBootFlowDisplay();
    ui->countLabel->setText(QString("已載入 %1 個測試項目%2").arg(m_flowMonitor->testItemCount())
        .arg(flow.version.isEmpty() ? QString() : " · 腳本版本 " + flow.version));
    setRunning(false);
    setStatus(executableReady?"腳本已載入 — 請先試跑（不關機）":"腳本已載入，可查看與編輯；執行前請補齊設定："+readinessError);
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
    if (error.isEmpty()) return true;
    setStatus("FAIL — 環境檢查未通過", "fail");
    ui->logEdit->appendPlainText(error);
    return false;
}

void MainWindow::refreshModeNote()
{
    const auto relay = loadRelaySettings(m_dir);
    if (!warmMode()) {
        ui->modeNote->setText(QString("網路冷開機：控制端 確認排程後正常關機。控制端 等待 %1 秒後斷電 %2 秒。")
            .arg(relay.delaySeconds).arg(relay.ticks / 10.0));
        m_flowMonitor->setTailDescription(ui->modeNote->text());
        return;
    }
    const QString outcome = ui->continueOnFailureCheck->isChecked() ? "檢查完成（含 FAIL）" : "PASS";
    ui->modeNote->setText(QString("暖重開機：正式測試 PASS 後重新啟動 Windows，不呼叫外部控制器。").replace("PASS", outcome));
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
    ui->resetStatisticsButton->setEnabled(!running);
    const QSettings reportSettings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    ui->exportReportButton->setEnabled(!running && !reportSettings.value("Report/Sessions").toStringList().isEmpty());
    ui->clearButton->setEnabled(ui->exportReportButton->isEnabled());
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
    const QString agent = relayAgentUrl();
    if (!dry && !warmMode()) {
        QString error;
        auto relay = loadRelaySettings(m_dir);
        relay.agentUrl = agent;
        if (agent.isEmpty())
            error = discoveredAgents().size() > 1 ? "找到多台 Agent，請在連線設定選擇一台。"
                : "尚未找到 Agent，請確認兩台 PC 在同一網段及 UDP 45454 已放行。";
        if (!error.isEmpty() || !relayAgentSettingsValid(relay, error)) {
            const QString message = "無法開始正式冷開機：" + error;
            setStatus(message, "fail");
            const QString detail = agent.isEmpty() ? message : message + " [Agent=" + agent + "]";
            ui->logEdit->appendPlainText(detail);
            if (!recordRelayPreflightFailure(m_dir, detail))
                ui->logEdit->appendPlainText("無法寫入 boot_cycle_detail.log");
            return;
        }
    }
    refreshBootFlowDisplay();
    if (!automatic && !saveSettings()) return;
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    quint64 lastFormalCycle = settings.value("Run/LastFormalCycle", 0).toULongLong();
    if (!dry && (!automatic || !settings.contains("Run/LastFormalCycle"))) {
        loadHistory();
        lastFormalCycle = ui->cycleLimitSpin->value() == 0 ? 0
            : m_formalCount + static_cast<quint64>(ui->cycleLimitSpin->value());
        settings.setValue("Run/LastFormalCycle", lastFormalCycle);
        settings.setValue("Run/FirstFormalCount", m_formalCount);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            setStatus("FAIL — 無法保存正式測試輪數", "fail");
            return;
        }
    }
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
    const bool warm = warmMode();
    QString reportSession;
    if (!dry && automatic && settings.value("Report/FormalWarm").toBool() == warm)
        reportSession = settings.value("Report/FormalSession").toString();
    if (reportSession.isEmpty()) reportSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!dry) {
        settings.setValue("Report/FormalSession", reportSession);
        settings.setValue("Report/FormalWarm", warm);
    }
    settings.setValue("Report/LatestSession", reportSession);
    auto sessions = settings.value("Report/Sessions").toStringList();
    const auto existingSession = std::find_if(sessions.cbegin(), sessions.cend(),
        [&reportSession](const QString &entry) { return entry.startsWith(reportSession + '|'); });
    const auto entry = existingSession != sessions.cend()
        ? *existingSession : QString("%1|%2|%3|%4").arg(reportSession, dry ? "trial" : "formal",
            warm ? "warm" : "cold", QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    sessions.removeAll(entry);
    sessions.prepend(entry);
    settings.setValue("Report/Sessions", sessions);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        setStatus("無法儲存本次報告設定", "fail");
        return;
    }
    QFile log(m_dir + "/boot_cycle_detail.log");
    m_logOffset = log.exists() ? log.size() : 0;
    m_pending.clear();
    m_dry = dry; m_pass = m_fail = m_stopping = false;
    m_relayFailureReason.clear();
    m_relayReconnecting = false;
    m_shutdownRequested = false;
    m_stoppedByAgent = false;
    m_flowMonitor->setTailDescription(dry ? "本次為試跑：保存報告，不送電源命令、不關機。" : ui->modeNote->text());
    m_flowMonitor->begin(dry ? "試跑準備中；依腳本執行，不執行電源動作" : "啟動等待 20 秒，之後執行 JSON 流程");
    ui->modeLabel->setText(QString("%1／%2").arg(dry ? "試跑（不關機）" : "正式測試",
        warm ? "暖重開機" : "冷開機"));
    ui->countLabel->setText("JSON 流程：等待檢查");
    setStatus(dry ? "檢查中 — 準備執行檢查" : "檢查中 — 等待 20 秒再執行檢查");
    QStringList args{"--worker", "--run-directory", m_dir, "--target-exe", target.absoluteFilePath(),
        "--report-session", reportSession};
    if (ui->continueOnFailureCheck->isChecked()) args << "--continue-on-failure";
    if (dry) args << "--dry-run";
    if (warmMode()) args << "--warm-reboot";
    if (!agent.isEmpty()) args << "--agent-url" << agent;
    args << "--flow-file" << m_bootFlowFile;
    if (!dry && lastFormalCycle != 0)
        args << "--last-formal-cycle" << QString::number(lastFormalCycle);
    m_runningAgent = agent;
    setRunning(true);m_logTimer.start();
    auto *worker = new QProcess(this);
    m_testProcess = worker;
    worker->setWorkingDirectory(m_dir);
    worker->setStandardOutputFile(QProcess::nullDevice());
    const auto finish = [this, worker, reportSession](int code, QProcess::ExitStatus status, QString reason) {
        if (m_testProcess != worker) return;
        m_testProcess = nullptr;
        if (!reason.isEmpty()) {
            m_fail = true;
            const QString diagnostic = QString::fromLocal8Bit(worker->readAllStandardError()).trimmed().left(2048);
            const QString detail = diagnostic.isEmpty() ? reason : reason + " | " + diagnostic.simplified();
            if (!recordBootWorkerFailure(m_dir, detail, reportSession)) ui->logEdit->appendPlainText(detail);
        }
        finishTest(code, status);
        if (!reason.isEmpty()) setStatus(reason, "fail");
        worker->deleteLater();
    };
    connect(worker, &QProcess::errorOccurred, this, [worker, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(1, QProcess::CrashExit, "無法啟動測試程序：" + worker->errorString());
    });
    connect(worker, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        const QString reason = status == QProcess::CrashExit
            ? QString("測試程序意外結束（0x%1），請查看詳細紀錄。").arg(quint32(code), 8, 16, QChar('0'))
            : QString();
        finish(code, status, reason);
    });
    worker->start(QCoreApplication::applicationFilePath(), args);
}

void MainWindow::requestStop(bool fromAgent)
{
    QFile marker(m_dir + "/STOP");
    if (!marker.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || marker.write("Stop requested from Qt UI\r\n") < 0 || !marker.flush()) {
        QMessageBox::critical(this, "停止失敗", "無法建立 STOP：" + marker.errorString());
        return;
    }
    marker.close();
    m_stopping = true;
    m_stoppedByAgent = m_stoppedByAgent || fromAgent;
    m_flowMonitor->requestStop();
    setStatus(fromAgent ? (m_running ? "控制端 已要求停止，正在停止…" : "已由控制端 停止")
        : "正在停止…", "stopped");
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

std::array<quint64, 6> MainWindow::statisticsTotals() const
{
    return {m_formalCount, m_dryCount, m_results[1], m_results[2], m_results[3], m_results[0]};
}

void MainWindow::refreshStatistics()
{
    auto counts = statisticsTotals();
    for (size_t i = 0; i < counts.size(); ++i)
        counts[i] -= qMin(counts[i], m_statisticsBaseline[i]);
    ui->statisticsLabel->setText(QString("累計 %1 次（正式 %2／試跑 %3）｜PASS %4｜FAIL %5｜停止 %6｜未完成 %7")
        .arg(counts[0] + counts[1]).arg(counts[0]).arg(counts[1])
        .arg(counts[2]).arg(counts[3]).arg(counts[4]).arg(counts[5]));
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
    const QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    const auto baseline = settings.value("Statistics/Baseline").toStringList();
    m_statisticsBaseline = {};
    for (size_t i = 0; i < m_statisticsBaseline.size(); ++i)
        m_statisticsBaseline[i] = baseline.value(qsizetype(i)).toULongLong();
    m_pending.clear(); refreshStatistics();
}

bool MainWindow::clearRecordFiles(const QString &session, QString &error)
{
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    if (!clearReportWork(m_dir, session, error)) return false;
    auto sessions = settings.value("Report/Sessions").toStringList();
    sessions.removeIf([&session](const QString &entry) { return entry.startsWith(session + '|'); });
    settings.setValue("Report/Sessions", sessions);
    if (settings.value("Report/FormalSession").toString() == session) settings.remove("Report/FormalSession");
    if (settings.value("Report/LatestSession").toString() == session) settings.remove("Report/LatestSession");
    settings.sync();
    if (settings.status() == QSettings::NoError) return true;
    error = "無法更新報告索引";
    return false;
}

void MainWindow::resetStatistics()
{
    if (m_running) return;
    if (QMessageBox::question(this, "重設累計次數", "要將底部統計歸零嗎？歷史紀錄與報告會保留。",
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes || m_running) return;
    const auto totals = statisticsTotals();
    QStringList baseline;
    for (const auto count : totals) baseline.append(QString::number(count));
    QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    settings.setValue("Statistics/Baseline", baseline);
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        QMessageBox::warning(this, "無法重設累計次數", "無法儲存統計設定。");
        return;
    }
    m_statisticsBaseline = totals;
    refreshStatistics();
}

void MainWindow::manageReports()
{
    if (m_running) return;
    const QSettings settings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    QDialog dialog(this);
    dialog.setWindowTitle("測試報告");
    dialog.resize(760, 550);
    auto *layout = new QVBoxLayout(&dialog);
    auto *list = new QListWidget(&dialog);
    layout->addWidget(list);
    for (const auto &entry : settings.value("Report/Sessions").toStringList()) {
        const auto fields = entry.split('|');
        if (fields.size() != 4) continue;
        auto *item = new QListWidgetItem(QString("%1  %2／%3  [%4]").arg(fields[3],
            fields[1] == "trial" ? "試跑" : "正式測試", fields[2] == "warm" ? "暖重開機" : "冷開機", fields[0].left(8)));
        item->setData(Qt::UserRole, fields);
        list->addItem(item);
    }
    if (!list->count()) return;
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    auto *preview = buttons->addButton("開啟報告", QDialogButtonBox::ActionRole);
    auto *exportButton = buttons->addButton("匯出完整報告", QDialogButtonBox::ActionRole);
    auto *remove = buttons->addButton("清除所選暫存…", QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(list, &QListWidget::currentItemChanged, &dialog, [=](QListWidgetItem *item) {
        preview->setEnabled(item != nullptr);
        exportButton->setEnabled(item != nullptr);
        remove->setEnabled(item != nullptr);
    });
    connect(list, &QListWidget::itemDoubleClicked, preview, &QPushButton::click);
    connect(preview, &QPushButton::clicked, &dialog, [&] {
        if (m_running || !list->currentItem()) return;
        const auto session = list->currentItem()->data(Qt::UserRole).toStringList();
        QTemporaryDir temporary;
        const auto path = temporary.filePath("report.html");
        QString error;
        const auto work = temporary.isValid()
            ? previewBootSession(m_dir, session[0], session[1] == "trial", session[2] == "warm", path, error)
            : QString();
        QFile report(path);
        if (work.isEmpty() || !report.open(QIODevice::ReadOnly)) {
            QMessageBox::warning(&dialog, "無法開啟報告", error.isEmpty() ? "無法讀取報告預覽。" : error);
            return;
        }
        QDialog viewer(&dialog);
        viewer.setWindowTitle(list->currentItem()->text());
        viewer.resize(950, 680);
        auto *viewerLayout = new QVBoxLayout(&viewer);
        auto *browser = new QTextBrowser(&viewer);
        browser->setOpenLinks(false);
        browser->setHtml(QString::fromUtf8(report.readAll()));
        const auto base = QUrl::fromLocalFile(work + '/');
        connect(browser, &QTextBrowser::anchorClicked, &viewer, [base](const QUrl &link) {
            if (link.isRelative()) QDesktopServices::openUrl(base.resolved(link));
        });
        viewerLayout->addWidget(browser);
        auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &viewer);
        connect(close, &QDialogButtonBox::rejected, &viewer, &QDialog::reject);
        viewerLayout->addWidget(close);
        viewer.exec();
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [&] {
        if (!m_running && list->currentItem())
            exportReport(list->currentItem()->data(Qt::UserRole).toStringList());
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        const auto *item = list->currentItem();
        if (m_running || !item) return;
        const auto session = item->data(Qt::UserRole).toStringList();
        if (QMessageBox::question(&dialog, "清除報告暫存",
                "要清除這批測試的報告暫存嗎？\n" + item->text() + "\n已匯出的報告會保留。",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes || m_running) return;
        QString error;
        if (!clearRecordFiles(session[0], error)) {
            QMessageBox::warning(&dialog, "無法清除報告暫存", error);
            return;
        }
        delete list->takeItem(list->row(item));
        setRunning(false);
        if (!list->count()) dialog.accept();
    });
    list->setCurrentRow(0);
    dialog.exec();
}

void MainWindow::exportReport(const QStringList &session)
{
    QString error;
    const QString folder = exportBootSession(m_dir, session[0], session[1] == "trial", session[2] == "warm", error);
    if (folder.isEmpty()) {
        QMessageBox::warning(this, "無法匯出報告", error);
        return;
    }
    const auto displayPath = QDir::toNativeSeparators(folder);
    ui->logEdit->appendPlainText("完整報告已匯出：" + displayPath);
    QMessageBox message(QMessageBox::Information, "完整報告已匯出", displayPath, QMessageBox::NoButton, this);
    message.setTextFormat(Qt::PlainText);
    message.setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *open = message.addButton("開啟報告資料夾", QMessageBox::ActionRole);
    message.addButton(QMessageBox::Close);
    message.exec();
    if (message.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
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
    const qsizetype reconnect = line.indexOf(" | RELAY RECONNECT: ");
    const qsizetype preflightFailure = line.indexOf(" | RELAY PREFLIGHT FAILED: ");
    if (reconnect >= 0) {
        m_relayReconnecting = true;
        if (!m_stopping) setStatus("等待控制端重連 — " + line.mid(reconnect + QString(" | RELAY RECONNECT: ").size()));
    } else if (line.contains(" | RELAY RECONNECTED: ")) {
        m_relayReconnecting = false;
        if (!m_stopping) setStatus("已重新連線 — 繼續測試");
    } else if (preflightFailure >= 0) {
        m_relayFailureReason = "無法開始正式冷開機：" + line.mid(preflightFailure + QString(" | RELAY PREFLIGHT FAILED: ").size())
            .section(" [Agent=", 0, 0);
        m_fail = true;
        setStatus(m_relayFailureReason, "fail");
    } else if (line.contains("FAIL:")) {
        m_fail = true;
        const QString message = line.mid(line.indexOf("FAIL:") + 5);
        const qsizetype relayFailure = message.indexOf("控制端 排程未確認：");
        if (relayFailure >= 0) m_relayFailureReason = message.mid(relayFailure).section(" [Agent=", 0, 0);
        setStatus(m_relayFailureReason.isEmpty() ? "FAIL — " + message : m_relayFailureReason, "fail");
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
        m_stopping = true;
        m_stoppedByAgent = m_stoppedByAgent || line.contains("STOP detected by Relay Agent");
        setStatus(m_stoppedByAgent ? "已由控制端 停止" : "已停止", "stopped");
    } else if (line.contains("RELAY COUNTDOWN:")) {
        m_flowMonitor->phase("斷電前等待中；現在仍可停止");
        setStatus((m_fail ? QString("FAIL 已記錄 — ") : QString()) + "斷電前倒數中；可按停止", m_fail ? "fail" : "idle");
    } else if (line.contains("RELAY AGENT ACCEPTED:")) {
        m_flowMonitor->phase("控制端 已接受排程，準備正常關機");
        setStatus("控制端 已接受排程", m_fail ? "fail" : "idle");
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
        m_shutdownRequested = true;
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
    m_relayReconnecting = false;
    if (exitCode != 0 || status != QProcess::NormalExit) m_shutdownRequested = false;
    if (exitCode == 0 && status == QProcess::NormalExit && !m_dry) {
        QFile marker(m_dir + "/STOP");
        if (marker.open(QIODevice::ReadOnly) && marker.readLine().trimmed() == "Cycle limit reached") {
            setStatus("已達設定輪數 — 測試報告已產出", m_fail ? "fail" : "stopped");
            m_flowMonitor->phase("已達設定輪數；保存報告，不執行電源動作");
            m_flowMonitor->finish(true, false, "已達設定輪數；測試報告已產出");
            return;
        }
    }
    if (m_stopping && exitCode == 0 && status == QProcess::NormalExit) {
        m_shutdownRequested = false;
        QString message = m_stoppedByAgent ? "已由控制端 停止" : "已停止";
        if (m_fail) message += " — 已保存本次 FAIL 紀錄";
        else if (m_pass) message += " — 已保存本次 PASS 紀錄";
        setStatus(message, "stopped");
    }
    else if (m_fail && m_dry) setStatus("FAIL — 試跑完成，已記錄，不關機", "fail");
    else if (m_fail && exitCode == 0 && status == QProcess::NormalExit)
        setStatus("FAIL — 已記錄，已要求電源動作以繼續下一輪", "fail");
    else if (m_fail || exitCode != 0 || status == QProcess::CrashExit)
        setStatus(m_relayFailureReason.isEmpty() ? "FAIL — 檢查或控制動作失敗" : m_relayFailureReason, "fail");
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

bool MainWindow::homeRunSelfTest()
{
    QTemporaryDir sandbox;if(!sandbox.isValid())return false;
    const QString oldDirectory=m_dir,oldFlow=m_bootFlowFile;
    m_dir=sandbox.path();m_bootFlowFile.clear();loadHistory();
    TestFlow flow;flow.executable=QCoreApplication::applicationFilePath();flow.name="首頁執行路徑驗證";
    flow.arguments = "--worker --describe-action --dry-run";
    flow.reuseExisting = false;
    FlowStep launch; launch.type = "launch"; launch.name = "啟動離線驗證程序";
    FlowStep step;step.type="delay";step.name="純等待，不操作硬體";step.timeoutMs=20;flow.steps={launch, step};
    QString error;const QString script=m_dir+"/flow.json";
    auto execute=[&](bool cancel, bool crash = false){
        if(!saveFlow(script,flow,error) || !importBootFlow(script))return false;
        startTest(true);if(!m_testProcess)return false;
        QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
        connect(m_testProcess,&QProcess::finished,&loop,&QEventLoop::quit);
        connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);deadline.start(12000);
        if(cancel)QTimer::singleShot(1200,&loop,[this]{requestStop(true);});
        if (crash) QTimer::singleShot(500, &loop, [this] { if (m_testProcess) m_testProcess->kill(); });
        loop.exec();
        if (m_testProcess) {
            requestStop();
            auto *worker = m_testProcess;
            if (!worker->waitForFinished(3000)) { worker->kill(); worker->waitForFinished(3000); }
            QApplication::processEvents();
            return false;
        }
        return !m_running;
    };
    const bool passed=execute(false) && m_pass && !m_fail && m_dryCount==1 && m_formalCount==0;
    flow.steps.last().timeoutMs=5000;
    const bool stopped=passed && execute(true) && m_stopping && m_dryCount==2 && m_formalCount==0
        && ui->statusLabel->text().startsWith("已由控制端 停止");
    const QSettings records(m_dir + "/test_settings.ini", QSettings::IniFormat);
    const auto sessions = records.value("Report/Sessions").toStringList();
    const QString saved = stopped && sessions.size() == 2
        ? exportBootSession(m_dir, sessions.last().section('|', 0, 0), true, false, error) : QString();
    QFile exportedLog(saved + "/boot_cycle_detail.log");
    const bool separated = !saved.isEmpty() && exportedLog.open(QIODevice::ReadOnly)
        && exportedLog.readAll().count("Dry-run started") == 1
        && !QDir(saved + "/evidence").entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    const bool isolated = stopped && execute(false, true) && m_fail
        && ui->statusLabel->text().startsWith("測試程序意外結束")
        && ui->logEdit->toPlainText().contains("WORKER FAILURE:");
    m_dir=oldDirectory;m_bootFlowFile=oldFlow;refreshBootFlowDisplay();loadHistory();
    return passed && stopped && separated && isolated;
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
        && !checkEnvironment() && m_testProcess==nullptr;
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
    const QString reportSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString otherSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString work = m_dir + "/report_work/" + reportSession;
    const QString otherWork = m_dir + "/report_work/" + otherSession;
    QSettings reportSettings(m_dir + "/test_settings.ini", QSettings::IniFormat);
    reportSettings.setValue("Report/LatestSession", reportSession);
    const QString otherEntry = otherSession + "|formal|cold|other";
    reportSettings.setValue("Report/Sessions", QStringList{otherEntry, reportSession + "|trial|warm|fixture"});
    reportSettings.sync();
    const bool staged = QDir().mkpath(work + "/evidence/round_1") && QDir().mkpath(otherWork)
        && QFile::copy(m_dir + "/boot_cycle_detail.log", work + "/boot_cycle_detail.log")
        && QFile::copy(m_dir + "/boot_cycle_results.csv", work + "/boot_cycle_results.csv")
        && QFile::copy(m_dir + "/boot_cycle_results.csv", work + "/evidence/round_1/result.txt");
    const QString previewPath = records.filePath("preview.html");
    const bool previewed = staged && !previewBootSession(m_dir, reportSession, true, true, previewPath, clearError).isEmpty()
        && QFile::exists(previewPath) && !QDir(m_dir + "/reports").exists();
    const QString exported = staged ? exportBootSession(m_dir, reportSession, true, true, clearError) : QString();
    const bool cleared = previewed && !exported.isEmpty() && exported.contains("/reports/trial/warm/")
        && clearRecordFiles(reportSession, clearError) && !QDir(work).exists() && QDir(otherWork).exists()
        && QFile::exists(exported + "/boot_cycle_report.html") && QFile::exists(exported + "/evidence/round_1/result.txt")
        && QFile::exists(m_dir + "/boot_cycle_detail.log")
        && QFile::exists(m_dir + "/boot_cycle_results.csv") && QFile::exists(m_dir + "/test_settings.ini")
        && reportSettings.value("Report/Sessions").toStringList() == QStringList{otherEntry}
        && QFile::exists(m_dir + "/STOP") && QFile::exists(m_dir + "/FAILED");
    loadHistory();
    QTimer::singleShot(0, this, [] {
        if (auto *confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            confirmation->button(QMessageBox::Yes)->click();
    });
    resetStatistics();
    loadHistory();
    const bool reset = m_formalCount == 3 && m_dryCount == 1
        && m_results == std::array<quint64, 4>{1, 1, 1, 1}
        && ui->statisticsLabel->text().startsWith("累計 0 次");
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
    TestFlow withoutTarget=imported;withoutTarget.executable=m_dir+"/missing_dut.exe";
    const QString missingTargetScript=m_dir+"/without_target.json";
    const bool importWithoutTarget=saveFlow(missingTargetScript,withoutTarget,importedError)
        && importBootFlow(missingTargetScript) && m_flowMonitor->testItemCount()==1
        && ui->targetPathEdit->text() == withoutTarget.executable
        && !ui->dryButton->isEnabled() && !ui->runButton->isEnabled();
    const bool importedOk = saveFlow(source, imported, importedError) && importBootFlow(source);
    const bool importedValid = importedOk && validateBootFlow(importedError)
        && loadFlow(m_bootFlowFile, saved, importedError) && saved.steps.size() == 1 && saved.steps[0].name == delay.name
        && ui->pageTitle->text() == imported.name && m_flowMonitor->testItemCount() == 1
        && ui->targetPathEdit->text() == imported.executable
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
    m_bootFlowFile = missingTargetScript;
    refreshBootFlowDisplay(true);
    const bool resultRetained=m_flowMonitor->hasResults() && m_flowMonitor->rowState(2)=="fail"
        && ui->targetPathEdit->text() == withoutTarget.executable;
    m_bootFlowFile = previousFlowFile;
    refreshBootFlowDisplay();
    const bool nextPlanCleared=!m_flowMonitor->hasResults();
    return operatorDefault && engineeringLocked && monitor && resultRetained && nextPlanCleared && pass && fail && locked && idle && warm && cold && unconfigured && history && clearLocked && cleared && reset
        && importWithoutTarget && importedValid && persisted && noPartialPass && flowPass && partialNotCounted && importedCounted
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
