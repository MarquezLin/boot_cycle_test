#pragma once
int regionPreview();
#include "flow_model.h"
#include <QDialog>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
namespace Ui { class FlowDialog; }
class FlowRunner;
class QPlainTextEdit;
class FlowDialog final : public QDialog {
    Q_OBJECT
public:
    explicit FlowDialog(const QString &target, QWidget *parent = nullptr, const QString &file = {});
    ~FlowDialog() override;
    bool insertionSelfTest();
    bool runSelfTest();
protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
private:
    void readEditor();
    void showEditor(int row);
    void refreshFields();
    void refreshWindows();
    void refreshBinding();
    void setAdvanced(bool advanced);
    void updateTable(bool preserveResults = false);
    void markStepForRetest(int row);
    void insertSteps(int index, const QVector<FlowStep> &steps);
    void insertScanDraft(int index, const TestFlow &draft);
    void buildInsertion(bool after);
    void removeSelectedStep();
    void moveSelectedStep(int offset);
    void runFlow(bool single);
    void previewNumber();
    void selectNumber(const QString &raw, int row);
    void setBusy(bool busy);
    void sample();
    void log(const QString &line);
    void setReadResult(int row,const QString &detail);
    void refreshReadDetails();
    TestFlow currentFlow() const;
    void applyFlow(const TestFlow &flow);
    bool applyItems(const TestFlow &flow, QString &error);
    std::unique_ptr<Ui::FlowDialog> ui;
    QVector<FlowStep> m_steps;
    QVector<FlowTestItem> m_testItems;
    QString m_flowName, m_flowVersion;
    FlowRunner *m_runner = nullptr;
    QPlainTextEdit *m_readDetails = nullptr;
    QString m_file, m_reportDirectory, m_lastValue, m_replayCaptureFile;
    bool m_loading = false, m_busy = false, m_picking = false;
    QTimer m_waitTimer;
    QElapsedTimer m_stepElapsed;
    int m_activeStep = -1;
};
