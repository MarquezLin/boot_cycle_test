#include <windows.h>
#include "boot_flow_runner.h"
#include "relay_controller.h"
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUuid>
#include <QUrl>
#include <QVector>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>


namespace fs = std::filesystem;

namespace {

std::wstring resultContext;
fs::path reportWork;
constexpr auto kResultHeader = "time,result,received_frames,detail\r\n";

constexpr DWORD kStartupDelaySeconds = 20;

std::wstring nowText()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::string narrow(const std::wstring &value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

QString reportWorkPath(const QString &root, const QString &session)
{
    const QUuid id(session);
    if (id.isNull() || id.toString(QUuid::WithoutBraces) != session) return {};
    const QString base = QDir(root).absoluteFilePath("report_work");
    const QString path = QDir(base).filePath(session);
    for (const auto &part : {base, path}) {
        const QFileInfo info(part);
        if (info.isSymLink() || info.isJunction() || (info.exists() && !info.isDir())) return {};
    }
    return path;
}

bool appendDetail(const fs::path &dir, const std::wstring &message)
{
    const auto line = narrow(nowText()) + " | " + narrow(message) + "\r\n";
    const auto write = [&](const fs::path &folder) {
        std::ofstream out(folder / "boot_cycle_detail.log", std::ios::app | std::ios::binary);
        out << line;
        out.flush();
        return out.good();
    };
    const bool historySaved = write(dir);
    const bool workSaved = reportWork.empty() || write(reportWork);
    return historySaved && workSaved;
}

bool appendResult(const fs::path &dir, const std::wstring &result,
                  unsigned long long received, const std::wstring &detail)
{
    std::string clean = narrow(resultContext + detail);
    for (char &ch : clean) {
        if (ch == '\r' || ch == '\n' || ch == ',') ch = ' ';
    }
    const auto timestamp = narrow(nowText());
    const auto write = [&](const fs::path &folder) {
        const fs::path csv = folder / "boot_cycle_results.csv";
        const bool newFile = !fs::exists(csv);
        std::ofstream out(csv, std::ios::app | std::ios::binary);
        if (newFile) out << kResultHeader;
        out << timestamp << ',' << narrow(result) << ',' << received << ',' << clean << "\r\n";
        out.flush();
        return out.good();
    };
    const bool historySaved = write(dir);
    const bool workSaved = reportWork.empty() || write(reportWork);
    return historySaved && workSaved;
}

QString createDryRunFolder(const QString &root)
{
    const QString base = root + "/dry_run_results";
    if (!QDir().mkpath(base)) return {};
    QDir folder(base);
    const auto timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    for (int suffix = 1; ; ++suffix) {
        const auto name = suffix == 1 ? timestamp : timestamp + '_' + QString::number(suffix);
        if (folder.mkdir(name)) return folder.filePath(name);
        if (!folder.exists(name)) return {};
    }
}

bool saveRunRecords(const QString &root, const QString &destination, const QString &name,
                    qint64 offset, const QByteArray &header = {})
{
    QFile source(root + '/' + name);
    QByteArray records;
    if (source.exists()) {
        if (!source.open(QIODevice::ReadOnly) || source.size() < offset || !source.seek(offset)) return false;
        records = source.readAll();
        if (source.error() != QFileDevice::NoError) return false;
    }
    if (offset > 0 || records.isEmpty()) records.prepend(header);
    QSaveFile output(destination + '/' + name);
    return output.open(QIODevice::WriteOnly) && output.write(records) == records.size() && output.commit();
}

bool writeReport(const fs::path &dir, const QString &ending, bool dryRun = false, const QString &scope = {},
                 const QString &output = {})
{
    const auto root = QString::fromStdWString(dir.wstring());
    QFile results(root + "/boot_cycle_results.csv");
    QFile log(root + "/boot_cycle_detail.log");
    if ((results.exists() && !results.open(QIODevice::ReadOnly)) || !log.open(QIODevice::ReadOnly))
        return false;

    QString rows;
    int passed = 0;
    int failed = 0;
    while (results.isOpen() && !results.atEnd()) {
        const auto fields = QString::fromUtf8(results.readLine()).trimmed().split(',');
        if (fields.size() != 4 || (fields[1] != "PASS" && fields[1] != "FAIL")) continue;
        passed += fields[1] == "PASS";
        failed += fields[1] == "FAIL";
        rows += "<tr>";
        for (qsizetype i = 0; i < fields.size(); ++i) {
            const auto &field = fields[i];
            const bool flowResult = i == 2 && field == "0"
                && (fields[3].contains("; Flow=") || fields[3].contains("; ImportedFlow="));
            QString shown=flowResult?QStringLiteral("—"):field;
            if(i==1)shown=field=="PASS"?"通過（PASS）":"未通過（FAIL）";
            if(i==3){
                const bool trial=field.contains("DryRun=true");
                const QString mode=field.contains("Mode=warm")?"暖重開機設定":"冷開機設定";
                shown=trial?"試跑：只執行檢查，不關機／不重開機。":mode+"；請參考下方測試項目結果。";
                if(fields[1]=="FAIL")shown+=" 本輪未通過，請查看失敗項目的原因與排查提示。";
                rows+="<td>"+shown.toHtmlEscaped()+"<details><summary>工程紀錄</summary><pre>"+field.toHtmlEscaped()+"</pre></details></td>";
            }else rows+="<td>"+shown.toHtmlEscaped()+"</td>";
        }
        rows += "</tr>";
    }
    struct ItemResult {QString cycle,name,hint,state="未執行";QStringList observations;int total=0,done=0;bool failed=false,stopped=false;};
    QVector<ItemResult> readable;QVector<int> stepRows;QJsonArray planSteps;
    QString steps;
    int rounds = 0;
    QString cycle;
    QString step;
    const QRegularExpression cycleHeader(QStringLiteral("^Cycle=(\\d+);.*FailurePolicy="));
    const auto addStep = [&](const QString &result) {
        if (step.isEmpty()) return;
        steps += "<tr><td>" + cycle + "</td><td>" + step.toHtmlEscaped()
            + "</td><td>" + result + "</td></tr>";
        step.clear();
    };
    while (!log.atEnd()) {
        const auto line = QString::fromUtf8(log.readLine()).trimmed();
        const auto separator = line.indexOf(" | ");
        if (separator < 0) continue;
        const auto message = line.mid(separator + 3);
        if (message == "Boot check started" || message == "Dry-run started") ++rounds;
        if(message.startsWith("FLOW_EVENT ")){
            const auto event=QJsonDocument::fromJson(message.mid(11).toUtf8()).object();
            if(event["event"]=="plan"){
                planSteps=event["steps"].toArray();stepRows.clear();QMap<QString,QJsonObject> items;QMap<QString,int> groups;
                for(const auto &value:event["testItems"].toArray()){const auto item=value.toObject();items.insert(item["id"].toString(),item);}
                for(int i=0;i<planSteps.size();++i){const auto entry=planSteps[i].toObject();const QString id=entry["testItemId"].toString();
                    const QString key=id.isEmpty()?QString("step_%1").arg(i):"group_"+id;
                    if(!groups.contains(key)){ItemResult result;result.cycle=cycle;result.name=id.isEmpty()?entry["title"].toString():items[id]["name"].toString();result.hint=items.value(id)["failureHint"].toString();groups.insert(key,int(readable.size()));readable.append(result);}
                    const int row=groups[key];++readable[row].total;stepRows.append(row);
                }
            }else if(event["event"]=="finish"){
                const int i=event["index"].toInt(-1);
                if(i>=0 && i<stepRows.size()){
                    auto &result=readable[stepRows[i]];const QString state=event["result"].toString();++result.done;
                    result.failed=result.failed || state=="FAIL";result.stopped=result.stopped || state=="STOP";
                    result.state=result.failed?"未通過（FAIL）":result.stopped?"已停止":result.done==result.total?"通過（PASS）":"未完成";
                    QString observation=event["value"].toString().section('\n',0,0).trimmed();
                    if(observation.isEmpty()){const QString type=planSteps[i].toObject()["type"].toString();observation=type.contains("等待")?"已完成等待":type.contains("點擊")?"已完成按鈕操作":state=="PASS"?"此步驟已完成":"此步驟未完成";}
                    const auto detailLines=event["value"].toString().split('\n');
                    for(const auto &detailLine:detailLines)
                        if(detailLine.startsWith("擷取：") || detailLine.startsWith("採樣："))observation+="\n"+detailLine.left(240);
                    if(!result.observations.contains(observation))result.observations.append(observation.left(600));
                }
            }
        }
        const auto match = cycleHeader.match(message);
        if (match.hasMatch()) {
            addStep(QStringLiteral("未完成"));
            cycle = match.captured(1);
        } else if (message.startsWith(QStringLiteral("FLOW | 步驟 "))) {
            addStep(QStringLiteral("未完成"));
            step = message.mid(7);
        } else if (message.startsWith("FLOW | PASS:")) {
            addStep("PASS");
        } else if (message.startsWith("FLOW | FAIL:")) {
            addStep("FAIL");
        }
    }
    addStep(QStringLiteral("未完成"));
    if (results.error() != QFileDevice::NoError || log.error() != QFileDevice::NoError) return false;

    QString itemRows;
    for(const auto &item:readable){QString explanation;
        for(const auto &observation:item.observations)explanation+="<div>"+QString(observation.toHtmlEscaped()).replace('\n',"<br>")+"</div>";
        if(explanation.isEmpty())explanation="尚未執行此項目。";
        if(item.failed && !item.hint.isEmpty())explanation+="<p><b>處理方式：</b>"+item.hint.toHtmlEscaped()+"</p>";
        itemRows+="<tr><td>"+item.cycle.toHtmlEscaped()+"</td><td>"+item.name.toHtmlEscaped()+"</td><td class='"+(item.failed?QString("fail"):item.state.startsWith("通過")?QString("pass"):QString("pending"))+"'>"+item.state.toHtmlEscaped()+"</td><td>"+explanation+"</td></tr>";
    }
    const QString readableSection=itemRows.isEmpty()?QString():"<h2>測試項目與判定說明</h2><table><thead><tr><th>輪次</th><th>測試項目</th><th>結果</th><th>實測結果／未通過原因</th></tr></thead><tbody>"+itemRows+"</tbody></table>";
    QString navigation;
    if (!scope.isEmpty()) {
        navigation = "<p><a href='boot_cycle_detail.log'>詳細紀錄</a>";
        if (results.exists()) navigation += " ｜ <a href='boot_cycle_results.csv'>結果 CSV</a>";
        const QDir evidence(root + "/evidence");
        if (evidence.exists()) {
            navigation += "</p><p>逐輪證據：";
            for (const auto &name : evidence.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const auto href = QString::fromLatin1(QUrl::toPercentEncoding(name));
                navigation += " <a href='evidence/" + href + "/'>" + name.toHtmlEscaped() + "</a>";
            }
        }
        navigation += "</p>";
    }
    const auto html = QStringLiteral(
        "<!doctype html><html lang=\"zh-Hant\"><meta charset=\"utf-8\">"
        "<title>GVFG 測試報告</title><style>"
        "body{font:16px system-ui;margin:2rem}table{border-collapse:collapse;width:100%}"
        "th,td{border:1px solid #bbb;padding:.6rem;text-align:left;vertical-align:top;overflow-wrap:anywhere}th{background:#eef2f6}.pass{color:#16803c}.fail{color:#c62828}details{margin:.6rem 0;color:#526176}pre{white-space:pre-wrap;overflow-wrap:anywhere}"
        "</style><h1>GVFG 測試報告</h1>"
        "<p>產出時間（本機） %1</p><p>結束原因 %2</p>"
        "<p>%7 PASS %3 / FAIL %4</p>%9"
        "<h2>結果明細</h2><table><thead><tr><th>時間</th><th>結果</th>"
        "<th>接收影格數</th><th>明細</th></tr></thead><tbody>%5</tbody></table>"
        "%8%6</html>")
        .arg(QString::fromStdWString(nowText()), ending.toHtmlEscaped(),
             QString::number(passed), QString::number(failed), rows,
             steps.isEmpty() ? QString() : QStringLiteral(
                 "<details><summary>工程明細：原始執行步驟</summary><table><thead><tr><th>輪次</th><th>項目</th>"
                 "<th>結果</th></tr></thead><tbody>") + steps + "</tbody></table></details>",
             !scope.isEmpty() ? QString("%1｜已啟動 %2 輪｜結果紀錄（筆）").arg(scope.toHtmlEscaped()).arg(rounds)
                 : dryRun ? QStringLiteral("本次試跑")
                 : QStringLiteral("累計結果筆數（含試跑及電源操作失敗）"), readableSection, navigation).toUtf8();
    QSaveFile report(output.isEmpty() ? root + "/boot_cycle_report.html" : output);
    return report.open(QIODevice::WriteOnly) && report.write(html) == html.size() && report.commit();
}

bool reportTree(const QString &source, const QString &destination, QString &error)
{
    const QDir folder(source);
    for (const auto &entry : folder.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (entry.isSymLink() || entry.isJunction()) {
            error = "暫存含有連結，請檢查：" + entry.filePath();
            return false;
        }
        const QString target = destination.isEmpty() ? QString() : QDir(destination).filePath(entry.fileName());
        if (entry.isDir()) {
            if (!target.isEmpty() && !QDir().mkpath(target)) { error = "無法建立：" + target; return false; }
            if (!reportTree(entry.filePath(), target, error)) return false;
        } else if (!target.isEmpty() && !QFile::copy(entry.filePath(), target)) {
            error = "無法複製：" + entry.filePath();
            return false;
        }
    }
    return true;
}

bool stopRequested(const fs::path &dir)
{
    return fs::exists(dir / "STOP") || fs::exists(dir / "FAILED");
}

bool waitInterruptible(const fs::path &dir, DWORD seconds)
{
    for (DWORD i = 0; i < seconds; ++i) {
        if (stopRequested(dir)) return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return !stopRequested(dir);
}

void markFailure(const fs::path &dir, const std::wstring &detail)
{
    appendResult(dir, L"FAIL", 0, detail);
    appendDetail(dir, L"FAIL: " + resultContext + detail);
    std::ofstream(dir / "FAILED", std::ios::binary) << narrow(nowText()) << " | " << narrow(detail) << "\r\n";
}

} // namespace

bool recordRelayPreflightFailure(const QString &directory, const QString &detail)
{
    return appendDetail(fs::path(directory.toStdWString()),
        L"RELAY PREFLIGHT FAILED: " + detail.toStdWString());
}

bool recordBootWorkerFailure(const QString &directory, const QString &detail, const QString &session)
{
    const auto message = L"WORKER FAILURE: " + detail.toStdWString();
    const bool saved = appendDetail(fs::path(directory.toStdWString()), message);
    const auto work = reportWorkPath(directory, session);
    return (session.isEmpty() || (!work.isEmpty() && QDir().mkpath(work)
        && appendDetail(fs::path(work.toStdWString()), message))) && saved;
}

bool clearReportWork(const QString &directory, const QString &session, QString &error)
{
    const auto work = reportWorkPath(directory, session);
    if (work.isEmpty()) { error = "本次暫存路徑無效"; return false; }
    if (!QDir(work).exists()) return true;
    if (!reportTree(work, {}, error)) return false;
    if (!QDir(work).removeRecursively()) { error = "無法清除：" + work; return false; }
    return true;
}

static bool writeSessionReport(const QString &work, bool trial, bool warm, const QString &output = {})
{
    QFile ending(work + "/ending.txt");
    const QString outcome = ending.open(QIODevice::ReadOnly)
        ? QString::fromUtf8(ending.readAll()).trimmed() : QStringLiteral("測試紀錄快照");
    const QString scope = QString("%1／%2").arg(trial ? "試跑（不關機）" : "正式測試",
        warm ? "暖重開機" : "冷開機");
    return writeReport(fs::path(work.toStdWString()), outcome, trial, scope, output);
}

QString previewBootSession(const QString &directory, const QString &session, bool trial, bool warm,
                           const QString &output, QString &error)
{
    const auto work = reportWorkPath(directory, session);
    if (work.isEmpty() || !QFileInfo::exists(work + "/boot_cycle_detail.log")) {
        error = "找不到本次測試紀錄";
        return {};
    }
    if (!writeSessionReport(work, trial, warm, output)) {
        error = "無法產生報告預覽";
        return {};
    }
    return work;
}

QString exportBootSession(const QString &directory, const QString &session, bool trial, bool warm, QString &error)
{
    const auto work = reportWorkPath(directory, session);
    if (work.isEmpty() || !QFileInfo::exists(work + "/boot_cycle_detail.log")) {
        error = "找不到本次可匯出的測試紀錄";
        return {};
    }
    const QString base = QDir(directory).absoluteFilePath(QString("reports/%1/%2")
        .arg(trial ? "trial" : "formal", warm ? "warm" : "cold"));
    if (!QDir().mkpath(base)) { error = "無法建立報告資料夾：" + base; return {}; }
    QTemporaryDir staging(base + "/.export_XXXXXX");
    if (!staging.isValid()) { error = staging.errorString(); return {}; }
    if (!reportTree(work, staging.path(), error)) return {};
    if (!QFileInfo::exists(staging.filePath("boot_cycle_results.csv"))
        && !saveRunRecords(work, staging.path(), "boot_cycle_results.csv", 0, kResultHeader)) {
        error = "無法建立結果 CSV";
        return {};
    }
    QFile ending(staging.filePath("ending.txt"));
    if (!writeSessionReport(staging.path(), trial, warm)) {
        error = "無法產生 HTML 報告";
        return {};
    }
    if (ending.exists() && !ending.remove()) { error = ending.errorString(); return {}; }
    const QString name = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss_zzz")
        + '_' + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!QDir(base).rename(QFileInfo(staging.path()).fileName(), name)) {
        error = "無法保存報告資料夾";
        return {};
    }
    return QDir(base).filePath(name);
}

bool regenerateBootReport(const QString &directory){
    return writeReport(fs::path(directory.toStdWString()),"既有紀錄重新產生（沒有重新執行測試）",true);
}

int runBootTestInDirectory(int argc, wchar_t **argv,const QString &directory)
{
    resultContext.clear();
    reportWork.clear();
    const auto resetReportWork = qScopeGuard([] { reportWork.clear(); });
    fs::path dir(directory.toStdWString());
    bool dryRun = false, warmReboot = false, describeAction = false, continueOnFailure = false;
    fs::path targetExe, flowFile;
    QString agentUrl, reportSession;
    quint64 lastFormalCycle = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--dry-run") dryRun = true;
        else if (arg == L"--continue-on-failure") continueOnFailure = true;
        else if (arg == L"--warm-reboot") warmReboot = true;
        else if (arg == L"--describe-action") describeAction = true;
        else if (arg == L"--target-exe" && i + 1 < argc) targetExe = argv[++i];
        else if (arg == L"--flow-file" && i + 1 < argc) flowFile = argv[++i];
        else if (arg == L"--run-directory" && i + 1 < argc) dir = argv[++i];
        else if (arg == L"--agent-url" && i + 1 < argc) agentUrl = QString::fromWCharArray(argv[++i]);
        else if (arg == L"--report-session" && i + 1 < argc) reportSession = QString::fromWCharArray(argv[++i]);
        else if (arg == L"--last-formal-cycle" && i + 1 < argc) {
            bool valid = false;
            lastFormalCycle = QString::fromWCharArray(argv[++i]).toULongLong(&valid);
            if (!valid || lastFormalCycle == 0) return 1;
        }
        else { std::wcerr << L"Unknown argument: " << arg << L'\n'; return 1; }
    }
    const wchar_t *powerCommand = warmReboot
        ? L"shutdown.exe /r /t 0 /d p:4:1 /c \"GVFG warm reboot test\""
        : L"shutdown.exe /s /t 0 /d p:4:1 /c \"GVFG cold boot test\"";
    auto relay = loadRelaySettings(QString::fromStdWString(dir.wstring()));
    if (!agentUrl.isEmpty()) relay.agentUrl = agentUrl;
    const bool remoteRelay = !warmReboot;
    if (describeAction) {
        if (remoteRelay) {
            QString problem;
            if (!dryRun) relayAgentSettingsValid(relay, problem);
            std::wcout << L"Mode=cold\nAction=" << (dryRun ? L"none (dry-run)" : L"remote relay schedule, then Windows shutdown")
                << L"\nAgent=" << relay.agentUrl.toStdWString()
                << L"\nDelaySeconds=" << relay.delaySeconds << L"\nDurationSeconds=" << relay.ticks / 10.0
                << L"\nError=" << problem.toStdWString() << L'\n';
            return problem.isEmpty() ? 0 : 1;
        }
        std::wcout << L"Mode=" << (warmReboot ? L"warm" : L"cold") << L"\nAction="
                   << (dryRun ? L"none (dry-run)" : powerCommand)
                   << L"\nController=not called\n";
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\GVFG_BootCycleTest_Runner");
    if (!mutex) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    if (stopRequested(dir)) {
        appendDetail(dir, L"Runner skipped because STOP or FAILED exists");
        CloseHandle(mutex);
        return 0;
    }

    const auto root = QString::fromStdWString(dir.wstring());
    const QString managedWork = reportSession.isEmpty() ? QString() : reportWorkPath(root, reportSession);
    if (!reportSession.isEmpty() && (managedWork.isEmpty() || !QDir().mkpath(managedWork))) {
        appendDetail(dir, L"FAIL: Cannot create report session folder");
        CloseHandle(mutex);
        return 1;
    }
    if (!managedWork.isEmpty()) reportWork = fs::path(managedWork.toStdWString());
    const bool legacyTrial = dryRun && managedWork.isEmpty();
    const auto dryFolder = legacyTrial ? createDryRunFolder(root) : QString();
    if (legacyTrial && dryFolder.isEmpty()) {
        appendDetail(dir, L"FAIL: Cannot create dry_run_results folder");
        CloseHandle(mutex);
        return 1;
    }
    const QString detailName = "boot_cycle_detail.log", resultsName = "boot_cycle_results.csv";
    const auto logOffset = QFileInfo(root + '/' + detailName).size();
    const auto csvOffset = QFileInfo(root + '/' + resultsName).size();
    if (legacyTrial) appendDetail(dir, L"Dry-run folder: " + dryFolder.toStdWString());
    const auto saveEnding = [&](const QString &ending) {
        if (managedWork.isEmpty()) return true;
        QSaveFile file(managedWork + "/ending.txt");
        const auto data = ending.toUtf8();
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
    };
    saveEnding("執行中或上次執行中斷");
    const auto finishRun = [&](const QString &ending, int exitCode) {
        const bool saved = !managedWork.isEmpty() ? saveEnding(ending) : legacyTrial
            ? saveRunRecords(root, dryFolder, detailName, logOffset)
                && saveRunRecords(root, dryFolder, resultsName, csvOffset, kResultHeader)
                && writeReport(fs::path(dryFolder.toStdWString()), ending, true)
            : writeReport(dir, ending);
        if (!saved) {
            appendDetail(dir, L"FAIL: Cannot save test report or records");
            exitCode = 1;
        }
        CloseHandle(mutex);
        return exitCode;
    };

    if (flowFile.empty()) {
        markFailure(dir, L"Missing required --flow-file JSON path");
        return finishRun(QStringLiteral("尚未匯入 JSON 流程"), 1);
    }

    std::error_code targetError;
    if (targetExe.empty() || !targetExe.is_absolute() || !fs::is_regular_file(targetExe, targetError)) {
        markFailure(dir, L"Target EXE is missing or invalid: " + targetExe.wstring());
        return finishRun(QStringLiteral("目標程式無效"), 1);
    }
    appendDetail(dir, L"Target EXE: " + targetExe.wstring());

    appendDetail(dir, warmReboot ? L"Mode=warm (Windows restart)" : L"Mode=cold (external power cycle)");
    unsigned long long cycle = 1;
    quint64 formalCycles = 0;
    std::ifstream history(dir / "boot_cycle_detail.log", std::ios::binary);
    std::string historyLine;
    while (std::getline(history, historyLine)) {
        if (!historyLine.empty() && historyLine.back() == '\r') historyLine.pop_back();
        const auto separator = historyLine.find(" | ");
        if (separator != std::string::npos) {
            const auto message = historyLine.substr(separator + 3);
            if (message == "Boot check started" || message == "Dry-run started") ++cycle;
            if (message == "Boot check started") ++formalCycles;
        }
    }
    const auto finishAtLimit = [&]() {
        appendDetail(dir, L"Cycle limit reached; no power action");
        QSaveFile marker(QString::fromStdWString((dir / "STOP").wstring()));
        const QByteArray reason("Cycle limit reached\r\n");
        if (!marker.open(QIODevice::WriteOnly) || marker.write(reason) != reason.size() || !marker.commit()) {
            markFailure(dir, L"Cannot save cycle limit stop marker");
            return finishRun(QStringLiteral("無法保存輪數完成狀態"), 1);
        }
        return finishRun(QStringLiteral("已達設定測試輪數"), 0);
    };
    if (!dryRun && lastFormalCycle != 0 && formalCycles >= lastFormalCycle)
        return finishAtLimit();
    const bool finalCycle = !dryRun && lastFormalCycle != 0 && formalCycles + 1 == lastFormalCycle;
    const auto withRelayReconnect = [&](const auto &request, const QString &phase, QString &problem) {
        bool recovering = false;
        while (!stopRequested(dir)) {
            bool retryable = false;
            if (request(retryable)) {
                if (recovering) appendDetail(dir, L"RELAY RECONNECTED: " + phase.toStdWString());
                return true;
            }
            if (!retryable || stopRequested(dir)) return false;
            recovering = true;
            appendDetail(dir, L"RELAY RECONNECT: " + phase.toStdWString() + L"：" + problem.toStdWString());
            if (!waitInterruptible(dir, 3)) return false;
        }
        return false;
    };
    if (!dryRun && remoteRelay) {
        int channel = 0;
        QString problem;
        if (!withRelayReconnect([&](bool &retryable) {
            return checkRelayAgent(relay, channel, problem, true, &retryable);
        }, "正式測試前連線檢查", problem)) {
            if (stopRequested(dir)) {
                appendDetail(dir, L"RELAY PREFLIGHT STOPPED: cancelled while waiting for Relay Agent");
                return finishRun(QStringLiteral("測試已停止"), 0);
            }
            appendDetail(dir, L"RELAY PREFLIGHT FAILED: " + problem.toStdWString()
                + L" [Agent=" + relay.agentUrl.toStdWString() + L"]");
            return finishRun(QStringLiteral("控制端連線檢查未通過"), 1);
        }
    }
    resultContext = L"Cycle=" + std::to_wstring(cycle) + (warmReboot ? L"; Mode=warm; " : L"; Mode=cold; ")
        + (dryRun ? L"DryRun=true; " : L"DryRun=false; ");
    appendDetail(dir, dryRun ? L"Dry-run started" : L"Boot check started");
    appendDetail(dir, resultContext + (continueOnFailure ? L"FailurePolicy=continue" : L"FailurePolicy=stop"));
    if (!waitInterruptible(dir, dryRun ? 1 : kStartupDelaySeconds)) {
        appendDetail(dir, L"Stopped before application launch");
        return finishRun(QStringLiteral("測試已停止"), 0);
    }

    std::wstring error;
    const auto flowPath = flowFile.wstring();
    appendDetail(dir, L"Running JSON flow: " + flowPath);
    const QString evidence = !managedWork.isEmpty() ? managedWork + QString("/evidence/round_%1").arg(cycle, 6, 10, QChar('0'))
        : legacyTrial ? dryFolder + "/evidence" : QString();
    const int checkResult = executeBootFlow(flowFile.wstring(), dir.wstring(), targetExe.wstring(),
        [&](const std::wstring &message) {appendDetail(dir, message);}, error,
        evidence.toStdWString());
    if (checkResult == 2)
        appendDetail(dir, L"STOP detected during JSON flow; no power action");
    if (checkResult == 0) {
        if (!appendResult(dir, L"PASS", 0, L"Flow=" + flowPath)) {
            markFailure(dir, L"Cannot save PASS record");
            return finishRun(QStringLiteral("紀錄寫入失敗，測試終止"), 1);
        }
        appendDetail(dir, L"PASS: flow=" + flowPath);
    }
    if (checkResult == 2 || stopRequested(dir))
        return finishRun(QStringLiteral("測試已停止"), 0);
    if (checkResult != 0) {
        if (!continueOnFailure) {
            markFailure(dir, error);
            return finishRun(QStringLiteral("檢查失敗，測試終止"), 1);
        }
        const bool resultSaved = appendResult(dir, L"FAIL", 0, error);
        const bool detailSaved = appendDetail(dir, L"FAIL: " + resultContext + error);
        if (!resultSaved || !detailSaved) {
            markFailure(dir, L"Cannot save failure records; continuation aborted");
            return finishRun(QStringLiteral("紀錄寫入失敗，測試終止"), 1);
        }
        if (!finalCycle && !dryRun)
            appendDetail(dir, L"CONTINUE_AFTER_FAILURE: check failed; next boot cycle will run");
    }

    if (dryRun) {
        appendDetail(dir, L"Dry-run complete; controller command and shutdown skipped");
        return finishRun(QStringLiteral("試跑完成"), checkResult == 0 ? 0 : 1);
    }
    if (finalCycle) return finishAtLimit();

    if (remoteRelay) {
        if (stopRequested(dir)) return finishRun(QStringLiteral("測試已停止"), 0);
        QString id, problem;
        auto cancel = [&] {
            if (id.isEmpty()) return;
            QString cancelError;
            if (cancelRelayAgent(relay, id, cancelError))
                appendDetail(dir, L"RELAY AGENT CANCELLED: " + id.toStdWString());
            else
                appendDetail(dir, L"RELAY AGENT CANCEL UNCONFIRMED: " + id.toStdWString() + L" " + cancelError.toStdWString());
        };
        bool stoppedByAgent = false;
        if (!withRelayReconnect([&](bool &retryable) {
            return scheduleRelayAgent(relay, id, problem, stoppedByAgent, &retryable);
        }, "確認斷電排程", problem)) {
            cancel();
            if (stoppedByAgent || fs::exists(dir / "STOP")) {
                if (!fs::exists(dir / "STOP")) {
                    QSaveFile marker(root + "/STOP");
                    const QByteArray reason("Stop requested by Relay Agent\r\n");
                    if (!marker.open(QIODevice::WriteOnly) || marker.write(reason) != reason.size() || !marker.commit()) {
                        markFailure(dir, L"Cannot save Relay Agent stop marker");
                        return finishRun(QStringLiteral("無法保存停止狀態"), 1);
                    }
                }
                appendDetail(dir, stoppedByAgent ? L"STOP detected by Relay Agent during scheduling"
                    : L"STOP detected during relay scheduling");
                return finishRun(QStringLiteral("測試已停止"), 0);
            }
            markFailure(dir, L"控制端 排程未確認：" + problem.toStdWString()
                + L" [Agent=" + relay.agentUrl.toStdWString() + L", ID=" + id.toStdWString() + L"]");
            return finishRun(QStringLiteral("控制端 排程未確認，測試終止"), 1);
        }
        appendDetail(dir, L"RELAY AGENT ACCEPTED: " + id.toStdWString());
        if (stopRequested(dir)) {
            cancel();
            appendDetail(dir, L"STOP detected after relay scheduling");
            return finishRun(QStringLiteral("測試已停止"), 0);
        }
        appendDetail(dir, L"Requesting normal Windows shutdown");
        saveEnding("已要求 Windows 關機，等待下一輪續跑");
        if (_wsystem(powerCommand) != 0) {
            cancel();
            markFailure(dir, L"Windows shutdown failed after relay scheduling");
            return finishRun(QStringLiteral("Windows 電源操作失敗，測試終止"), 1);
        }
        CloseHandle(mutex);
        return 0;
    }

    appendDetail(dir, L"Waiting 15 seconds before warm restart");
    if (!waitInterruptible(dir, 15)) {
        appendDetail(dir, L"STOP detected; no Windows restart");
        return finishRun(QStringLiteral("測試已停止"), 0);
    }
    appendDetail(dir, L"Requesting Windows warm restart");
    saveEnding("已要求 Windows 重新啟動，等待下一輪續跑");
    const int rc = _wsystem(powerCommand);
    if (rc != 0) {
        markFailure(dir, L"shutdown.exe failed with code " + std::to_wstring(rc));
        return finishRun(QStringLiteral("Windows 電源操作失敗，測試終止"), 1);
    }
    CloseHandle(mutex);
    return 0;
}



int runBootTest(int argc,wchar_t **argv)
{
    return runBootTestInDirectory(argc, argv, QCoreApplication::applicationDirPath());
}
