#include "flow_samples.h"
#include <QRegularExpression>

QStringList flowSampleNames()
{
    return {"CH0 audio received > 0", "是否進入 Running", "解析度是否為 1920 × 1080", "FPS 是否約 60（59～61）",
            "Running + 1920 × 1080 + 約 60 FPS", "Video received frames > 0", "預覽區有畫面內容（含黑白切換）", "預覽區持續更新（畫面變化）"};
}
QString flowSampleHint(int index)
{
    if (index == 6) return "請只框選影像預覽內部，排除工具列／文字／邊框。亮度標準差或前後截圖平均 RGB 差異 ≥ 8 即通過，支援黑白切換；固定純色無法藉此確認有影像。";
    if (index == 7) return "請框選預覽內部；以不同間隔（至少 500 ms）比較截圖，平均 RGB 差異 ≥ 2 即 PASS，支援純黑／純白互換；靜態畫面不通過，這不是實際 FPS 測量。";
    if (index == 3 || index == 4) return "FPS 預設允許 59～61（包含 59.94），可改預期值範圍。檢查的是 APP 顯示值，不是由截圖測量實際 FPS。UIA 請取控制項；OCR 請分別框選。";
    if (index == 5) return "讀取 APP 的 Video received frame 數並要求 > 0；請選正確的 video 計數器，不要選 audio 或設定欄位。這不等於外接螢幕輸出檢查。";
    if (index == 1) return "讀取狀態並要求 Running；Not Running、Running=false 不會通過。若 APP 使用中文狀態或不同格式，請調整預期正規式。";
    if (index == 2) return "讀取 APP 實際訊號解析度，而非設定選單或視窗大小；接受 1920x1080、1920*1080、1920×1080 與中間空白。";
    return "載入後先設定待測 EXE，再取 UIA 控制項或框選 OCR 區域，最後單步試跑。不要把顯示值誤認為實際硬體驗證。";
}
QVector<FlowStep> flowSampleSteps(int index, bool ocr)
{
    QVector<FlowStep> steps;
    auto read = [&](const QString &name, const QString &extraction, const QString &condition, const QString &expected) {
        FlowStep step; step.type = ocr ? "readOCR" : "readUIA"; step.name = name;
        // Each application exposes different UIA controls. Require the user to pick one.
        step.selector.clear(); step.text = extraction; step.condition = condition; step.expected = expected; step.timeoutMs = 30000;
        steps.append(step);
    };
    auto running = [&] {read("確認狀態 Running（請選狀態控制項／框選狀態）", {}, "regex", R"((?i)^\s*(?:(?:status|state)\s*[:=]\s*)?running\s*$)");};
    auto resolution = [&] {read("確認訊號解析度 1920 × 1080（請選解析度）", {}, "regex", R"((?<!\d)1920\s*[xX×*]\s*1080(?!\d))");};
    auto fps = [&] {read("確認 FPS 59～61（請只選 FPS 值）", R"((?i)(?:\bfps\s*[:=]?\s*|^\s*)([0-9]+(?:\.[0-9]+)?)(?=\s*(?:fps\b|$)))", "range", "59,61");};
    switch (index) {
    case 0:
        read("確認 CH0 audio received > 0（請選 audio 狀態）", R"((?i)received\s*[:=]\s*([0-9]+))", "gt", "0"); break;
    case 1: running(); break;
    case 2: resolution(); break;
    case 3: fps(); break;
    case 4: running(); resolution(); fps(); break;
    case 5:
        read("確認 video received frames > 0（請選 video 計數）", R"((?i)(?:received|frames)\s*[:=]\s*([0-9]+))", "gt", "0"); break;
    case 6: case 7: {
        FlowStep step; step.type = "checkImage"; step.name = index == 6 ? "預覽區有畫面內容（請框選預覽內部）" : "預覽區畫面有變化（請框選預覽內部）";
        step.condition = index == 6 ? "contrast" : "change"; step.expected = index == 6 ? "8" : "2"; step.timeoutMs = 10000;
        steps.append(step); break;
    }
    default: break;
    }
    return steps;
}
bool flowSamplesSelfTest()
{
    QString parsed, error;
    const auto running = flowSampleSteps(1, false).first();
    for (const auto &bad : QStringList{"Not Running", "Running=false", "Running: false", "Stopped", "Idle"})
        if (evaluateFlowValue(bad, running.condition, running.expected, parsed, error)) return false;
    if (!evaluateFlowValue("Status: Running", running.condition, running.expected, parsed, error)) return false;
    const auto resolution = flowSampleSteps(2, false).first();
    for (const auto &good : QStringList{"1920x1080", "1920 * 1080", QString::fromUtf8("1920×1080")})
        if (!evaluateFlowValue(good, resolution.condition, resolution.expected, parsed, error)) return false;
    if (evaluateFlowValue("1920x10800", resolution.condition, resolution.expected, parsed, error)) return false;
    const auto fps = flowSampleSteps(3, false).first();
    if (QRegularExpression(fps.text).match("CH0 FPS: 59.94").captured(1) != "59.94") return false;
    if (!evaluateFlowValue("59.94 FPS", fps.condition, fps.expected, parsed, error)
        || evaluateFlowValue("30 FPS", fps.condition, fps.expected, parsed, error)) return false;
    for (int index = 0; index < flowSampleNames().size(); ++index) {
        const auto uia = flowSampleSteps(index, false), ocr = flowSampleSteps(index, true);
        if (uia.isEmpty() || uia.size() != ocr.size()) return false;
        for (const auto &step : uia) if (!step.text.isEmpty() && !QRegularExpression(step.text).isValid()) return false;
    }
    return flowSampleSteps(4, true).size() == 3;
}
