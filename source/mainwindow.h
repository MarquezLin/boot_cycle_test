#pragma once
#include "startup_manager.h"
#include <QMainWindow>
#include <QProcess>
#include <QTimer>
#include <memory>
#include <array>

namespace Ui { class MainWindow; }
class QCloseEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
    void startTest(bool dry, bool automatic = false);
    void showExisting();
    bool smokeTest();
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void refreshModeNote();
    bool saveSettings();
    void browseTarget();
    bool targetAvailable() const;
    bool checkEnvironment();
    void refreshStartup();
    void setRunning(bool running);
    void requestStop();
    void readLog();
    void loadHistory();
    void countHistoryLine(const QString &line);
    void refreshStatistics();
    void clearHistory();
    bool clearRecordFiles(QString &error);
    void showLine(const QString &line);
    void setStatus(const QString &text, const QString &state = "idle");
    void finishTest(int exitCode, QProcess::ExitStatus status);
    bool warmMode() const;
    std::unique_ptr<Ui::MainWindow> ui;
    StartupManager m_startup;
    QProcess m_worker;
    QTimer m_logTimer, m_startupTimer;
    QString m_dir;
    QByteArray m_pending;
    qint64 m_logOffset = 0;
    quint64 m_formalCount = 0, m_dryCount = 0;
    std::array<quint64, 4> m_results{}; // unfinished, PASS, FAIL, stopped
    int m_historyResult = -1;
    bool m_running = false, m_dry = true, m_pass = false, m_fail = false, m_stopping = false;
};
