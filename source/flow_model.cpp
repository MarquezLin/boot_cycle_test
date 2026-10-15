#include "flow_model.h"
#include "ocr_reader.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <cmath>

QStringList flowTypes() { return {"launch", "waitWindow", "clickUIA", "clickPoint", "typeText", "hotkey", "delay", "readUIA", "readOCR", "assert", "checkImage"}; }
QString flowTypeName(const QString &type)
{
    const QStringList labels{"啟動 APP", "等待視窗", "點擊 UIA 控制項", "點擊視窗位置", "輸入文字", "鍵盤快捷鍵", "等待時間", "讀取 UIA 文字", "框選 OCR 讀取", "判定讀取值", "檢查預覽畫面"};
    const int index = flowTypes().indexOf(type);
    return index < 0 ? type : labels[index];
}
bool saveFlow(const QString &path, const TestFlow &flow, QString &error)
{
    QJsonArray steps;
    for (const auto &s : flow.steps) steps.append(QJsonObject{{"type", s.type}, {"name", s.name}, {"selector", s.selector},
        {"automationId", s.automationId}, {"text", s.text}, {"condition", s.condition}, {"expected", s.expected}, {"windowTitle", s.windowTitle},
        {"timeoutMs", s.timeoutMs}, {"x", s.region.x()}, {"y", s.region.y()}, {"width", s.region.width()}, {"height", s.region.height()}});
    QJsonObject root{{"schemaVersion", 1}, {"executable", flow.executable}, {"arguments", flow.arguments},
        {"workingDirectory", flow.workingDirectory}, {"ocrExecutable", flow.ocrExecutable}, {"ocrLanguage", flow.ocrLanguage}, {"reuseExisting", flow.reuseExisting}, {"steps", steps}};
    QSaveFile file(path);
    const auto data = QJsonDocument(root).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {error = file.errorString(); return false;}
    return true;
}
bool loadFlow(const QString &path, TestFlow &flow, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {error = file.errorString(); return false;}
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    const auto root = document.object();
    if (parse.error != QJsonParseError::NoError || !document.isObject() || root.value("schemaVersion").toInt() != 1
        || !root.value("steps").isArray()) {error = "不是支援的流程 JSON（schemaVersion=1）"; return false;}
    TestFlow loaded;
    loaded.executable = root.value("executable").toString(); loaded.arguments = root.value("arguments").toString();
    loaded.workingDirectory = root.value("workingDirectory").toString(); loaded.ocrExecutable = root.value("ocrExecutable").toString();
    loaded.ocrLanguage = root.value("ocrLanguage").toString("eng");
    loaded.reuseExisting = root.value("reuseExisting").toBool(true);
    for (const auto value : root.value("steps").toArray()) {
        if (!value.isObject()) {error = "步驟格式錯誤"; return false;}
        const auto o = value.toObject(); FlowStep s;
        s.type = o.value("type").toString(); s.name = o.value("name").toString(); s.selector = o.value("selector").toString();
        s.automationId = o.value("automationId").toString(); s.text = o.value("text").toString(); s.windowTitle = o.value("windowTitle").toString();
        s.condition = o.value("condition").toString("contains"); s.expected = o.value("expected").toString(); s.timeoutMs = o.value("timeoutMs").toInt(10000);
        s.region = QRect(o.value("x").toInt(), o.value("y").toInt(), o.value("width").toInt(), o.value("height").toInt());
        loaded.steps.append(s);
    }
    flow = loaded; return true;
}
bool evaluateFlowValue(const QString &raw, const QString &condition, const QString &expected, QString &parsed, QString &error)
{
    parsed = raw.trimmed(); error.clear();
    if (condition == "equalsIgnoreCase") {
        QString left = parsed, right = expected;
        const QRegularExpression whitespace("\\s+"); left.remove(whitespace); right.remove(whitespace);
        return left.compare(right, Qt::CaseInsensitive) == 0;
    }
    if (condition == "containsIgnoreCase") return parsed.contains(expected, Qt::CaseInsensitive);
    if (condition == "equals") return parsed == expected;
    if (condition == "contains") return parsed.contains(expected);
    if (condition == "regex") {
        QRegularExpression pattern(expected);
        if (!pattern.isValid()) {error = "正規表示式無效：" + pattern.errorString(); return false;}
        return pattern.match(parsed).hasMatch();
    }
    static const QRegularExpression number(R"([-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?)");
    const auto match = number.match(parsed);
    if (!match.hasMatch()) {error = "讀取值沒有可解析的數字"; return false;}
    parsed = match.captured(); bool ok = false;
    const double actual = parsed.toDouble(&ok);
    if (!ok || !std::isfinite(actual)) {error = "數值格式無效"; return false;}
    if (condition == "range") {
        const auto bounds = expected.split(','); bool a = false, b = false;
        const double low = bounds.value(0).trimmed().toDouble(&a), high = bounds.value(1).trimmed().toDouble(&b);
        if (bounds.size() != 2 || !a || !b || !std::isfinite(low) || !std::isfinite(high) || low > high) {error = "範圍請填最小值,最大值"; return false;}
        return actual >= low && actual <= high;
    }
    const double value = expected.toDouble(&ok);
    if (!ok || !std::isfinite(value)) {error = "預期值必須是有效數字"; return false;}
    if (condition == "gt") return actual > value;
    if (condition == "ge") return actual >= value;
    if (condition == "lt") return actual < value;
    if (condition == "le") return actual <= value;
    if (condition == "numberEquals") return actual == value;
    error = "不支援的判定條件"; return false;
}
bool validateFlow(const TestFlow &flow, QString &error, int singleStep, bool hasRead)
{
    if (!QFileInfo(flow.executable).isFile() || !QFileInfo(flow.executable).isAbsolute()) {error = "請選擇存在的待測 EXE 絕對路徑"; return false;}
    if (!flow.workingDirectory.isEmpty() && !QFileInfo(flow.workingDirectory).isDir()) {error = "工作目錄不存在"; return false;}
    if (flow.steps.isEmpty()) {error = "請新增測試步驟"; return false;}
    if (singleStep < -1 || singleStep >= flow.steps.size()) {error = "請選取有效的測試步驟"; return false;}
    const int first = singleStep < 0 ? 0 : singleStep;
    const int end = singleStep < 0 ? static_cast<int>(flow.steps.size()) : singleStep + 1;
    for (int i = first; i < end; ++i) {
        const auto &s = flow.steps[i]; QString reason;
        if (!flowTypes().contains(s.type)) reason = "不支援的步驟";
        else if (s.timeoutMs < 1 || s.timeoutMs > 600000) reason = "時間必須介於 1～600000 毫秒";
        else if ((s.type == "clickUIA" || s.type == "readUIA") && s.selector.isEmpty() && s.automationId.isEmpty()) reason = "請指定控制項名稱或 AutomationId";
        else if (s.selector.startsWith("regex:") && !QRegularExpression(s.selector.mid(6)).isValid()) reason = "UIA 名稱正規式無效";
        else if ((s.type == "readUIA" || s.type == "readOCR") && !s.text.isEmpty()
                 && (!QRegularExpression(s.text).isValid() || QRegularExpression(s.text).captureCount() < 1)) reason = "擷取正規式必須有效且包含括號群組";
        else if (s.type == "readOCR" && detectOcrExecutable(flow.ocrExecutable).isEmpty()) reason = "找不到文字辨識引擎，請保留套件內的 ocr 資料夾";
        else if ((s.type == "readOCR" || s.type == "checkImage") && (s.region.x() < 0 || s.region.y() < 0 || s.region.width() < 1 || s.region.height() < 1)) reason = "請框選有效 OCR／影像區域";
        else if (s.type == "clickPoint" && (s.region.x() < 0 || s.region.y() < 0)) reason = "點擊座標不可小於零";
        else if (s.type == "hotkey" && !QStringList{"ENTER", "TAB", "ESC", "CTRL+A", "CTRL+C", "CTRL+V", "ALT+F4"}.contains(s.text.toUpper())) reason = "支援 Enter、Tab、Esc、Ctrl+A/C/V、Alt+F4";
        else if (s.type == "assert") {
            QString parsed, problem;
            evaluateFlowValue("0", s.condition, s.expected, parsed, problem);
            if (!hasRead) reason = "判定前必須先加入讀取步驟";
            else if (!problem.isEmpty()) reason = problem;
            else if (s.expected.isEmpty()) reason = "預期值不可留空";
        }
        if (!reason.isEmpty()) {error = QString("步驟 %1：%2").arg(i + 1).arg(reason); return false;}
        if (s.type == "checkImage") {
            bool ok = false; const double threshold = s.expected.toDouble(&ok);
            if (!ok || !std::isfinite(threshold) || threshold <= 0 || threshold > 255
                || (s.condition != "contrast" && s.condition != "change")) {
                error = QString("步驟 %1：影像條件須為有內容／有變化，預期門檻須大於 0 且不超過 255").arg(i + 1); return false;
            }
            if (s.condition == "change" && s.timeoutMs < 600) {error = "畫面變化檢查的逾時請至少設定 600 毫秒"; return false;}
        }
        if ((s.type == "readUIA" || s.type == "readOCR") && !s.expected.isEmpty()) {
            QString parsed, problem; evaluateFlowValue("0", s.condition, s.expected, parsed, problem);
            if (!problem.isEmpty()) {error = QString("步驟 %1：%2").arg(i + 1).arg(problem); return false;}
        }
        if (s.type == "readOCR" || s.type == "readUIA") hasRead = true;
    }
    return true;
}
bool flowModelSelfTest()
{
    QString value, error;
    if (!evaluateFlowValue("received=64 frames", "gt", "0", value, error) || value != "64") return false;
    if (evaluateFlowValue("no signal", "gt", "0", value, error) || error.isEmpty()) return false;
    if (!evaluateFlowValue("59.9 FPS", "range", "59,61", value, error)) return false;
    if (evaluateFlowValue("Connected", "equals", "Disconnected", value, error)) return false;
    QTemporaryDir dir; TestFlow source, loaded;
    source.executable = "C:/測試 APP/target.exe";
    source.reuseExisting = false;
    FlowStep step; step.type = "readOCR"; step.region = QRect(10, 20, 120, 40); step.name = "讀取狀態"; step.windowTitle = "預覽視窗"; source.steps.append(step);
    if (!saveFlow(dir.path() + "/flow.json", source, error) || !loadFlow(dir.path() + "/flow.json", loaded, error)) return false;
    TestFlow check;
    check.executable = dir.path() + "/target.exe";
    QFile target(check.executable); if (!target.open(QIODevice::WriteOnly)) return false; target.close();
    FlowStep unconfigured; unconfigured.type = "clickUIA";
    FlowStep read; read.type = "readUIA"; read.selector = "Running";
    FlowStep assertion; assertion.type = "assert"; assertion.condition = "equals"; assertion.expected = "Running";
    check.steps = {unconfigured, read, assertion};
    if (validateFlow(check, error) || !validateFlow(check, error, 1) || validateFlow(check, error, 0)
        || validateFlow(check, error, 2) || !validateFlow(check, error, 2, true)) return false;
    check.steps[1].selector.clear();
    if (validateFlow(check, error, 1) || !error.startsWith("步驟 2：")) return false;
    return !loaded.reuseExisting && TestFlow().reuseExisting && loaded.executable == source.executable && loaded.steps.size() == 1 && loaded.steps[0].region == step.region && loaded.steps[0].windowTitle == step.windowTitle;
}
