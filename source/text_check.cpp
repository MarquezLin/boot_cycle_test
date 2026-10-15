#include "text_check.h"
#include "ocr_reader.h"
#include "ui_text_check.h"
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
TextCheck::TextCheck(const QImage &crop, const FlowStep &step, const QString &engine, const QString &language, QWidget *parent)
    : QDialog(parent), ui(std::make_unique<Ui::TextCheck>()), m_step(step), m_engine(engine), m_language(language)
{
    ui->setupUi(this);
    ui->imageLabel->setPixmap(QPixmap::fromImage(crop).scaled(680, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    prepareOcrImage(crop).save(m_temp.path() + "/crop.png");
    ui->presetCheck->setText("沿用既定條件：" + step.name);
    ui->presetCheck->setEnabled(!step.expected.isEmpty()); ui->presetCheck->setChecked(!step.expected.isEmpty());
    if (step.condition == "equalsIgnoreCase" || step.condition == "equals" || step.condition == "containsIgnoreCase" || step.condition == "contains") ui->expectedEdit->setText(step.expected);
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText("保存這個文字檢查"); ui->buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    connect(ui->presetCheck, &QCheckBox::toggled, this, &TextCheck::refresh); connect(ui->kindCombo, &QComboBox::currentIndexChanged, this, &TextCheck::refresh);
    connect(ui->readButton, &QPushButton::clicked, this, &TextCheck::recognize); connect(ui->checkButton, &QPushButton::clicked, this, &TextCheck::evaluate);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] {
        const auto step = resultStep();
        if (step.expected.isEmpty() || (ui->kindCombo->currentIndex() == 3 && !ui->presetCheck->isChecked() && ui->minimumSpin->value() > ui->maximumSpin->value())) {
            QMessageBox::warning(this, "請設定判定條件", "請填入預期文字，或設定有效的數值範圍。"); return;
        }
        accept();
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_timeout.stop();
        ui->readButton->setEnabled(true);
        const auto text = QString::fromUtf8(m_process.readAllStandardOutput()).trimmed();
        m_read = status == QProcess::NormalExit && code == 0 && !text.isEmpty();
        ui->recognizedEdit->setPlainText(text);
        if (!m_read) ui->resultLabel->setText("無法讀到文字，請重新框選或檢查辨識元件：" + QString::fromUtf8(m_process.readAllStandardError()));
        else evaluate();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {m_timeout.stop(); ui->readButton->setEnabled(true); ui->resultLabel->setText("無法啟動文字辨識元件：" + m_process.errorString());}
    });
    m_timeout.setSingleShot(true); connect(&m_timeout, &QTimer::timeout, &m_process, &QProcess::kill);
    refresh(); QTimer::singleShot(0, this, &TextCheck::recognize);
}
TextCheck::~TextCheck() {if (m_process.state() != QProcess::NotRunning) {m_process.kill(); m_process.waitForFinished(1000);}}
FlowStep TextCheck::resultStep() const
{
    auto step = m_step; step.type = "readOCR"; step.selector.clear(); step.automationId.clear();
    if (ui->presetCheck->isChecked()) return step;
    step.text.clear();
    switch (ui->kindCombo->currentIndex()) {
    case 0: step.condition = "equalsIgnoreCase"; step.expected = ui->expectedEdit->text().trimmed(); break;
    case 1: step.condition = "containsIgnoreCase"; step.expected = ui->expectedEdit->text().trimmed(); break;
    case 2: step.condition = "gt"; step.expected = QString::number(ui->minimumSpin->value()); break;
    default: step.condition = "range"; step.expected = QString::number(ui->minimumSpin->value()) + ',' + QString::number(ui->maximumSpin->value()); break;
    }
    step.name = "文字檢查：" + ui->kindCombo->currentText() + " " + step.expected; return step;
}
void TextCheck::refresh()
{
    ui->customWidget->setEnabled(!ui->presetCheck->isChecked()); const bool numeric = ui->kindCombo->currentIndex() >= 2;
    ui->expectedEdit->setVisible(!numeric); ui->expectedLabel->setVisible(!numeric); ui->numberLabel->setVisible(numeric);
    ui->minimumSpin->setVisible(numeric); ui->maximumSpin->setVisible(ui->kindCombo->currentIndex() == 3);
}
void TextCheck::recognize()
{
    if (m_process.state() != QProcess::NotRunning) return;
    m_read = false; ui->readButton->setEnabled(false); ui->resultLabel->setText("正在讀取框內文字…");
    m_process.start(m_engine, ocrArguments(m_engine, m_temp.path() + "/crop.png", m_language));
    m_timeout.start(15000);
}
void TextCheck::evaluate()
{
    if (!m_read) {ui->resultLabel->setText("請先讀取文字；空白或讀取失敗不會算 PASS。"); return;}
    auto step = resultStep(); QString value = ui->recognizedEdit->toPlainText().trimmed(), parsed, error;
    if (!step.text.isEmpty()) {
        const auto match = QRegularExpression(step.text).match(value);
        if (!match.hasMatch() || match.lastCapturedIndex() < 1) {ui->resultLabel->setText("FAIL — 這次讀到的文字不含步驟要求的數值。"); return;}
        value = match.captured(1);
    }
    const bool pass = evaluateFlowValue(value, step.condition, step.expected, parsed, error);
    ui->resultLabel->setText((pass ? "PASS — " : "FAIL — ") + (error.isEmpty() ? "這次讀到的文字" + QString(pass ? "符合" : "不符合") + "預期條件。" : error));
}
