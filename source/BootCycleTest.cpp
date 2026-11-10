#include <windows.h>
#include "boot_flow_runner.h"
#include "relay_controller.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
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
constexpr auto kResultHeader = "time,result,received_frames,detail\r\n";

constexpr DWORD kStartupDelaySeconds = 20;

fs::path executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

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

bool appendDetail(const fs::path &dir, const std::wstring &message)
{
    std::ofstream out(dir / "boot_cycle_detail.log", std::ios::app | std::ios::binary);
    out << narrow(nowText()) << " | " << narrow(message) << "\r\n";
    out.flush();
    return out.good();
}

bool appendResult(const fs::path &dir, const std::wstring &result,
                  unsigned long long received, const std::wstring &detail)
{
    const fs::path csv = dir / "boot_cycle_results.csv";
    const bool newFile = !fs::exists(csv);
    std::ofstream out(csv, std::ios::app | std::ios::binary);
    if (newFile) out << kResultHeader;
    std::string clean = narrow(resultContext + detail);
    for (char &ch : clean) {
        if (ch == '\r' || ch == '\n' || ch == ',') ch = ' ';
    }
    out << narrow(nowText()) << ',' << narrow(result) << ',' << received << ',' << clean << "\r\n";
    out.flush();
    return out.good();
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

bool writeReport(const fs::path &dir, const QString &ending, bool dryRun = false)
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
    const auto html = QStringLiteral(
        "<!doctype html><html lang=\"zh-Hant\"><meta charset=\"utf-8\">"
        "<title>GVFG 測試報告</title><style>"
        "body{font:16px system-ui;margin:2rem}table{border-collapse:collapse;width:100%}"
        "th,td{border:1px solid #bbb;padding:.6rem;text-align:left;vertical-align:top;overflow-wrap:anywhere}th{background:#eef2f6}.pass{color:#16803c}.fail{color:#c62828}details{margin:.6rem 0;color:#526176}pre{white-space:pre-wrap;overflow-wrap:anywhere}"
        "</style><h1>GVFG 測試報告</h1>"
        "<p>產出時間（本機） %1</p><p>結束原因 %2</p>"
        "<p>%7 PASS %3 / FAIL %4</p>"
        "<h2>結果明細</h2><table><thead><tr><th>時間</th><th>結果</th>"
        "<th>接收影格數</th><th>明細</th></tr></thead><tbody>%5</tbody></table>"
        "%8%6</html>")
        .arg(QString::fromStdWString(nowText()), ending.toHtmlEscaped(),
             QString::number(passed), QString::number(failed), rows,
             steps.isEmpty() ? QString() : QStringLiteral(
                 "<details><summary>工程明細：原始執行步驟</summary><table><thead><tr><th>輪次</th><th>項目</th>"
                 "<th>結果</th></tr></thead><tbody>") + steps + "</tbody></table></details>",
             dryRun ? QStringLiteral("本次試跑") : QStringLiteral("累計結果筆數（含試跑及電源操作失敗）"),readableSection).toUtf8();
    QSaveFile report(root + "/boot_cycle_report.html");
    return report.open(QIODevice::WriteOnly) && report.write(html) == html.size() && report.commit();
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

enum class PowerCycleRequest { NotConfigured, PulseSent, Failed };

PowerCycleRequest requestExternalPowerCycle(const fs::path &dir, const RelaySettings &settings, std::wstring &error)
{
    if (settings.enabled) {
        QString problem;
        const bool sent = sendRelayPulse(settings,
            [&](const QString &message) {appendDetail(dir, message.toStdWString());}, problem);
        error = problem.toStdWString();
        return sent ? PowerCycleRequest::PulseSent : PowerCycleRequest::Failed;
    }
    error.clear();
    appendDetail(dir, L"CONTROLLER_NOT_CONFIGURED: UART placeholder; no command sent. "
                      L"Shutdown only; external power restoration is required.");
    return PowerCycleRequest::NotConfigured;
}

} // namespace

bool regenerateBootReport(const QString &directory){
    return writeReport(fs::path(directory.toStdWString()),"既有紀錄重新產生（沒有重新執行測試）",true);
}

