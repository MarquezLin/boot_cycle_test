#include "boot_flow_runner.h"
#include "flow_model.h"
#include "flow_runner.h"
#include "flow_monitor.h"
#include <QJsonDocument>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <QUuid>

int executeBootFlow(const std::wstring &file, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error,
    const std::wstring &evidenceDirectory)
{
    const QString root = QString::fromStdWString(directory);
    auto stopped = [&] {return QFile::exists(root + "/STOP") || QFile::exists(root + "/FAILED");};
    if (stopped()) return 2;
    TestFlow flow; QString problem;
    if (!loadFlow(QString::fromStdWString(file), flow, problem) || !validateFlow(flow, problem)) {
        error = problem.toStdWString(); return 1;
    }
    if (QFileInfo(flow.executable).canonicalFilePath().compare(QFileInfo(QString::fromStdWString(target)).canonicalFilePath(), Qt::CaseInsensitive) != 0) {
        error = L"Imported flow target differs from the configured target EXE"; return 1;
    }
    const QString evidence = evidenceDirectory.empty()
        ? root + "/boot_flow_results/" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")
            + '_' + QUuid::createUuid().toString(QUuid::Id128).left(8)
        : QString::fromStdWString(evidenceDirectory);
    if (!QDir().mkpath(evidence) || !saveFlow(evidence + "/flow.json", flow, problem)) {
        error = L"Cannot save imported-flow evidence: " + problem.toStdWString(); return 1;
    }
    log(L"Imported flow evidence: " + evidence.toStdWString());
    auto event = [&](const QJsonObject &object) {
        log(L"FLOW_EVENT " + QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)).toStdWString());
    };
    event(flowMonitorPlan(flow));
    FlowRunner runner; runner.flow = flow; runner.evidenceDirectory = evidence;
    QEventLoop loop; QTimer stopTimer; stopTimer.setInterval(100);
    bool success = false, cancelled = false;
    QObject::connect(&runner, &FlowRunner::stepStarted, &loop, [&](int index) {
        event({{"event", "start"}, {"index", index}});
    });
    QObject::connect(&runner, &FlowRunner::stepObserved, &loop, [&](int index, const QString &value) {
        event({{"event", "observed"}, {"index", index}, {"value", value}});
    });
    QObject::connect(&runner, &FlowRunner::progress, &loop, [&](QString line) {
        line.replace('\r', "\\r"); line.replace('\n', "\\n");
        log(L"FLOW | " + line.toStdWString());
    });
    QObject::connect(&runner, &FlowRunner::stepFinished, &loop, [&](int index, const QString &result, const QString &value) {
        event({{"event", "finish"}, {"index", index}, {"result", result}, {"value", value}});
        if (result == "FAIL") problem = QString("步驟 %1：%2").arg(index + 1).arg(value);
    });
    QObject::connect(&runner, &FlowRunner::completed, &loop, [&](bool ok, bool stop) {success = ok; cancelled = stop;});
    QObject::connect(&runner, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&stopTimer, &QTimer::timeout, &loop, [&] {if (stopped()) runner.stop();});
    stopTimer.start(); runner.start(); loop.exec(); runner.wait(); stopTimer.stop();
    const int result = cancelled || stopped() ? 2 : success ? 0 : 1;
    QFile outcome(evidence + "/result.txt");
    if (outcome.open(QIODevice::WriteOnly)) outcome.write(result == 0 ? "PASS" : result == 2 ? "STOP" : "FAIL");
    if (result == 1) error = (problem.isEmpty() ? QString("通用流程未通過") : problem).toStdWString();
    return result;
}

bool bootFlowProgressSelfTest()
{
    QTemporaryDir dir(QCoreApplication::applicationDirPath() + "/progress_check_XXXXXX");
    if (!dir.isValid()) return false;
    TestFlow flow; flow.executable = QCoreApplication::applicationFilePath();
    FlowStep step; step.type = "delay"; step.name = "測試含換行\n與特殊字元 \" 的事件"; step.timeoutMs = 50;
    flow.steps = {step, step};
    flow.name = "通用測試"; flow.version = "1";
    flow.testItems = {{"g", "確認兩個等待步驟", "不需操作", "等待中", "通知工程人員"}};
    for (auto &s : flow.steps) s.testItemId = "g";
    QString error; const QString path = dir.path() + "/flow.json";
    if (!saveFlow(path, flow, error)) return false;
    FlowMonitor monitor; int plans = 0, starts = 0, finishes = 0;
    auto receive = [&](const std::wstring &message) {
        const QString line = QString::fromStdWString(message);
        if (!line.startsWith("FLOW_EVENT ")) return;
        const auto object = QJsonDocument::fromJson(line.mid(11).toUtf8()).object();
        if (object["event"] == "plan") ++plans;
        if (object["event"] == "start") ++starts;
        if (object["event"] == "finish") ++finishes;
        monitor.applyEvent(object);
    };
    std::wstring problem;
    const int result = executeBootFlow(path.toStdWString(), dir.path().toStdWString(),
        flow.executable.toStdWString(), receive, problem);
    monitor.phase("保存報告"); monitor.finish(result == 0, false, "完成");
    if (result != 0 || plans != 1 || starts != 2 || finishes != 2
        || monitor.rowState(0) != "pass" || monitor.rowState(1) != "pass"
        || monitor.rowState(2) != "pass" || monitor.rowState(3) != "pass" || monitor.testItemCount()!=1) return false;
    flow.steps[0].timeoutMs = 1500;
    if (!saveFlow(path, flow, error)) return false;
    QTimer cancel; cancel.setSingleShot(true);
    QObject::connect(&cancel, &QTimer::timeout, &cancel, [&] {
        QFile stop(dir.path() + "/STOP"); if (stop.open(QIODevice::WriteOnly)) stop.write("self-test");
    });
    cancel.start(200);
    const int stopped = executeBootFlow(path.toStdWString(), dir.path().toStdWString(),
        flow.executable.toStdWString(), receive, problem);
    monitor.finish(false, true, "手動停止");
    return stopped == 2 && monitor.rowState(1) == "stopped" && monitor.rowState(2) == "skipped";
}
