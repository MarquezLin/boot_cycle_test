#include "flow_model.h"
#include "ocr_reader.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QSet>
#include <QMap>
#include <cmath>

QStringList flowTypes() { return {"launch", "waitWindow", "clickUIA", "clickPoint", "typeText", "hotkey", "delay", "readUIA", "readOCR", "assert", "checkImage", "checkVideo"}; }
QString flowTypeName(const QString &type)
{
    const QStringList labels{"啟動 APP", "等待視窗", "點擊 UIA 控制項", "點擊視窗位置", "輸入文字", "鍵盤快捷鍵", "等待時間", "讀取 UIA 文字", "框選 OCR 讀取", "判定讀取值", "檢查預覽畫面", "參考影片比較"};
    const int index = flowTypes().indexOf(type);
    return index < 0 ? type : labels[index];
}
namespace {
QString fieldContext(QString prefix) {
    prefix=prefix.trimmed();prefix.remove(QRegularExpression("[=:\\s]+$"));
    const auto parts=prefix.split('|');
    if(parts.size()>1) {
        const QString first=parts.first().trimmed(),last=parts.last().trimmed();
        return last.isEmpty() || last==first?first:first+" · "+last;
    }
    return prefix;
}
QString legacyValueLabel(const QString &pattern) {
    int capture=-1;bool characterClass=false;
    for(int i=0;i<pattern.size();++i) {
        if(pattern[i]=='\\'){++i;continue;}
        if(pattern[i]=='[')characterClass=true;else if(pattern[i]==']')characterClass=false;
        else if(!characterClass && pattern[i]=='(' && i+1<pattern.size() && pattern[i+1]!='?'){capture=i;break;}
    }
    if(capture<0)return {};
    QString prefix=pattern.left(capture);
    prefix.remove("(?m)^[ \\t]*");
    prefix.replace(R"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)","#");
    prefix.replace(R"([+-]?[0-9]+(?:\.[0-9]+)?)","#");
    prefix.replace(QRegularExpression(R"(\\([^A-Za-z0-9]))"),"\\1");
    return fieldContext(prefix).left(100);
}
}
bool flowAutoName(const QString &name) {return name.startsWith("確認「") && name.endsWith("」顯示文字");}
QString flowCheckSummary(const FlowStep &step) {
    if(step.type=="checkVideo")return "參考影片："+QFileInfo(step.referenceVideo).fileName()+"；連續觀察至少 3 秒";
    if(step.type!="readUIA" && step.type!="readOCR" && step.type!="assert")return {};
    const QString label=!step.valueLabel.isEmpty()?step.valueLabel:legacyValueLabel(step.text);
    const QMap<QString,QString> symbols{{"gt",">"},{"ge","≥"},{"lt","<"},{"le","≤"},{"numberEquals","="}};
    if(symbols.contains(step.condition))return (label.isEmpty()?QString("選定數值"):label)+" "+symbols.value(step.condition)+" "+step.expected;
    if(step.condition=="range")return (label.isEmpty()?QString("選定數值"):label)+" ∈ ["+step.expected+"]";
    if(step.expected.isEmpty())return label.isEmpty()?QString("只讀取資料，未設定 PASS 條件"):"讀取 "+label;
    return QString(step.condition.startsWith("contains")?"文字包含「":"文字判定「")+step.expected.left(80)+"」";
}
QString flowStepDisplayName(const FlowStep &step) {
    const QString summary=flowCheckSummary(step);
    return flowAutoName(step.name) && !summary.isEmpty()?summary:step.name;
}
QString flowItemDisplayName(const TestFlow &flow,const FlowTestItem &item) {
    if(!flowAutoName(item.name))return item.name;
    QStringList summaries;for(const auto &step:flow.steps)if(step.testItemId==item.id){const auto summary=flowCheckSummary(step);if(!summary.isEmpty() && !summaries.contains(summary))summaries.append(summary);}
    return summaries.isEmpty()?item.name:summaries.join("；");
}
QVector<FlowNumberField> flowNumberFields(const QString &raw)
{
    const QString number=R"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)";
    const QRegularExpression token(QStringLiteral("(?<![\\w.])")+number);
    QVector<FlowNumberField> fields;const auto lines=raw.split('\n');
    for(int lineIndex=0;lineIndex<lines.size();++lineIndex) {
        const QString line=lines[lineIndex].trimmed();QVector<QRegularExpressionMatch> numbers;
        auto matches=token.globalMatch(line);while(matches.hasNext())numbers.append(matches.next());
        for(int selected=0;selected<numbers.size();++selected) {
            QString pattern="(?m)^[ \\t]*";qsizetype offset=0;
            for(int i=0;i<numbers.size();++i) {
                const auto &match=numbers[i];pattern+=QRegularExpression::escape(line.mid(offset,match.capturedStart()-offset));
                pattern+=i==selected?'('+number+')':number;offset=match.capturedEnd();
            }
            pattern+=QRegularExpression::escape(line.mid(offset))+"[ \\t]*\\r?$";
            auto unique=QRegularExpression(pattern).globalMatch(raw);if(!unique.hasNext())continue;
            const auto first=unique.next();if(unique.hasNext())continue;
            const auto &match=numbers[selected];
            QString name=fieldContext(line.left(match.capturedStart()));
            const auto unit=QRegularExpression("^\\s*([A-Za-z%℃°]+)").match(line.mid(match.capturedEnd()));
            if(unit.hasMatch())name+=" ("+unit.captured(1)+")";
            fields.append({QString("第 %1 行｜%2 → %3").arg(lineIndex+1).arg(line.left(match.capturedStart()).right(70).trimmed(),match.captured()),pattern,first.captured(1),name});
        }
    }
    return fields;
}
bool saveFlow(const QString &path, const TestFlow &flow, QString &error)
{
    if (!validateFlowItems(flow, error)) return false;
    QJsonArray steps;
    for (const auto &s : flow.steps) steps.append(QJsonObject{{"type", s.type}, {"name", s.name}, {"selector", s.selector},
        {"automationId", s.automationId}, {"text", s.text}, {"condition", s.condition}, {"expected", s.expected}, {"windowTitle", s.windowTitle},
        {"timeoutMs", s.timeoutMs}, {"x", s.region.x()}, {"y", s.region.y()}, {"width", s.region.width()}, {"height", s.region.height()}, {"testItemId", s.testItemId}, {"valueLabel",s.valueLabel}, {"referenceVideo",s.referenceVideo},{"captureMode",s.captureMode},{"sampleFps",s.sampleFps},{"captureDurationMs",s.captureDurationMs},{"activateBeforeCapture",s.activateBeforeCapture}});
    QJsonArray items;
    for (const auto &item : flow.testItems) items.append(QJsonObject{{"id", item.id}, {"name", item.name},
        {"instruction", item.instruction}, {"runningText", item.runningText}, {"failureHint", item.failureHint}});
    QJsonObject root{{"schemaVersion", 1}, {"executable", flow.executable}, {"arguments", flow.arguments},
        {"workingDirectory", flow.workingDirectory}, {"ocrExecutable", flow.ocrExecutable}, {"ocrLanguage", flow.ocrLanguage}, {"reuseExisting", flow.reuseExisting}, {"steps", steps},
        {"name", flow.name}, {"version", flow.version}, {"testItems", items}};
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
    loaded.name = root.value("name").toString(); loaded.version = root.value("version").toString();
    if (root.contains("testItems") && !root.value("testItems").isArray()) {error = "testItems 須為測試項目陣列"; return false;}
    for (const auto &value : root.value("testItems").toArray()) {
        if (!value.isObject()) {error = "測試項目格式錯誤"; return false;}
        const auto item = value.toObject();
        loaded.testItems.append({item["id"].toString(), item["name"].toString(), item["instruction"].toString(), item["runningText"].toString(), item["failureHint"].toString()});
    }
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
        s.testItemId = o.value("testItemId").toString();
        s.valueLabel = o.value("valueLabel").toString();
        s.referenceVideo = o.value("referenceVideo").toString();
        s.captureMode=o.value("captureMode").toString("window");s.sampleFps=o.value("sampleFps").toInt(30);s.captureDurationMs=o.value("captureDurationMs").toInt(10000);s.activateBeforeCapture=o.value("activateBeforeCapture").toBool(true);
        if (!s.referenceVideo.isEmpty() && QFileInfo(s.referenceVideo).isRelative()) s.referenceVideo = QFileInfo(path).dir().absoluteFilePath(s.referenceVideo);
        s.region = QRect(o.value("x").toInt(), o.value("y").toInt(), o.value("width").toInt(), o.value("height").toInt());
        loaded.steps.append(s);
    }
    if (!validateFlowItems(loaded, error)) return false;
    flow = loaded; return true;
}
bool validateFlowItems(const TestFlow &flow, QString &error)
{
    error.clear(); QSet<QString> ids, finished;
    for (const auto &item : flow.testItems) {
        if (item.id.trimmed().isEmpty() || item.name.trimmed().isEmpty() || ids.contains(item.id)) {
            error = "測試項目須有唯一 ID 與非空白名稱"; return false;
        }
        ids.insert(item.id);
    }
    QString previous;
    for (int i=0; i<flow.steps.size(); ++i) {
        const auto id=flow.steps[i].testItemId;
        if (!id.isEmpty() && !ids.contains(id)) {error=QString("步驟 %1 指向不存在的測試項目").arg(i+1);return false;}
        if (id!=previous) {
            if (!previous.isEmpty()) finished.insert(previous);
            if (!id.isEmpty() && finished.contains(id)) {error="同一測試項目的步驟必須連續；請調整項目範圍或步驟順序。";return false;}
            previous=id;
        }
    }
    return true;
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
    if (!validateFlowItems(flow, error)) return false;
    if (!QFileInfo(flow.executable).isFile() || !QFileInfo(flow.executable).isAbsolute()) {error = "請選擇存在的待測 EXE 絕對路徑"; return false;}
    if (!flow.workingDirectory.isEmpty() && !QFileInfo(flow.workingDirectory).isDir()) {error = "工作目錄不存在"; return false;}
    if (flow.steps.isEmpty()) {error = "請新增測試步驟"; return false;}
    if (singleStep < -1 || singleStep >= flow.steps.size()) {error = "請選取有效的測試步驟"; return false;}
    const int first = singleStep < 0 ? 0 : singleStep;
    const int end = singleStep < 0 ? static_cast<int>(flow.steps.size()) : singleStep + 1;
    for (int i = first; i < end; ++i) {
        const auto &s = flow.steps[i]; QString reason;
        if (!flowTypes().contains(s.type)) reason = "不支援的步驟";
        else if ((s.type=="checkVideo" || s.type=="checkImage" || s.type=="readOCR") && (s.captureDurationMs<100 || s.captureDurationMs>600000 || s.sampleFps<1 || s.sampleFps>120 || (s.captureMode!="window" && s.captureMode!="desktop"))) reason="擷取方式或採樣頻率無效（1～120 FPS）";
        else if (s.timeoutMs < 1 || s.timeoutMs > 600000) reason = "時間必須介於 1～600000 毫秒";
        else if ((s.type == "clickUIA" || s.type == "readUIA") && s.selector.isEmpty() && s.automationId.isEmpty()) reason = "請指定控制項名稱或 AutomationId";
        else if (s.selector.startsWith("regex:") && !QRegularExpression(s.selector.mid(6)).isValid()) reason = "UIA 名稱正規式無效";
        else if ((s.type == "readUIA" || s.type == "readOCR") && !s.text.isEmpty()
                 && (!QRegularExpression(s.text).isValid() || QRegularExpression(s.text).captureCount() < 1)) reason = "擷取正規式必須有效且包含括號群組";
        else if (s.type == "readOCR" && detectOcrExecutable(flow.ocrExecutable).isEmpty()) reason = "找不到文字辨識引擎，請保留套件內的 ocr 資料夾";
        else if ((s.type == "readOCR" || s.type == "checkImage" || s.type == "checkVideo") && (s.region.x() < 0 || s.region.y() < 0 || s.region.width() < 1 || s.region.height() < 1)) reason = "請框選有效 OCR／影像區域";
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
        if (s.type == "checkVideo" && (!QFileInfo(s.referenceVideo).isFile() || s.captureDurationMs < 3500)) {error = "參考影片不存在，或擷取時間少於 3500 毫秒"; return false;}
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
    const QString sample="CH0 | Running | 1920x1080\nCH0 Video | received=124 frames\nCH0 Audio | 48000 Hz 2 ch 16-bit | received=208 frames";
    FlowStep video;video.type="readUIA";video.name="確認「整段狀態文字」顯示文字";video.condition="gt";video.expected="0";
    FlowStep audio=video;
    for(const auto &field:flowNumberFields(sample)) {
        if(field.name.contains("CH0 Video") && field.name.contains("received"))video.text=field.pattern;
        if(field.name.contains("CH0 Audio") && field.name.contains("received"))audio.text=field.pattern;
    }
    if(video.text.isEmpty() || audio.text.isEmpty() || !flowStepDisplayName(video).contains("CH0 Video")
        || !flowStepDisplayName(audio).contains("CH0 Audio") || !flowStepDisplayName(audio).contains("> 0")
        || flowStepDisplayName(audio)==flowStepDisplayName(video))return false;
    video.name="自訂影像檢查";if(flowStepDisplayName(video)!=video.name)return false;
    QString value, error;
    if (!evaluateFlowValue("received=64 frames", "gt", "0", value, error) || value != "64") return false;
    if (evaluateFlowValue("no signal", "gt", "0", value, error) || error.isEmpty()) return false;
    if (!evaluateFlowValue("59.9 FPS", "range", "59,61", value, error)) return false;
    if (evaluateFlowValue("Connected", "equals", "Disconnected", value, error)) return false;
    QTemporaryDir dir; TestFlow source, loaded;
    source.executable = "C:/測試 APP/target.exe";
    source.reuseExisting = false;
    FlowStep step; step.type = "readOCR"; step.region = QRect(10, 20, 120, 40); step.name = "讀取狀態"; step.windowTitle = "預覽視窗"; step.activateBeforeCapture=false; source.steps.append(step);
    if (!saveFlow(dir.path() + "/flow.json", source, error) || !loadFlow(dir.path() + "/flow.json", loaded, error)) return false;
    if(loaded.steps[0].activateBeforeCapture)return false;
    source.steps[0].activateBeforeCapture=true;
    if(!saveFlow(dir.path()+"/activation.json",source,error) || !loadFlow(dir.path()+"/activation.json",loaded,error)
       || !loaded.steps[0].activateBeforeCapture)return false;
    if(!FlowStep().activateBeforeCapture)return false;
    QFile legacyInput(dir.path()+"/activation.json");if(!legacyInput.open(QIODevice::ReadOnly))return false;
    auto legacyRoot=QJsonDocument::fromJson(legacyInput.readAll()).object();legacyInput.close();
    auto legacySteps=legacyRoot["steps"].toArray();auto legacyStep=legacySteps[0].toObject();legacyStep.remove("activateBeforeCapture");legacySteps[0]=legacyStep;legacyRoot["steps"]=legacySteps;
    QFile legacyOutput(dir.path()+"/legacy.json");if(!legacyOutput.open(QIODevice::WriteOnly))return false;
    legacyOutput.write(QJsonDocument(legacyRoot).toJson());legacyOutput.close();TestFlow legacyActivation;
    if(!loadFlow(dir.path()+"/legacy.json",legacyActivation,error) || !legacyActivation.steps[0].activateBeforeCapture)return false;
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
    TestFlow grouped = source; grouped.name = "通用 APP 出廠測試"; grouped.version = "1.2";
    grouped.testItems = {{"audio", "確認音訊接收", "請保持連接\n不需操作", "正在等待資料", "檢查訊號源"}};
    grouped.steps[0].testItemId = "audio";
    TestFlow roundTrip;
    if (!saveFlow(dir.path() + "/grouped.json", grouped, error) || !loadFlow(dir.path() + "/grouped.json", roundTrip, error)
        || roundTrip.name != grouped.name || roundTrip.version != grouped.version || roundTrip.testItems.size()!=1
        || roundTrip.testItems[0].instruction != grouped.testItems[0].instruction
        || roundTrip.testItems[0].failureHint != "檢查訊號源" || roundTrip.steps[0].testItemId != "audio") return false;
    roundTrip.steps[0].testItemId = "missing";
    if (validateFlowItems(roundTrip, error)) return false;
    FlowStep ungrouped = grouped.steps[0]; ungrouped.testItemId.clear();
    grouped.steps = {grouped.steps[0], ungrouped, grouped.steps[0]};
    if (validateFlowItems(grouped, error)) return false;
    QFile legacy(dir.path() + "/legacy.json");
    if (!legacy.open(QIODevice::WriteOnly)) return false;
    legacy.write("{\"schemaVersion\":1,\"steps\":[{\"type\":\"delay\",\"name\":\"legacy\",\"timeoutMs\":50}]}");legacy.close();
    TestFlow old;
    if (!loadFlow(legacy.fileName(), old, error) || !old.testItems.isEmpty() || !old.steps[0].testItemId.isEmpty()) return false;
    return !loaded.reuseExisting && TestFlow().reuseExisting && loaded.executable == source.executable && loaded.steps.size() == 1 && loaded.steps[0].region == step.region && loaded.steps[0].windowTitle == step.windowTitle;
}
