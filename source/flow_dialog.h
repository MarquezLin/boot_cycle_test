#pragma once
int regionPreview();
#include "flow_model.h"
#include <QDialog>
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
#include <functional>
namespace Ui { class FlowDialog; }
class FlowRunner;
class FlowDialog final : public QDialog {
    Q_OBJECT
public:
    explicit FlowDialog(const QString &target, QWidget *parent = nullptr);
    ~FlowDialog() override;
    bool insertionSelfTest();
    void setBootImporter(std::function<bool(const TestFlow &)> importer);
    void setEditingCopy(const TestFlow &flow, const QString &sourceName = {});
    QString sourceFileName() const;
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
    void buildInsertion(bool after);
    void removeSelectedStep();
    void moveSelectedStep(int offset);
    void runFlow(bool single);
    void setBusy(bool busy);
    void sample();
    void log(const QString &line);
    TestFlow currentFlow() const;
    void applyFlow(const TestFlow &flow);
    std::unique_ptr<Ui::FlowDialog> ui;
    QVector<FlowStep> m_steps;
    FlowRunner *m_runner = nullptr;
    QString m_file, m_reportDirectory, m_lastValue;
    QString m_importName;
    bool m_loading = false, m_busy = false, m_picking = false;
    QTimer m_waitTimer;
    QElapsedTimer m_stepElapsed;
    int m_activeStep = -1;
    std::function<bool(const TestFlow &)> m_bootImporter;
};
