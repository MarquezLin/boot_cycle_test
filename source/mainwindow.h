#pragma once
#include "startup_manager.h"
#include "flow_model.h"
#include <QMainWindow>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <memory>
#include <array>

namespace Ui { class MainWindow; }
class QCloseEvent;
class FlowMonitor;
class QUdpSocket;
class QNetworkAccessManager;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void startTest(bool dry, bool automatic = false);
    void startRelaySession();
    void showExisting();
    bool smokeTest();
    bool homeRunSelfTest();
    bool monitorPreview(const QString &state);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refreshModeNote();
    bool saveSettings();
    bool targetAvailable() const;
    bool checkEnvironment();
    bool importBootFlow(const QString &file);
    void refreshBootFlowDisplay(bool preserveResults = false);
    bool validateBootFlow(QString &error) const;
    void refreshStartup();
    void setRunning(bool running);
    void requestStop(bool fromAgent = false);
    void pollRelayAgent();
    QStringList discoveredAgents() const;
    QString relayAgentUrl() const;
    void readLog();
    void loadHistory();
    void countHistoryLine(const QString &line);
    void refreshStatistics();
    void resetStatistics();
    void manageReports();
    void exportReport(const QStringList &session);
    bool clearRecordFiles(const QString &session, QString &error);
    std::array<quint64, 6> statisticsTotals() const;
    void showLine(const QString &line);
    void setStatus(const QString &text, const QString &state = "idle");
    void finishTest(int exitCode, QProcess::ExitStatus status);
    bool warmMode() const;
    std::unique_ptr<Ui::MainWindow> ui;
    FlowMonitor *m_flowMonitor = nullptr;
    StartupManager m_startup;
    QProcess *m_testProcess = nullptr;
    QTimer m_logTimer, m_startupTimer;
    QUdpSocket *m_discovery = nullptr;
    QNetworkAccessManager *m_relayNetwork = nullptr;
    QElapsedTimer m_relayClock;
    QHash<QString, qint64> m_discoveredAgents;
    QString m_lastRelayCommand, m_lastRelayResult, m_lastRelayCommandAgent, m_runningAgent;
    QString m_relayFailureReason;
    bool m_relayReconnecting = false;
    bool m_relayPolling = false;
    bool m_shutdownRequested = false;
    bool m_stoppedByAgent = false;
    QString m_dir;
    QString m_bootFlowFile;
    QByteArray m_pending;
    qint64 m_logOffset = 0;
    quint64 m_formalCount = 0, m_dryCount = 0;
    std::array<quint64, 4> m_results{}; // unfinished, PASS, FAIL, stopped
    std::array<quint64, 6> m_statisticsBaseline{};
    int m_historyResult = -1;
    bool m_running = false, m_dry = true, m_pass = false, m_fail = false, m_stopping = false;
};
