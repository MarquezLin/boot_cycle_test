#pragma once
#include <functional>
#include <string>
#include <QString>

// 0 = passed, 1 = failed, 2 = stopped. Power actions stay in boot orchestration.
int executeBootFlow(const std::wstring &file, const std::wstring &directory, const std::wstring &target,
    const std::function<void(const std::wstring &)> &log, std::wstring &error,
    const std::wstring &evidenceDirectory = {});
bool bootFlowProgressSelfTest();

int runBootTestInDirectory(int argc,wchar_t **argv,const QString &directory);
bool recordRelayPreflightFailure(const QString &directory, const QString &detail);
bool recordBootWorkerFailure(const QString &directory, const QString &detail, const QString &session = {});
bool clearReportWork(const QString &directory, const QString &session, QString &error);
QString previewBootSession(const QString &directory, const QString &session, bool trial, bool warm,
                           const QString &output, QString &error);
QString exportBootSession(const QString &directory, const QString &session, bool trial, bool warm, QString &error);
