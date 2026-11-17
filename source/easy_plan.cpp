#include "easy_plan.h"
#include "flow_samples.h"
#include <QRegularExpression>

#include <cmath>
QVector<FlowStep> easyPlanSteps(const EasyPlan &p)
{
    QVector<FlowStep> steps;
    if (p.clickStart) {FlowStep click; click.type = "clickUIA"; click.name = "點擊按鈕（請指一下目標按鈕）"; steps.append(click);}
    if (p.delay) {
        FlowStep wait; wait.type = "delay"; wait.timeoutMs = p.delayMs;
        wait.name = QString("等待 %1 秒").arg(QString::number(p.delayMs / 1000.0)); steps.append(wait);
    }
    auto add = [&](int index) {steps += flowSampleSteps(index, false);};
    if (p.running) {add(1); steps.last().name = "確認進入 Running";}
    if (p.resolution) {
        auto step = flowSampleSteps(2, false).first(); step.name = QString("確認解析度 %1 × %2").arg(p.width).arg(p.height);
        step.expected = QString(R"((?<!\d)%1\s*[xX×*]\s*%2(?!\d))").arg(p.width).arg(p.height); steps.append(step);
    }
    if (p.fps) {
        auto step = flowSampleSteps(3, false).first(); step.expected = QString::number(qMax(0.0, p.targetFps - p.tolerance)) + ',' + QString::number(p.targetFps + p.tolerance);
        step.name = QString("確認 FPS %1 ± %2").arg(p.targetFps).arg(p.tolerance); steps.append(step);
    }
    if (p.audio) {add(0); steps.last().name = "確認收到音訊資料";}
    if (p.video) {add(5); steps.last().name = "確認收到影像資料";}
    if (p.content) {add(6); steps.last().name = "確認預覽區有畫面";}
    if (p.motion) {add(7); steps.last().name = "確認預覽畫面會變動";}
    if (p.textCompare) {
        FlowStep step; step.type = p.textOcr ? "readOCR" : "readUIA"; step.condition = p.textCondition; step.expected = p.expectedText; step.timeoutMs = 30000;
        step.name = QString(p.textOcr ? "框選 OCR 文字比對（" : "比對字串（") + QString(p.textCondition.startsWith("contains") ? "包含" : "等於") + "）："
            + (p.expectedText.isEmpty() ? "請填入預期文字" : p.expectedText);
        steps.append(step);
    }
    if(p.referenceCompare) {FlowStep step;step.type="checkVideo";step.name="確認影片畫面與顏色正常";step.referenceVideo=p.referenceVideo;step.timeoutMs=10000;steps.append(step);}
    if (p.loopTargetStep > 0) {
        FlowStep step;
        step.type = "loop";
        step.targetStep = p.loopTargetStep;
        step.jumpCount = p.loopJumpCount;
        steps.append(step);
    }
    if (p.stop) {
        FlowStep step;
        step.type = "stop";
        steps.append(step);
    }
    return steps;
}
QString describeEasyPlan(const EasyPlan &p)
{
    QStringList lines;
    for (const auto &step : easyPlanSteps(p)) lines << QString::number(lines.size() + 1) + ". " + flowStepDisplayName(step);
    return lines.join('\n') + "\n\n等待步驟不需指定目標；畫面與 OCR 項目加入後，請選取步驟並框選區域。參考影片比較：訊號來源須正常速度播放同一短片；本機只讀取參考檔，不必另開播放器。框選完整影片內容並排除黑邊，連續觀察至少 3 秒；靜止或循環切換時可能無法對齊。不會立即執行。";
}
bool easyPlanSelfTest()
{
    QString error;
    if (!easyPlanSteps(EasyPlan()).isEmpty()) return false;
    EasyPlan literal; literal.textCompare = true; literal.expectedText = "Stopped [CH0].*"; literal.textCondition = "equals";
    const auto literalSteps = easyPlanSteps(literal);
    if (literalSteps.size() != 1 || literalSteps.first().type != "readUIA" || !literalSteps.first().text.isEmpty()) return false;
    const auto &literalStep = literalSteps.first(); QString parsed;
    if (!evaluateFlowValue("Stopped [CH0].*", literalStep.condition, literalStep.expected, parsed, error)
        || evaluateFlowValue("Stopped CH0 anything", literalStep.condition, literalStep.expected, parsed, error)) return false;
    literal.textCondition = "containsIgnoreCase"; literal.expectedText = "No Signal";
    const auto contains = easyPlanSteps(literal).first();
    return evaluateFlowValue("CH0: NO SIGNAL", contains.condition, contains.expected, parsed, error)
        && !evaluateFlowValue("CH0: Running", contains.condition, contains.expected, parsed, error);
}
