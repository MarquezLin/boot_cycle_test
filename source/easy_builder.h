#pragma once
#include "easy_plan.h"
#include <QDialog>
#include <memory>
namespace Ui {class EasyBuilder;}
class EasyBuilder final : public QDialog {
    Q_OBJECT
public:
    explicit EasyBuilder(QWidget *parent = nullptr, bool insertion = false,
        const QStringList &loopTargets = {}, const FlowStep *editLoop = nullptr);
    ~EasyBuilder() override;
    EasyPlan plan() const;
    bool append() const;
private:
    void refresh();
    std::unique_ptr<Ui::EasyBuilder> ui;
    class QCheckBox *m_referenceCheck;
    class QLineEdit *m_referencePath;
};
