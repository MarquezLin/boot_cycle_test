#include "easy_builder.h"
#include "ui_easy_builder.h"
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>

EasyBuilder::EasyBuilder(QWidget *parent, bool insertion) : QDialog(parent), ui(std::make_unique<Ui::EasyBuilder>())
{
    ui->setupUi(this);
    ui->textConditionCombo->addItem("等於（忽略大小寫與空白）", "equalsIgnoreCase");
    ui->textConditionCombo->addItem("包含（忽略大小寫）", "containsIgnoreCase");
    ui->textConditionCombo->addItem("完全相同（區分大小寫與空白）", "equals");
    ui->textConditionCombo->addItem("包含（區分大小寫）", "contains");
    if (insertion) {ui->appendCheck->hide(); setWindowTitle("插入測試項目（保留原有步驟）");}
    if (const auto screen = this->screen()) resize(size().boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText(insertion ? "插入這些測試項目" : "確認並建立流程");
    ui->buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    for (auto *check : {ui->runningCheck, ui->resolutionCheck, ui->fpsCheck, ui->contentCheck, ui->motionCheck, ui->audioCheck, ui->videoCheck, ui->startCheck, ui->textCompareCheck, ui->delayCheck})
        connect(check, &QCheckBox::toggled, this, &EasyBuilder::refresh);
    for (auto *spin : {ui->widthSpin, ui->heightSpin}) connect(spin, &QSpinBox::valueChanged, this, &EasyBuilder::refresh);
    for (auto *spin : {ui->fpsSpin, ui->toleranceSpin, ui->delaySpin}) connect(spin, &QDoubleSpinBox::valueChanged, this, &EasyBuilder::refresh);
    connect(ui->expectedTextEdit, &QLineEdit::textChanged, this, &EasyBuilder::refresh);
    connect(ui->textConditionCombo, &QComboBox::currentIndexChanged, this, &EasyBuilder::refresh);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] {
        const auto p = plan();
        if (easyPlanSteps(p).isEmpty()) {
            QMessageBox::warning(this, "請確認項目", "請至少勾選一個操作或檢查項目。"); return;
        }
        if (p.fps && p.tolerance > p.targetFps) {QMessageBox::warning(this, "請確認項目", "允許誤差不可超過目標 FPS。"); return;}
        if (p.textCompare && p.expectedText.trimmed().isEmpty()) {QMessageBox::warning(this, "請填入預期文字", "請填入要確認的文字，例如 Stopped 或 No Signal。"); ui->expectedTextEdit->setFocus(); return;}
        accept();
    });
    refresh();
}
EasyBuilder::~EasyBuilder() = default;
EasyPlan EasyBuilder::plan() const
{
    EasyPlan p; p.running = ui->runningCheck->isChecked(); p.resolution = ui->resolutionCheck->isChecked(); p.fps = ui->fpsCheck->isChecked();
    p.content = ui->contentCheck->isChecked(); p.motion = ui->motionCheck->isChecked(); p.audio = ui->audioCheck->isChecked(); p.video = ui->videoCheck->isChecked();
    p.clickStart = ui->startCheck->isChecked(); p.width = ui->widthSpin->value(); p.height = ui->heightSpin->value();
    p.delay = ui->delayCheck->isChecked(); p.delayMs = qRound(ui->delaySpin->value() * 1000);
    p.targetFps = ui->fpsSpin->value(); p.tolerance = ui->toleranceSpin->value();
    p.textCompare = ui->textCompareCheck->isChecked(); p.expectedText = ui->expectedTextEdit->text(); p.textCondition = ui->textConditionCombo->currentData().toString();
    return p;
}
bool EasyBuilder::append() const {return ui->appendCheck->isChecked();}
void EasyBuilder::refresh()
{
    const auto p = plan();
    ui->expectedTextEdit->setEnabled(p.textCompare); ui->textConditionCombo->setEnabled(p.textCompare); ui->textConditionLabel->setEnabled(p.textCompare);
    ui->previewEdit->setPlainText(easyPlanSteps(p).isEmpty() ? "尚未選擇測試項目，請自行勾選。" : describeEasyPlan(p));
    ui->widthSpin->setEnabled(p.resolution); ui->heightSpin->setEnabled(p.resolution);
    ui->fpsSpin->setEnabled(p.fps); ui->toleranceSpin->setEnabled(p.fps);
    ui->delaySpin->setEnabled(p.delay);
}
