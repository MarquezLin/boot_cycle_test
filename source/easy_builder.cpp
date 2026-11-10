#include "easy_builder.h"
#include "ui_easy_builder.h"
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QPushButton>
#include <QScreen>

EasyBuilder::EasyBuilder(QWidget *parent, bool insertion) : QDialog(parent), ui(std::make_unique<Ui::EasyBuilder>())
{
    ui->setupUi(this);
    m_referenceCheck=new QCheckBox("參考影片：檢查畫面與顏色",this);m_referenceCheck->setObjectName("referenceVideoCheck");
    m_referencePath=new QLineEdit(this);m_referencePath->setPlaceholderText("選擇同一支本機測試短片（120 秒內）");
    auto *browse=new QPushButton("選擇影片…",this);
    ui->checksLayout->addWidget(m_referenceCheck,4,0);ui->checksLayout->addWidget(m_referencePath,4,1,1,2);ui->checksLayout->addWidget(browse,4,3);
    connect(browse,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"選擇參考影片",{},"影片 (*.mp4 *.avi *.mov *.mkv *.wmv);;所有檔案 (*)");if(!path.isEmpty()){m_referencePath->setText(path);m_referenceCheck->setChecked(true);}});
    connect(m_referenceCheck,&QCheckBox::toggled,this,&EasyBuilder::refresh);connect(m_referencePath,&QLineEdit::textChanged,this,&EasyBuilder::refresh);

    ui->textConditionCombo->addItem("等於（忽略大小寫與空白）", "equalsIgnoreCase");
    ui->textConditionCombo->addItem("包含（忽略大小寫）", "containsIgnoreCase");
    ui->textConditionCombo->addItem("完全相同（區分大小寫與空白）", "equals");
    ui->textConditionCombo->addItem("包含（區分大小寫）", "contains");
    if (insertion) {setWindowTitle("插入補充步驟（保留原有步驟）");}
    if (const auto screen = this->screen()) resize(size().boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    ui->buttonBox->button(QDialogButtonBox::Ok)->setText(insertion ? "插入補充步驟" : "加入補充步驟");
    ui->buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    for (auto *check : {ui->contentCheck, ui->motionCheck, ui->textCompareCheck, ui->delayCheck})
        connect(check, &QCheckBox::toggled, this, &EasyBuilder::refresh);
    for (auto *spin : {ui->delaySpin}) connect(spin, &QDoubleSpinBox::valueChanged, this, &EasyBuilder::refresh);
    connect(ui->expectedTextEdit, &QLineEdit::textChanged, this, &EasyBuilder::refresh);
    connect(ui->textConditionCombo, &QComboBox::currentIndexChanged, this, &EasyBuilder::refresh);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, [this] {
        const auto p = plan();
        if (easyPlanSteps(p).isEmpty()) {
            QMessageBox::warning(this, "請確認項目", "請至少勾選一個操作或檢查項目。"); return;
        }
        if(p.referenceCompare && !QFileInfo(p.referenceVideo).isFile()){QMessageBox::warning(this,"請選擇影片","請選擇存在的本機參考影片。");return;}
        if (p.textCompare && p.expectedText.trimmed().isEmpty()) {QMessageBox::warning(this, "請填入預期文字", "請填入要確認的文字，例如 Stopped 或 No Signal。"); ui->expectedTextEdit->setFocus(); return;}
        accept();
    });
    refresh();
}
EasyBuilder::~EasyBuilder() = default;
EasyPlan EasyBuilder::plan() const
{
    EasyPlan p; p.referenceCompare=m_referenceCheck->isChecked();p.referenceVideo=m_referencePath->text();p.content = ui->contentCheck->isChecked(); p.motion = ui->motionCheck->isChecked();
    p.delay = ui->delayCheck->isChecked(); p.delayMs = qRound(ui->delaySpin->value() * 1000);
    p.textCompare = ui->textCompareCheck->isChecked(); p.textOcr = true;
    p.expectedText = ui->expectedTextEdit->text(); p.textCondition = ui->textConditionCombo->currentData().toString();
    return p;
}
bool EasyBuilder::append() const {return true;}
void EasyBuilder::refresh()
{
    const auto p = plan();
    ui->expectedTextEdit->setEnabled(p.textCompare); ui->textConditionCombo->setEnabled(p.textCompare); ui->textConditionLabel->setEnabled(p.textCompare);
    ui->previewEdit->setPlainText(easyPlanSteps(p).isEmpty() ? "尚未選擇測試項目，請自行勾選。" : describeEasyPlan(p));
    ui->delaySpin->setEnabled(p.delay);
}
