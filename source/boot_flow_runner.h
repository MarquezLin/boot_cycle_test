#pragma once
#include <functional>
#include <string>
#include <QString>

struct TestFlow;
// 0 = passed, 1 = failed, 2 = cancelled, 3 = stopped by the script.
inline constexpr int BootFlowScriptStopped = 3;
int executeBootFlow(const TestFlow &flow, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error,
    const std::wstring &evidenceDirectory = {});
bool bootFlowProgressSelfTest();

int runBootTestInDirectory(int argc,wchar_t **argv,const QString &directory);
int bootStartupDelaySeconds(const QString &directory, bool dryRun);
bool recordRelayPreflightFailure(const QString &directory, const QString &detail);
bool recordBootWorkerFailure(const QString &directory, const QString &detail, const QString &session = {});
bool clearReportWork(const QString &directory, const QString &session, QString &error);
QString previewBootSession(const QString &directory, const QString &session, bool trial, bool warm,
                           const QString &output, QString &error);
QString exportBootSession(const QString &directory, const QString &session, bool trial, bool warm, QString &error);
