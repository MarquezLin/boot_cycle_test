#include "boot_flow_runner.h"
#include "flow_model.h"
#include "flow_runner.h"
#include "flow_monitor.h"
#include "offline_capture.h"
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
#include <QSettings>

int executeBootFlow(const TestFlow &flow, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error,
    const std::wstring &evidenceDirectory)
{
    const QString root = QString::fromStdWString(directory);
    auto stopped = [&] {return QFile::exists(root + "/STOP") || QFile::exists(root + "/FAILED");};
    if (stopped()) return 2;
    QString problem;
    if (!validateFlow(flow, problem)) {
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
    bool success = false, cancelled = false, scriptStopped = false;
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
    QObject::connect(&runner, &FlowRunner::scriptStopReached, &loop, [&] {scriptStopped = true;});
    QObject::connect(&runner, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&stopTimer, &QTimer::timeout, &loop, [&] {if (stopped()) runner.stop();});
    stopTimer.start(); runner.start(); loop.exec(); runner.wait(); stopTimer.stop();
    const int result = stopped() ? 2 : scriptStopped ? BootFlowScriptStopped : cancelled ? 2 : success ? 0 : 1;
    QFile outcome(evidence + "/result.txt");
    if (outcome.open(QIODevice::WriteOnly)) outcome.write(result == 0 ? "PASS" : result == 1 ? "FAIL" : "STOP");
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
    QVector<int> startedSteps;
    auto receive = [&](const std::wstring &message) {
        const QString line = QString::fromStdWString(message);
        if (!line.startsWith("FLOW_EVENT ")) return;
        const auto object = QJsonDocument::fromJson(line.mid(11).toUtf8()).object();
        if (object["event"] == "plan") ++plans;
        if (object["event"] == "start") {
            ++starts;
            startedSteps.append(object["index"].toInt());
        }
        if (object["event"] == "finish") ++finishes;
        monitor.applyEvent(object);
    };
    std::wstring problem;
    const auto runSavedFlow = [&] {
        TestFlow saved;
        if (!loadFlow(path, saved, error)) return 1;
        return executeBootFlow(saved, dir.path().toStdWString(), flow.executable.toStdWString(), receive, problem);
    };
    const int result = runSavedFlow();
    monitor.phase("保存報告"); monitor.finish(result == 0, false, "完成");
    if (result != 0 || plans != 1 || starts != 2 || finishes != 2
        || monitor.rowState(0) != "pass" || monitor.rowState(1) != "pass"
        || monitor.rowState(2) != "pass" || monitor.rowState(3) != "pass" || monitor.testItemCount()!=1) return false;
    FlowStep jump;
    jump.type = "loop";
    jump.testItemId = "g";
    jump.targetStep = 2;
    jump.jumpCount = 2;
    flow.steps = {step, step, jump, step};
    for (auto &s : flow.steps) s.testItemId = "g";
    startedSteps.clear();
    if (!saveFlow(path, flow, error) || runSavedFlow() != 0
        || startedSteps != QVector<int>{0, 1, 2, 1, 2, 1, 2, 3}) return false;

    FlowStep image;
    image.type = "checkImage";
    image.condition = "change";
    image.expected = "8";
    image.region = QRect(0, 0, 32, 18);
    image.captureDurationMs = 120;
    image.sampleFps = 60;
    const QString clip = dir.path() + "/fixture";
    std::atomic_bool stop = false;
    QString detail;
    bool bright = false;
    if (!offlineImageStep(image, clip, {}, [&](QString &) {
        QImage frame(32, 18, QImage::Format_RGB32);
        bright = !bright;
        frame.fill(bright ? Qt::white : Qt::black);
        return frame;
    }, [](const QString &) {}, stop, detail, error)) return false;
    FlowRunner runner;
    runner.flow = flow;
    runner.flow.steps[1] = image;
    runner.replayCaptureFile = clip + ".gvfgcap";
    runner.evidenceDirectory = dir.path();
    QEventLoop replay;
    bool expectedFailure = false, reachedNextStep = false;
    QObject::connect(&runner, &FlowRunner::stepFinished, &replay,
        [&](int index, const QString &status, const QString &detail) {
            if (index == 2 && detail.contains("2/2")) QFile::remove(runner.replayCaptureFile);
            if (index == 1 && status == "FAIL") expectedFailure = detail.contains("第 3 步迴圈的第 2/2 次跳轉後");
            if (index == 3) reachedNextStep = true;
        }, Qt::BlockingQueuedConnection);
    QObject::connect(&runner, &QThread::finished, &replay, &QEventLoop::quit);
    runner.start();
    replay.exec();
    runner.wait();
    if (!expectedFailure || reachedNextStep) return false;

    FlowStep stopStep;
    stopStep.type = "stop";
    stopStep.testItemId = "g";
    const auto repeatSteps = flow.steps;
    flow.steps.insert(3, stopStep);
    startedSteps.clear();
    if (!saveFlow(path, flow, error) || runSavedFlow() != BootFlowScriptStopped
        || startedSteps != QVector<int>{0, 1, 2, 1, 2, 1, 2, 3}
        || monitor.rowState(4) != "stopped" || monitor.rowState(5) != "skipped") return false;

    // Use an invalid relay address so this cold-mode test cannot schedule power.
    QTemporaryDir formal;
    if (!formal.isValid()) return false;
    QSettings settings(formal.filePath("test_settings.ini"), QSettings::IniFormat);
    settings.setValue("Test/StartupDelaySeconds", 0);
    settings.sync();
    std::wstring args[] = {L"self-test", L"--target-exe", flow.executable.toStdWString(),
        L"--flow-file", path.toStdWString(), L"--agent-url", L"invalid", L"--continue-on-failure"};
    wchar_t *argv[] = {args[0].data(), args[1].data(), args[2].data(), args[3].data(),
        args[4].data(), args[5].data(), args[6].data(), args[7].data()};
    if (runBootTestInDirectory(8, argv, formal.path()) != 0) return false;
    QFile marker(formal.filePath("STOP"));
    if (!marker.open(QIODevice::ReadOnly) || marker.readLine().trimmed() != "Script stop reached") return false;
    marker.close();
    QFile log(formal.filePath("boot_cycle_detail.log"));
    if (!log.open(QIODevice::ReadOnly) || !log.readAll().contains("STOP detected at scripted stop step; no power action"))
        return false;

    flow.steps = repeatSteps;
    flow.steps[0].timeoutMs = 1500;
    if (!saveFlow(path, flow, error)) return false;
    QTimer cancel; cancel.setSingleShot(true);
    QObject::connect(&cancel, &QTimer::timeout, &cancel, [&] {
        QFile stop(dir.path() + "/STOP"); if (stop.open(QIODevice::WriteOnly)) stop.write("self-test");
    });
    cancel.start(200);
    const int stopped = runSavedFlow();
    monitor.finish(false, true, "手動停止");
    return stopped == 2 && monitor.rowState(1) == "stopped" && monitor.rowState(2) == "skipped";
}
