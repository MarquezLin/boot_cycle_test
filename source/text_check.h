#pragma once
#include "flow_model.h"
#include <QDialog>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
namespace Ui {class TextCheck;}
class TextCheck final : public QDialog {
    Q_OBJECT
public:
    TextCheck(const QImage &crop, const FlowStep &step, const QString &engine, const QString &language, QWidget *parent = nullptr);
    ~TextCheck() override;
    FlowStep resultStep() const;
private:
    void recognize();
    void evaluate();
    void refresh();
    std::unique_ptr<Ui::TextCheck> ui;
    FlowStep m_step;
    QString m_engine, m_language;
    QTemporaryDir m_temp;
    QProcess m_process;
    QTimer m_timeout;
    bool m_read = false;
};
