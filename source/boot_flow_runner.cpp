#include "boot_flow_runner.h"
#include "flow_model.h"
#include "flow_runner.h"
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QUuid>

int executeBootFlow(const std::wstring &file, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error)
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
    const QString evidence = root + "/boot_flow_results/" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")
        + '_' + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!QDir().mkpath(evidence) || !saveFlow(evidence + "/flow.json", flow, problem)) {
        error = L"Cannot save imported-flow evidence: " + problem.toStdWString(); return 1;
    }
    log(L"Imported flow evidence: " + evidence.toStdWString());
    FlowRunner runner; runner.flow = flow; runner.evidenceDirectory = evidence;
    QEventLoop loop; QTimer stopTimer; stopTimer.setInterval(100);
    bool success = false, cancelled = false;
    QObject::connect(&runner, &FlowRunner::progress, &loop, [&](QString line) {
        line.replace('\r', "\\r"); line.replace('\n', "\\n");
        log(L"FLOW | " + line.toStdWString());
    });
    QObject::connect(&runner, &FlowRunner::stepFinished, &loop, [&](int index, const QString &result, const QString &value) {
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
