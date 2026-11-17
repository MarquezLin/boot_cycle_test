#pragma once
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

struct FlowStep {
    QString type = "waitWindow";
    QString name, selector, automationId, text, condition = "contains", expected, windowTitle;
    int timeoutMs = 10000;
    QRect region;
    QString testItemId;
    QString valueLabel;
    QString referenceVideo;
    QString captureMode = "window";
    int sampleFps = 30;
    int captureDurationMs = 10000;
    int ignoreVideoBottomPercent = 0;
    bool activateBeforeCapture = true;
    int targetStep = 1;
    int jumpCount = 1;
};
struct FlowTestItem {
    QString id, name, instruction, runningText, failureHint;
};
struct TestFlow {
    QString executable, arguments, workingDirectory, ocrExecutable, ocrLanguage = "eng";
    bool reuseExisting = true;
    QVector<FlowStep> steps;
    QString name, version;
    QVector<FlowTestItem> testItems;
    int formalCycleLimit = -1; // Legacy JSON without a limit uses the local setting.
};
bool validateFlowItems(const TestFlow &flow, QString &error);
bool flowHasStop(const TestFlow &flow);
bool saveFlow(const QString &path, const TestFlow &flow, QString &error);
bool loadFlow(const QString &path, TestFlow &flow, QString &error);
bool saveFlowCycleLimit(const QString &path, int limit, QString &error);
bool validateFlow(const TestFlow &flow, QString &error, int singleStep = -1, bool hasRead = false);
bool evaluateFlowValue(const QString &raw, const QString &condition, const QString &expected,
                       QString &parsed, QString &error);
QString flowTypeName(const QString &type);
QStringList flowTypes();
struct FlowNumberField {QString label, pattern, value, name;};
QVector<FlowNumberField> flowNumberFields(const QString &raw);
QString flowCheckSummary(const FlowStep &step);
QString flowStepDisplayName(const FlowStep &step);
QString flowItemDisplayName(const TestFlow &flow, const FlowTestItem &item);
bool flowAutoName(const QString &name);
bool flowModelSelfTest();
