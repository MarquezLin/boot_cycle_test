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
};
struct TestFlow {
    QString executable, arguments, workingDirectory, ocrExecutable, ocrLanguage = "eng";
    bool reuseExisting = true;
    QVector<FlowStep> steps;
};
bool saveFlow(const QString &path, const TestFlow &flow, QString &error);
bool loadFlow(const QString &path, TestFlow &flow, QString &error);
bool validateFlow(const TestFlow &flow, QString &error, int singleStep = -1, bool hasRead = false);
bool evaluateFlowValue(const QString &raw, const QString &condition, const QString &expected,
                       QString &parsed, QString &error);
QString flowTypeName(const QString &type);
QStringList flowTypes();
bool flowModelSelfTest();