int runBootTest(int argc, wchar_t **argv)
{
    const fs::path dir = executableDirectory();
    bool dryRun = false, warmReboot = false, describeAction = false, continueOnFailure = false;
    fs::path targetExe, flowFile;
    quint64 lastFormalCycle = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--dry-run") dryRun = true;
        else if (arg == L"--continue-on-failure") continueOnFailure = true;
        else if (arg == L"--warm-reboot") warmReboot = true;
        else if (arg == L"--describe-action") describeAction = true;
        else if (arg == L"--target-exe" && i + 1 < argc) targetExe = argv[++i];
        else if (arg == L"--flow-file" && i + 1 < argc) flowFile = argv[++i];
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
    const auto relay = loadRelaySettings(QString::fromStdWString(dir.wstring()));
    if (describeAction) {
        if (!warmReboot && relay.enabled) {
            QString problem; const auto command = describeRelayCommand(relay, problem);
            std::wcout << L"Mode=cold\nAction=" << (dryRun ? L"none (dry-run)" : L"relay timed pulse; no Windows shutdown")
                << L"\nDelaySeconds=" << relay.delaySeconds << L"\nDurationSeconds=" << relay.ticks / 10.0
                << L"\nTX=" << command.toStdWString() << L"\nError=" << problem.toStdWString() << L'\n';
            return problem.isEmpty() ? 0 : 1;
        }
        std::wcout << L"Mode=" << (warmReboot ? L"warm" : L"cold") << L"\nAction="
                   << (dryRun ? L"none (dry-run)" : powerCommand)
                   << L"\nController=" << (!dryRun && !warmReboot ? L"UART placeholder" : L"not called") << L'\n';
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
    const auto dryFolder = dryRun ? createDryRunFolder(root) : QString();
    if (dryRun && dryFolder.isEmpty()) {
        appendDetail(dir, L"FAIL: Cannot create dry_run_results folder");
        CloseHandle(mutex);
        return 1;
    }
    const QString detailName = "boot_cycle_detail.log", resultsName = "boot_cycle_results.csv";
    const auto logOffset = QFileInfo(root + '/' + detailName).size();
    const auto csvOffset = QFileInfo(root + '/' + resultsName).size();
    if (dryRun) appendDetail(dir, L"Dry-run folder: " + dryFolder.toStdWString());
    const auto finishRun = [&](const QString &ending, int exitCode) {
        const bool saved = dryRun
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
    const int checkResult = executeBootFlow(flowFile.wstring(), dir.wstring(), targetExe.wstring(),
        [&](const std::wstring &message) {appendDetail(dir, message);}, error,
        dryRun ? (dryFolder + "/evidence").toStdWString() : std::wstring());
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

    // The supplied relay protocol switches immediately, then restores itself.
    // Pre-switch delay is host-side; it is not a delayed shutdown-safe sequence.
    const DWORD delay = !warmReboot && relay.enabled ? DWORD(relay.delaySeconds) : 15;
    QString relayError;
    if (!warmReboot && relay.enabled && !relaySettingsValid(relay, relayError)) {
        markFailure(dir, relayError.toStdWString());
        return finishRun(QStringLiteral("繼電器設定錯誤，測試終止"), 1);
    }
    if (!warmReboot && relay.enabled)
        appendDetail(dir, L"RELAY COUNTDOWN: " + std::to_wstring(delay) + L" seconds before direct power cut; STOP cancels before send");
    else appendDetail(dir, warmReboot ? L"Waiting 15 seconds before warm restart"
                                : L"Waiting 15 seconds before controller request and normal shutdown");
    if (!waitInterruptible(dir, delay)) {
        appendDetail(dir, L"STOP detected; no controller command or shutdown");
        return finishRun(QStringLiteral("測試已停止"), 0);
    }
    const auto powerRequest = warmReboot ? PowerCycleRequest::NotConfigured
                                        : requestExternalPowerCycle(dir, relay, error);
    if (powerRequest == PowerCycleRequest::Failed) {
        markFailure(dir, L"External controller request failed: " + error);
        return finishRun(QStringLiteral("外部電源控制失敗，測試終止"), 1);
    }
    if (powerRequest == PowerCycleRequest::PulseSent) {
        appendDetail(dir, L"Relay pulse accepted; Windows shutdown skipped; restoration is module-timed");
        CloseHandle(mutex); return 0;
    }
    appendDetail(dir, warmReboot ? L"Requesting Windows warm restart" : L"Requesting normal Windows shutdown");
    const int rc = _wsystem(powerCommand);
    if (rc != 0) {
        markFailure(dir, L"shutdown.exe failed with code " + std::to_wstring(rc));
        return finishRun(QStringLiteral("Windows 電源操作失敗，測試終止"), 1);
    }
    CloseHandle(mutex);
    return 0;
}


