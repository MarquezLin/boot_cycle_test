#pragma once
#include "easy_plan.h"
#include <QDialog>
#include <memory>
namespace Ui {class EasyBuilder;}
class EasyBuilder final : public QDialog {
    Q_OBJECT
public:
    explicit EasyBuilder(QWidget *parent = nullptr, bool insertion = false);
    ~EasyBuilder() override;
    EasyPlan plan() const;
    bool append() const;
private:
    void refresh();
    std::unique_ptr<Ui::EasyBuilder> ui;
};
