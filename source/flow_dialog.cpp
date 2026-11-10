#include "flow_dialog.h"
#include "flow_items_dialog.h"
#include "app_scanner.h"
#include "flow_runner.h"
#include "flow_samples.h"
#include "easy_builder.h"
#include "ocr_reader.h"
#include "text_check.h"
#include "ui_flow_dialog.h"
#include "ui_region_dialog.h"
#include <QCloseEvent>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QFormLayout>
#include <QMessageBox>
#include <QMenu>
#include <QCheckBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QTemporaryDir>
#include <QUuid>
#include <QStandardPaths>
#include <windows.h>

namespace {
QString bindingSummary(const FlowStep &step)
{
    if(step.type=="checkVideo")return "參考影片："+step.referenceVideo+QString("；區域 %1 × %2；平均 RGB 差 ≤ 12，異常像素 ≤ 8%；需連續 3 秒正常播放").arg(step.region.width()).arg(step.region.height());
    if (step.type == "clickUIA" || step.type == "readUIA") {
        if (step.selector.isEmpty() && step.automationId.isEmpty()) return "尚未指定控制項：請按「指一下文字／按鈕位置」。";
        if (step.selector == "@point") return QString("已指定位置 (%1, %2)；名稱與 AutomationId 未指定，依位置查找 UIA 控制項。").arg(step.region.x()).arg(step.region.y());
        return "已設定控制項；名稱：" + (step.selector.isEmpty() ? "未指定" : step.selector)
            + "；AutomationId：" + (step.automationId.isEmpty() ? "未指定" : step.automationId);
    }
    if (step.type == "readOCR" || step.type == "checkImage" || step.type == "checkVideo") {
        if (step.region.width() < 1 || step.region.height() < 1) return "尚未框選區域：請按「框選文字或預覽區域」。";
        return QString("已設定%1區域 (%2, %3)，%4 × %5 像素；不需要名稱或 AutomationId。")
            .arg(step.type == "readOCR" ? "OCR 文字" : "影像").arg(step.region.x()).arg(step.region.y()).arg(step.region.width()).arg(step.region.height());
    }
    if (step.type == "clickPoint") return QString("點擊位置 (%1, %2)；不使用 AutomationId。").arg(step.region.x()).arg(step.region.y());
    return "此步驟不需要指定控制項名稱或 AutomationId。";
}
class RegionCanvas final : public QWidget {
public:
    explicit RegionCanvas(const QImage &image, QWidget *parent) : QWidget(parent), m_image(image) {setZoom(1.0);}
    double zoom() const {return m_zoom;}
    void setZoom(double value) {
        m_zoom = qBound(0.05, value, 8.0);
        // Image coordinates are physical pixels; QWidget dimensions are logical pixels.
        const double scale = m_zoom / devicePixelRatioF();
        setFixedSize(qMax(1, qRound(m_image.width() * scale)), qMax(1, qRound(m_image.height() * scale))); update();
    }
    void selectAll() {m_start=m_image.rect().topLeft();m_end=m_image.rect().bottomRight();m_selected=true;m_dragging=false;update();}
    QRect selected() const {return m_selected ? QRect(m_start, m_end).normalized().intersected(m_image.rect()) : QRect();}
protected:
    QRect displayRect() const {
        const QSize size = m_image.size().scaled(this->size(), Qt::KeepAspectRatio);
        return QRect(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    }
    QPoint imagePoint(const QPoint &point) const {
        const auto rect = displayRect();
        return QPoint(qBound(0, (point.x() - rect.x()) * m_image.width() / qMax(1, rect.width()), m_image.width() - 1),
                      qBound(0, (point.y() - rect.y()) * m_image.height() / qMax(1, rect.height()), m_image.height() - 1));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.fillRect(rect(), QColor("#24272b")); const auto display = displayRect(); painter.drawImage(display, m_image);
        if (m_selected) {
            const auto selection = selected();
            const QRectF visual(display.x() + double(selection.x()) * display.width() / m_image.width(),
                display.y() + double(selection.y()) * display.height() / m_image.height(),
                double(selection.width()) * display.width() / m_image.width(), double(selection.height()) * display.height() / m_image.height());
            painter.setPen(QPen(Qt::red, 2)); painter.setBrush(QColor(255, 0, 0, 35)); painter.drawRect(visual);
        }
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && displayRect().contains(event->position().toPoint())) {
            m_start = m_end = imagePoint(event->position().toPoint()); m_dragging = m_selected = true; update();
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override {if (m_dragging) {m_end = imagePoint(event->position().toPoint()); update();}}
    void mouseReleaseEvent(QMouseEvent *event) override {if (m_dragging) {m_end = imagePoint(event->position().toPoint()); m_dragging = false; update();}}
private:
    QImage m_image; QPoint m_start, m_end; double m_zoom = 1.0; bool m_dragging = false, m_selected = false;
};
void setupRegionDialog(QDialog &dialog, Ui::RegionDialog &ui, const QImage &image)
{
    ui.setupUi(&dialog);
    if (const auto screen = dialog.screen()) {
        const auto available = screen->availableGeometry().size() - QSize(40, 60);
        dialog.resize(QSize(qRound(available.width() * 0.95), qRound(available.height() * 0.95)));
    }
    auto *canvas = new RegionCanvas(image, ui.imageScroll);
    ui.imageScroll->setWidget(canvas);
    QObject::connect(ui.wholeImageButton,&QPushButton::clicked,&dialog,[canvas,&dialog]{canvas->selectAll();dialog.accept();});
    auto zoom = [canvas, label = ui.zoomLabel, size = image.size()](double value) {
        canvas->setZoom(value); label->setText(QString("%1% ｜ 原圖 %2 × %3 像素").arg(qRound(canvas->zoom() * 100)).arg(size.width()).arg(size.height()));
    };
    auto fit = [canvas, scroll = ui.imageScroll, zoom, image] {
        const QSize space = scroll->viewport()->size() - QSize(4, 4);
        const double dpi = canvas->devicePixelRatioF();
        zoom(qMin(1.0, qMin(double(space.width()) * dpi / image.width(), double(space.height()) * dpi / image.height())));
    };
    QObject::connect(ui.zoomInButton, &QPushButton::clicked, &dialog, [canvas, zoom] {zoom(canvas->zoom() * 1.25);});
    QObject::connect(ui.zoomOutButton, &QPushButton::clicked, &dialog, [canvas, zoom] {zoom(canvas->zoom() / 1.25);});
    QObject::connect(ui.actualButton, &QPushButton::clicked, &dialog, [zoom] {zoom(1.0);});
    QObject::connect(ui.fitButton, &QPushButton::clicked, &dialog, fit);
    QTimer::singleShot(0, &dialog, fit);
}
}

int regionPreview()
{
    QImage image(1600, 900, QImage::Format_RGB32); image.fill(Qt::white);
    QPainter painter(&image); painter.setPen(Qt::black); painter.setFont(QFont("Microsoft JhengHei UI", 16));
    for (int row = 0; row < 24; ++row)
        painter.drawText(24, 40 + row * 34, QString("Running    1920 x 1080    FPS: 60.00    音訊接收：%1").arg(row * 64));
    painter.end();
    QDialog dialog; Ui::RegionDialog ui; setupRegionDialog(dialog, ui, image);
    dialog.show(); QApplication::processEvents();
    const QString directory = QCoreApplication::applicationDirPath();
    const bool fit = dialog.grab().save(directory + "/region_fit_preview.png");
    ui.actualButton->click(); QApplication::processEvents();
    const bool actual = dialog.grab().save(directory + "/region_actual_preview.png");
    ui.wholeImageButton->click();
    const auto *canvas=static_cast<RegionCanvas *>(ui.imageScroll->widget());
    return fit && actual && dialog.result()==QDialog::Accepted && canvas->selected()==image.rect() ? 0 : 1;
}

FlowDialog::FlowDialog(const QString &target, QWidget *parent, const QString &file) : QDialog(parent), ui(std::make_unique<Ui::FlowDialog>())
{
    ui->setupUi(this);
    auto *reanalyze=new QPushButton("分析已擷取資料…",this);reanalyze->setObjectName("reanalyzeButton");ui->runLayout->insertWidget(1,reanalyze);
    connect(reanalyze,&QPushButton::clicked,this,[this]{
        const int row=ui->stepsTable->currentRow();if(m_busy || row<0)return;
        if(m_steps[row].type!="checkVideo" && m_steps[row].type!="checkImage"){QMessageBox::information(this,"離線分析","請選取畫面檢查或參考影片比較步驟。");return;}
        const auto path=QFileDialog::getOpenFileName(this,"選擇先前的擷取資料",m_reportDirectory,"影格擷取 (*.gvfgcap)");
        if(path.isEmpty())return;m_replayCaptureFile=path;runFlow(true);m_replayCaptureFile.clear();
    });
    auto *captureSettings=new QPushButton("影像擷取設定…",this);captureSettings->setObjectName("captureSettingsButton");
    ui->runLayout->insertWidget(1,captureSettings);
    connect(captureSettings,&QPushButton::clicked,this,[this]{
        const int row=ui->stepsTable->currentRow();if(m_busy || row<0)return;
        readEditor();const auto &step=m_steps[row];
        if(step.type!="checkVideo" && step.type!="checkImage" && step.type!="readOCR"){
            QMessageBox::information(this,"影像擷取設定","請選取畫面檢查、參考影片比較或 OCR 步驟。UIA 讀取不使用影像採樣。");return;}
        QDialog dialog(this);dialog.setWindowTitle("影像擷取設定 — "+flowStepDisplayName(step));auto *form=new QFormLayout(&dialog);
        auto *mode=new QComboBox(&dialog);mode->addItem("背景視窗（可被遮住，不可最小化）","window");mode->addItem("前景桌面（相容模式，需保持可見）","desktop");mode->setCurrentIndex(mode->findData(step.captureMode));
        auto *fps=new QSpinBox(&dialog);fps->setRange(1,120);fps->setValue(step.sampleFps);fps->setSuffix(" FPS");fps->setEnabled(step.type!="readOCR");
        auto *seconds=new QDoubleSpinBox(&dialog);seconds->setRange(step.type=="checkVideo"?3.5:0.1,600);seconds->setDecimals(1);seconds->setValue(step.captureDurationMs/1000.0);seconds->setSuffix(" 秒");seconds->setEnabled(step.type!="readOCR");
        QLineEdit *referencePath=nullptr;
        if(step.type=="checkVideo"){
            auto *referenceRow=new QWidget(&dialog);auto *referenceLayout=new QHBoxLayout(referenceRow);referenceLayout->setContentsMargins(0,0,0,0);
            referencePath=new QLineEdit(step.referenceVideo,referenceRow);referencePath->setObjectName("referenceVideoPath");referencePath->setMinimumWidth(300);referencePath->setPlaceholderText("選擇與訊號來源相同的本機影片");
            auto *browseReference=new QPushButton("選擇影片…",referenceRow);referenceLayout->addWidget(referencePath,1);referenceLayout->addWidget(browseReference);form->addRow("參考影片",referenceRow);
            connect(browseReference,&QPushButton::clicked,&dialog,[&dialog,referencePath]{
                const auto path=QFileDialog::getOpenFileName(&dialog,"選擇參考影片",referencePath->text(),"影片 (*.mp4 *.avi *.mov *.mkv *.wmv);;所有檔案 (*)");if(!path.isEmpty())referencePath->setText(path);
            });
        }
        auto *activate=new QCheckBox("前景相容：擷取前啟用預覽視窗（必要時才勾選）",&dialog);activate->setChecked(step.activateBeforeCapture);
        activate->setToolTip("此動作會切換前景，讓部分背景停止繪圖的 APP 恢復更新；取消勾選才是不主動切前景的背景擷取。前景桌面模式本身仍需要前景。");
        form->addRow("腳本動作",activate);
        form->addRow("擷取秒數",seconds);form->addRow("擷取方式",mode);form->addRow("目標採樣頻率",fps);
        auto *hint=new QLabel("先擷取，再離線比對；擷取期間不做參考搜尋或寫圖。比較影格暫存為最多 320×180，記憶體上限 256 MiB；超過上限會停止並提示縮短秒數。實際頻率受 APP、螢幕與搬移速度限制。OCR 每次辨識耗時較長，不使用 FPS 設定。背景擷取本身不需焦點；是否先啟用視窗由上述腳本動作決定。最小化、鎖定桌面或受保護內容可能無法擷取。",&dialog);hint->setWordWrap(true);form->addRow(hint);
        auto *buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);form->addRow(buttons);
        connect(buttons,&QDialogButtonBox::accepted,&dialog,[&dialog,referencePath]{
            if(referencePath && !QFileInfo(referencePath->text().trimmed()).isFile()){QMessageBox::warning(&dialog,"請選擇參考影片","請選擇存在的本機影片檔案。");referencePath->setFocus();return;}
            dialog.accept();
        });connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        if(dialog.exec()!=QDialog::Accepted)return;
        if(referencePath)m_steps[row].referenceVideo=QFileInfo(referencePath->text().trimmed()).absoluteFilePath();
        m_steps[row].activateBeforeCapture=activate->isChecked();
        m_steps[row].captureDurationMs=qRound(seconds->value()*1000);m_steps[row].captureMode=mode->currentData().toString();m_steps[row].sampleFps=fps->value();markStepForRetest(row);refreshBinding();ui->fileLabel->setText("流程已修改，請保存流程");
    });

    auto *referenceButton=new QPushButton("變更所選步驟的參考影片…",ui->stepGroup);
    ui->stepGroup->layout()->addWidget(referenceButton);
    connect(referenceButton,&QPushButton::clicked,this,[this]{
        const int row=ui->stepsTable->currentRow();
        if(row<0 || m_steps[row].type!="checkVideo"){QMessageBox::information(this,"參考影片","請先選取參考影片比較步驟；可由補充步驟加入。");return;}
        const auto path=QFileDialog::getOpenFileName(this,"選擇參考影片",m_steps[row].referenceVideo,"影片 (*.mp4 *.avi *.mov *.mkv *.wmv);;所有檔案 (*)");
        if(!path.isEmpty()){m_steps[row].referenceVideo=path;markStepForRetest(row);refreshBinding();}
    });
    m_readDetails=new QPlainTextEdit(this);m_readDetails->setReadOnly(true);m_readDetails->setMaximumHeight(110);
    m_readDetails->setPlaceholderText("選取已執行的步驟，查看讀取值、判定條件與原始文字。");m_readDetails->hide();
    ui->mainLayout->insertWidget(ui->mainLayout->indexOf(ui->stepsTable)+1,m_readDetails);
    connect(ui->stepsTable,&QTableWidget::currentCellChanged,this,[this]{refreshReadDetails();});
    auto *scanButton = new QPushButton("掃描 APP 建立腳本…", this);
    scanButton->setObjectName("appScannerButton");scanButton->setMinimumHeight(40);
    scanButton->setStyleSheet("QPushButton {background:#2563eb; color:white; padding:6px 16px; border-radius:5px;} QPushButton:disabled {background:#cbd5e1;}");
    ui->easyLayout->insertWidget(0, scanButton);
    connect(scanButton, &QPushButton::clicked, this, [this] {
        if (m_busy) return;readEditor();
        if (!QFileInfo(ui->exeEdit->text()).isFile()) {QMessageBox::information(this,"請先選擇 APP","先選擇待測 EXE，再按 F6 開啟程式後掃描。");return;}
        AppScannerDialog scanner(currentFlow(), this);
        if (scanner.exec() != QDialog::Accepted) return;
        const auto draft=scanner.draft();
        const bool wasEmpty=m_steps.isEmpty();
        if (m_steps.isEmpty()) {
            FlowStep launch;launch.type="launch";launch.name="啟動待測 APP";
            FlowStep wait;wait.type="waitWindow";wait.name="等待主視窗";wait.timeoutMs=60000;
            m_steps={launch,wait};updateTable(true);
        }
        if (m_steps.size()==2 && m_steps[0].type=="launch" && m_steps[1].type=="waitWindow"
            && m_steps[0].testItemId.isEmpty() && m_steps[1].testItemId.isEmpty()
            && (wasEmpty || scanner.insertionIndex()==m_steps.size())) {
            const QString id=QUuid::createUuid().toString(QUuid::Id128);
            m_testItems.append({id,"啟動測試程式","確認設備與訊號線已接妥。","正在啟動 APP 並等待視窗。","請確認程式與驅動已安裝。"});
            m_steps[0].testItemId=m_steps[1].testItemId=id;
        }
        if (m_flowName.isEmpty())m_flowName=QFileInfo(ui->exeEdit->text()).completeBaseName()+" 自動化測試";
        insertScanDraft(wasEmpty?int(m_steps.size()):scanner.insertionIndex(),draft);
    });
    auto *itemsButton = new QPushButton("測試項目與現場說明…", this);
    itemsButton->setObjectName("testItemsButton");
    itemsButton->setMinimumWidth(180);
    ui->fileActions->insertWidget(2, itemsButton);
    connect(itemsButton, &QPushButton::clicked, this, [this] {
        if (m_busy) return;
        readEditor();
        FlowItemsDialog dialog(currentFlow(), ui->stepsTable->currentRow(), this);
        if (dialog.exec() != QDialog::Accepted) return;
        const auto flow = dialog.resultFlow();
        QString error;
        if (!applyItems(flow, error)) QMessageBox::critical(this, "分類保存失敗", error);
    });
    m_waitTimer.setInterval(250);
    connect(&m_waitTimer, &QTimer::timeout, this, [this] {
        if (m_activeStep < 0 || m_activeStep >= m_steps.size()) return;
        const auto &step = m_steps[m_activeStep];
        if(step.type=="checkImage" || step.type=="checkVideo")return;
        const auto elapsed = m_stepElapsed.elapsed();
        ui->statusLabel->setText(QString("步驟 %1：%2｜已等待 %3 / %4 秒%5")
            .arg(m_activeStep + 1).arg(step.name).arg(elapsed / 1000).arg((step.timeoutMs + 999) / 1000)
            .arg(elapsed > step.timeoutMs + 1000 ? "；等待外部操作返回，可按停止" : QString()));
    });
    connect(ui->refreshWindowsButton, &QPushButton::clicked, this, &FlowDialog::refreshWindows);
    connect(ui->windowCombo, &QComboBox::currentIndexChanged, this, [this] {
        if (m_loading) return;
        const int row = ui->stepsTable->currentRow(); if (row < 0 || row >= m_steps.size()) return;
        const auto title = ui->windowCombo->currentData().toString();
        if (m_steps[row].windowTitle != title) m_lastValue = QString();
        m_steps[row].windowTitle = title; refreshBinding();
    });
    connect(ui->advancedCheck, &QCheckBox::toggled, this, &FlowDialog::setAdvanced);
    connect(ui->insertBeforeButton, &QPushButton::clicked, this, [this] {buildInsertion(false);});
    connect(ui->insertAfterButton, &QPushButton::clicked, this, [this] {buildInsertion(true);});
    connect(ui->easyButton, &QPushButton::clicked, this, [this] {
        EasyBuilder builder(this); if (builder.exec() != QDialog::Accepted) return;
        if (!builder.append() || m_steps.isEmpty()) {
            FlowStep launch; launch.type = "launch"; launch.name = "開啟待測 APP";
            FlowStep wait; wait.type = "waitWindow"; wait.name = "等待 APP 出現"; wait.timeoutMs = 60000;
            m_steps = {launch, wait};
            updateTable();
        }
        insertSteps(static_cast<int>(m_steps.size()), easyPlanSteps(builder.plan()));
        ui->statusLabel->setText("請選一個檢查步驟，再指一下要讀的位置，或框選預覽。完成後保存並試跑。" );
    });
    if (const auto screen = this->screen()) resize(size().boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    for (const auto &type : flowTypes()) ui->typeCombo->addItem(flowTypeName(type), type);
    const QStringList conditions{"字串相等", "字串包含", "正規式符合", "數字 >", "數字 ≥", "數字 <", "數字 ≤", "數字相等", "數字範圍（含邊界）", "影像有內容（亮度差異或前後變化 ≥）", "影像有變化（平均 RGB 差異 ≥）", "文字等於（不分大小寫）", "包含文字（不分大小寫）"};
    const QStringList tokens{"equals", "contains", "regex", "gt", "ge", "lt", "le", "numberEquals", "range", "contrast", "change", "equalsIgnoreCase", "containsIgnoreCase"};
    for (int i = 0; i < tokens.size(); ++i) ui->conditionCombo->addItem(conditions[i], tokens[i]);
    ui->sampleCombo->addItems(flowSampleNames());
    auto sampleHint = [this] {
        const int index = ui->sampleCombo->currentIndex();
        ui->sampleHintLabel->setText(flowSampleHint(index)); ui->sampleSourceCombo->setEnabled(!m_busy && index < 6);
    };
    connect(ui->sampleCombo, &QComboBox::currentIndexChanged, this, sampleHint); sampleHint();
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    ui->stepsTable->setColumnWidth(4, 260);
    ui->stepsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    ui->exeEdit->setText(target);
    for (auto *edit : {ui->exeEdit, ui->argsEdit, ui->cwdEdit, ui->ocrEdit, ui->languageEdit})
        connect(edit, &QLineEdit::textEdited, this, [this] {m_lastValue = QString();});
    auto browse = [this](QLineEdit *edit) {
        const auto path = QFileDialog::getOpenFileName(this, "選擇 EXE", edit->text(), "執行檔 (*.exe)");
        if (!path.isEmpty()) edit->setText(path);
    };
    connect(ui->browseExeButton, &QPushButton::clicked, this, [=] {browse(ui->exeEdit);});
    connect(ui->launchExeButton, &QPushButton::clicked, this, [this] {
        const auto flow = currentFlow();
        const QFileInfo exe(flow.executable);
        if (!exe.isAbsolute() || !exe.isFile() || exe.suffix().compare("exe", Qt::CaseInsensitive) != 0) {
            QMessageBox::warning(this, "無法開啟待測 EXE", "請先選擇有效的待測 EXE。");
            return;
        }
        const auto windows = flowWindows(exe.absoluteFilePath());
        if (!windows.isEmpty()) {
            const auto window = reinterpret_cast<HWND>(windows.first().handle);
            if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
            if (!SetForegroundWindow(window)) log("待測 APP 已開啟，請切換至其視窗。");
            refreshWindows();
            return;
        }
        const auto workDir = flow.workingDirectory.isEmpty() ? exe.absolutePath() : flow.workingDirectory;
        if (!QDir(workDir).exists() || !QProcess::startDetached(exe.absoluteFilePath(),
                QProcess::splitCommand(flow.arguments), workDir)) {
            QMessageBox::warning(this, "無法開啟待測 EXE", "請確認啟動參數、工作目錄及執行權限。");
            return;
        }
        log("已啟動待測 APP。視窗出現後即可取控制項或框選區域。");
        QTimer::singleShot(1000, this, [this] {if (!m_busy) refreshWindows();});
    });
    connect(ui->browseOcrButton, &QPushButton::clicked, this, [=] {browse(ui->ocrEdit);});
    connect(ui->addButton, &QPushButton::clicked, this, [this] {readEditor(); FlowStep step; step.name = "新步驟"; insertSteps(static_cast<int>(m_steps.size()), {step});});
    connect(ui->removeButton, &QPushButton::clicked, this, &FlowDialog::removeSelectedStep);
    connect(ui->upButton, &QPushButton::clicked, this, [this] {moveSelectedStep(-1);});
    connect(ui->downButton, &QPushButton::clicked, this, [this] {moveSelectedStep(1);});
    connect(ui->stepsTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {showEditor(row);});
    ui->stepsTable->setToolTip("雙擊步驟編輯；畫面／OCR／影片比較直接框選區域，等待步驟直接修改秒數。");
    auto editStep=[this](int row, int) {
        if(m_busy || m_loading || row<0 || row>=m_steps.size())return;
        readEditor();ui->stepsTable->selectRow(row);showEditor(row);
        const auto type=m_steps[row].type;
        if(type=="checkVideo" || type=="checkImage" || type=="readOCR" || type=="clickPoint") {
            // Use the same crop editor as the explicit button, including its
            // validation, OCR conditions, retest marking and cancel behaviour.
            ui->regionButton->click();return;
        }
        if(type=="delay") {
            bool accepted=false;
            const double seconds=QInputDialog::getDouble(this,"編輯等待時間","等待幾秒：",
                m_steps[row].timeoutMs/1000.0,0.001,600.0,3,&accepted);
            if(!accepted)return;
            const int milliseconds=qRound(seconds*1000);
            if(milliseconds==m_steps[row].timeoutMs)return;
            const QString oldName=QString("等待 %1 秒").arg(QString::number(m_steps[row].timeoutMs/1000.0));
            if(m_steps[row].name==oldName)m_steps[row].name=QString("等待 %1 秒").arg(QString::number(milliseconds/1000.0));
            m_steps[row].timeoutMs=milliseconds;updateTable(true);ui->stepsTable->selectRow(row);showEditor(row);
            markStepForRetest(row);ui->fileLabel->setText("流程已修改，請保存流程");return;
        }
        ui->advancedCheck->setChecked(true);showEditor(row);
        QWidget *field=type=="readUIA" || type=="assert"?static_cast<QWidget *>(ui->expectedEdit)
            :type=="clickUIA"?static_cast<QWidget *>(ui->selectorEdit)
            :type=="hotkey" || type=="typeText"?static_cast<QWidget *>(ui->textEdit)
            :type=="waitWindow"?static_cast<QWidget *>(ui->timeoutSpin):static_cast<QWidget *>(ui->nameEdit);
        field->setFocus(Qt::OtherFocusReason);
        ui->statusLabel->setText("已開啟所選步驟設定；修改後請保存流程。");
    };
    connect(ui->stepsTable, &QTableWidget::cellDoubleClicked, this, editStep);
    ui->stepsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    ui->stepsTable->setToolTip("雙擊快速編輯；右鍵可選擇擷取設定、框選區域、判定條件或單步試跑。");
    connect(ui->stepsTable,&QWidget::customContextMenuRequested,this,[this,editStep](const QPoint &point){
        if(m_busy || m_loading)return;
        const auto *cell=ui->stepsTable->itemAt(point);if(!cell)return;
        readEditor();const int row=cell->row();ui->stepsTable->selectRow(row);showEditor(row);
        const QString type=m_steps[row].type;
        const bool image=type=="checkImage" || type=="checkVideo",ocr=type=="readOCR",uia=type=="clickUIA" || type=="readUIA";
        QMenu menu(this);menu.addSection(flowStepDisplayName(m_steps[row]).left(80));
        auto settings=[this,row]{ui->advancedCheck->setChecked(true);showEditor(row);ui->nameEdit->setFocus();};
        if(image || ocr){
            menu.addAction(type=="checkVideo"?"擷取與參考影片設定…":"影像擷取設定…",this,[this]{if(auto *button=findChild<QPushButton *>("captureSettingsButton"))button->click();});
            menu.addAction(image?"框選區域／使用整個畫面…":"框選 OCR 文字與判定…",ui->regionButton,&QPushButton::click);
            if(image)menu.addAction("分析已擷取資料…",this,[this]{if(auto *button=findChild<QPushButton *>("reanalyzeButton"))button->click();});
        }
        if(type=="delay")menu.addAction("編輯等待時間…",this,[editStep,row]{editStep(row,0);});
        if(type=="readUIA" || type=="assert")menu.addAction("讀取與 PASS 條件…",this,[this,row]{ui->advancedCheck->setChecked(true);showEditor(row);ui->expectedEdit->setFocus();});
        if(type=="readUIA" || ocr)menu.addAction("預覽讀取並選取數字…",ui->previewNumberButton,&QPushButton::click);
        if(uia)menu.addAction("重新指定文字／按鈕位置…",ui->pickButton,&QPushButton::click);
        if(type=="clickPoint")menu.addAction("重新指定點擊位置…",ui->regionButton,&QPushButton::click);
        menu.addAction("編輯步驟設定…",this,settings);
        menu.addAction("現場測試項目與說明…",this,[this]{if(auto *button=findChild<QPushButton *>("testItemsButton"))button->click();});
        menu.addSeparator();menu.addAction("單步試跑",this,[this]{runFlow(true);});
        menu.exec(ui->stepsTable->viewport()->mapToGlobal(point));
    });
    for (auto *edit : {ui->nameEdit, ui->selectorEdit, ui->idEdit, ui->textEdit, ui->expectedEdit})
        connect(edit, &QLineEdit::textEdited, this, &FlowDialog::readEditor);
    for (auto *spin : {ui->timeoutSpin, ui->xSpin, ui->ySpin, ui->widthSpin, ui->heightSpin})
        connect(spin, &QSpinBox::valueChanged, this, &FlowDialog::readEditor);
    connect(ui->typeCombo, &QComboBox::currentIndexChanged, this, &FlowDialog::readEditor);
    connect(ui->conditionCombo, &QComboBox::currentIndexChanged, this, &FlowDialog::readEditor);
    connect(ui->sampleButton, &QPushButton::clicked, this, [this] {
        if (ui->sampleApplyCombo->currentIndex() == 1 && !m_steps.isEmpty()
            && QMessageBox::question(this, "載入範例", "以所選範例取代目前的步驟？未保存內容會被取代。") != QMessageBox::Yes) return;
        sample();
    });
    connect(ui->openButton, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "開啟流程", m_file, "測試流程 (*.json)");
        if (path.isEmpty()) return;
        TestFlow loaded; QString error;
        if (!loadFlow(path, loaded, error)) {QMessageBox::critical(this, "無法開啟", error); return;}
        applyFlow(loaded); m_file = path; ui->fileLabel->setText(QFileInfo(path).fileName()); m_lastValue = QString();
    });
    connect(ui->saveButton, &QPushButton::clicked, this, [this] {
        readEditor(); auto path = QFileDialog::getSaveFileName(this, "保存流程", m_file, "測試流程 (*.json)");
        if (path.isEmpty()) return; if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";
        QString error;
        if (!saveFlow(path, currentFlow(), error)) {QMessageBox::critical(this, "保存失敗", error); return;}
        m_file = path; ui->fileLabel->setText(QFileInfo(path).fileName());
        QSettings settings(QCoreApplication::applicationDirPath() + "/test_settings.ini", QSettings::IniFormat);
        settings.setValue("Flow/LastFile", path); settings.sync();
        log("流程已保存：" + path);
        if (settings.status() != QSettings::NoError) log("流程已保存，但無法記住上次路徑；下次請使用開啟流程。" );
    });
    connect(ui->singleButton, &QPushButton::clicked, this, [this] {runFlow(true);});
    connect(ui->previewNumberButton, &QPushButton::clicked, this, &FlowDialog::previewNumber);
    connect(ui->runButton, &QPushButton::clicked, this, [this] {runFlow(false);});
    connect(ui->stopButton, &QPushButton::clicked, this, [this] {m_waitTimer.stop(); if (m_runner) m_runner->stop(); ui->statusLabel->setText("正在停止，等待目前操作返回…");});
    connect(ui->regionButton, &QPushButton::clicked, this, [this] {
        const int selected = ui->stepsTable->currentRow();
        const auto type = selected >= 0 ? m_steps[selected].type : QString();
        const bool addText = type != "readUIA" && type != "readOCR" && type != "checkImage" && type != "checkVideo" && type != "clickPoint";
        const QString title = selected >= 0 ? m_steps[selected].windowTitle : QString();
        const auto window = findFlowWindow(ui->exeEdit->text(), 0, title); QString error;
        if (!window) {QMessageBox::warning(this, "請選擇待測視窗", flowWindowError(ui->exeEdit->text(), 0, title)); refreshWindows(); return;}
        const auto image = selected>=0 && m_steps[selected].captureMode=="desktop" ? captureDesktopWindow(window,error) : captureFlowWindow(window, error);
        if (image.isNull()) {QMessageBox::warning(this, "無法擷取", error); return;}
        QDialog dialog(this); Ui::RegionDialog regionUi; setupRegionDialog(dialog, regionUi, image);
        regionUi.wholeImageButton->setVisible(type=="checkImage" || type=="checkVideo");
        auto *canvas = static_cast<RegionCanvas *>(regionUi.imageScroll->widget());
        if (dialog.exec() != QDialog::Accepted) return;
        const auto rect = canvas->selected();
        if (!rect.isValid()) {QMessageBox::warning(this, "尚未框選", "請拖曳框選有效區域"); return;}
        if (type != "clickPoint" && type != "checkImage" && type != "checkVideo") {
            const auto engine = detectOcrExecutable(ui->ocrEdit->text());
            if (engine.isEmpty()) {QMessageBox::warning(this, "無法讀取圖片文字", "框選已完成，但找不到文字辨識元件。請安裝元件後重新讀取。"); return;}
            auto step = addText ? FlowStep() : m_steps[selected]; step.region = rect;
            if (addText) {step.name = "文字狀態檢查"; step.timeoutMs = 30000; step.windowTitle = title;}
            const auto language = defaultOcrLanguage(engine);
            TextCheck check(image.copy(rect), step, engine, language, this);
            if (check.exec() != QDialog::Accepted) return;
            ui->ocrEdit->setText(engine); ui->languageEdit->setText(language);
            const int row = addText ? static_cast<int>(m_steps.size()) : selected;
            if (addText) m_steps.append(check.resultStep()); else m_steps[row] = check.resultStep();
            updateTable(true); markStepForRetest(row); ui->stepsTable->selectRow(row); showEditor(row);
            m_lastValue = QString(); ui->statusLabel->setText("文字區域與判定已設定；請單步試跑，再保存流程。"); return;
        }
        m_loading = true;
        if (ui->typeCombo->currentData() == "clickPoint") {ui->xSpin->setValue(rect.center().x()); ui->ySpin->setValue(rect.center().y());}
        else {
            if (ui->typeCombo->currentData() != "checkImage" && ui->typeCombo->currentData() != "checkVideo") ui->typeCombo->setCurrentIndex(ui->typeCombo->findData("readOCR"));
            ui->xSpin->setValue(rect.x()); ui->ySpin->setValue(rect.y()); ui->widthSpin->setValue(rect.width()); ui->heightSpin->setValue(rect.height());
        }
        m_loading = false; readEditor(); markStepForRetest(selected);
    });
    connect(ui->pickButton, &QPushButton::clicked, this, [this] {
        if (m_picking) return; m_picking = true; setBusy(true);
        ui->statusLabel->setText("請將滑鼠停在待測控制項上，3 秒後讀取");
        QTimer::singleShot(3000, this, [this] {
            QString name, id, error; const auto point = QCursor::pos();
            const int row = ui->stepsTable->currentRow();
            const auto window = flowWindowAtPoint(ui->exeEdit->text(), point);
            if (!window) {log("滑鼠位置不是待測 APP 的視窗，請指向待測 APP 的文字或按鈕。"); m_picking = false; setBusy(false); refreshWindows(); return;}
            if (pickFlowControl(window, point.x(), point.y(), name, id, error)) {
                for (const auto &candidate : flowWindows(ui->exeEdit->text()))
                    if (candidate.handle == window) m_steps[row].windowTitle = candidate.title;
                const auto client = flowClientPoint(window, point);
                m_loading = true; ui->selectorEdit->setText(id.isEmpty() ? "@point" : QString()); ui->idEdit->setText(id);
                ui->xSpin->setValue(client.x()); ui->ySpin->setValue(client.y()); m_loading = false; readEditor();
                log("控制項：" + name + "；AutomationId=" + id);
                refreshWindows(); log("所屬視窗：" + m_steps[row].windowTitle);
                if (id.isEmpty()) log("已記住視窗內的位置；視窗布局或縮放改變時，請重新指一下。" );
            } else log(error);
            m_picking = false; setBusy(false); ui->statusLabel->setText("控制項選取完成，請單步試跑確認");
        });
    });
    QSettings settings(QCoreApplication::applicationDirPath() + "/test_settings.ini", QSettings::IniFormat);
    const auto saved = file.isEmpty() ? settings.value("Flow/LastFile").toString() : file; TestFlow loaded; QString error;
    if (!saved.isEmpty() && loadFlow(saved, loaded, error)) {applyFlow(loaded); m_file = saved; ui->fileLabel->setText(QFileInfo(saved).fileName());}
    else {
        FlowStep launch; launch.type = "launch"; launch.name = "啟動待測 APP";
        FlowStep wait; wait.type = "waitWindow"; wait.name = "等待主視窗";
        m_steps = {launch, wait}; updateTable(); ui->stepsTable->selectRow(0);
    }
    setAdvanced(false);
}
FlowDialog::~FlowDialog() {if (m_runner) {m_runner->stop(); m_runner->wait();}}
bool FlowDialog::applyItems(const TestFlow &flow, QString &error)
{
    if (!m_file.isEmpty() && !saveFlow(m_file, flow, error)) return false;
    m_flowName = flow.name; m_flowVersion = flow.version; m_testItems = flow.testItems; m_steps = flow.steps;
    updateTable(true);
    if (m_file.isEmpty()) {
        ui->fileLabel->setText("分類已套用，尚未保存 JSON");
        ui->statusLabel->setText("此流程尚無檔案；請按「保存流程」後，再從首頁載入。");
    } else {
        ui->fileLabel->setText(QFileInfo(m_file).fileName());
        ui->statusLabel->setText("分類與步驟已保存；關閉編輯視窗後，首頁會重新讀取目前腳本。");
        log("分類已保存至目前 JSON：" + m_file);
    }
    return true;
}
bool FlowDialog::runSelfTest()
{
    TestFlow flow;flow.executable=QCoreApplication::applicationFilePath();
    FlowStep step;step.type="delay";step.name="編輯器完整試跑驗證";step.timeoutMs=50;
    flow.steps={step,step};flow.testItems={{"g","通用測試項目","不需操作","等待中","通知工程人員"}};
    for(auto &s:flow.steps)s.testItemId="g";
    applyFlow(flow);
    QTemporaryDir saved(QCoreApplication::applicationDirPath()+"/editor_check_XXXXXX");
    if(!saved.isValid())return false;
    m_file=saved.path()+"/flow.json";QString saveError;TestFlow reloaded;
    if(!applyItems(flow,saveError) || !loadFlow(m_file,reloaded,saveError)
        || reloaded.testItems.size()!=1 || reloaded.steps[0].testItemId!="g")return false;
    for(int run=0;run<5;++run) {
        runFlow(false);
        if(!m_runner)return false;
        QEventLoop loop;QTimer limit;limit.setSingleShot(true);
        connect(m_runner,&QThread::finished,&loop,&QEventLoop::quit);
        connect(&limit,&QTimer::timeout,&loop,&QEventLoop::quit);limit.start(5000);loop.exec();
        if(m_runner || !ui->statusLabel->text().startsWith("PASS"))return false;
    }
    const QString detail="讀取值：285 ｜ CH0 Audio received > 100 ｜ PASS\n原始文字：\nCH0 Video received=171\nCH0 Audio received=285";
    ui->stepsTable->selectRow(0);setReadResult(0,detail);updateTable(true);
    return ui->stepsTable->item(0,3)->text()==detail.section('\n',0,0)
        && m_readDetails->toPlainText()==detail;
}
void FlowDialog::buildInsertion(bool after)
{
    if (m_busy) return;
    const int row = ui->stepsTable->currentRow();
    const int index = row < 0 ? static_cast<int>(m_steps.size()) : row + (after ? 1 : 0);
    EasyBuilder builder(this, true);
    if (builder.exec() != QDialog::Accepted) return;
    readEditor();const auto steps=easyPlanSteps(builder.plan());insertSteps(index, steps);
    for(const auto &step:steps)if(step.type=="readOCR" || step.type=="checkImage" || step.type=="checkVideo") {
        ui->advancedCheck->setChecked(true);
        ui->statusLabel->setText("已加入框選檢查；請按「框選文字或預覽區域」指定範圍，再單步試跑。");break;
    }
}
void FlowDialog::insertSteps(int index, const QVector<FlowStep> &steps)
{
    if (steps.isEmpty() || index < 0 || index > m_steps.size()) return;
    const bool wasLoading = m_loading; m_loading = true;
    for (int offset = 0; offset < steps.size(); ++offset) {
        FlowStep inserted = steps[offset];
        if (inserted.testItemId.isEmpty() && index > 0 && index < m_steps.size()
            && m_steps[index-1].testItemId == m_steps[index].testItemId)
            inserted.testItemId = m_steps[index-1].testItemId;
        m_steps.insert(index + offset, inserted);
        // Shift existing table cells along with their step instead of resetting results.
        ui->stepsTable->insertRow(index + offset);
    }
    updateTable(true); m_loading = wasLoading;
    ui->stepsTable->selectRow(index); showEditor(index);
    ui->statusLabel->setText(QString("已插入 %1 個步驟；原有目標設定與測試結果已保留。請設定新步驟的目標並單步試跑。").arg(steps.size()));
    ui->fileLabel->setText("流程已修改，請保存流程");
}
void FlowDialog::insertScanDraft(int index,const TestFlow &draft)
{
    if(index<0 || index>m_steps.size() || draft.steps.isEmpty())return;
    auto steps=draft.steps;
    const bool inside=index>0 && index<m_steps.size() && !m_steps[index].testItemId.isEmpty()
        && m_steps[index-1].testItemId==m_steps[index].testItemId;
    if(inside)for(auto &step:steps)step.testItemId=m_steps[index].testItemId;
    else m_testItems+=draft.testItems;
    insertSteps(index,steps);
    ui->statusLabel->setText(QString("已在第 %1 步位置插入 %2 個掃描步驟%3；請試跑並保存流程。")
        .arg(index+1).arg(steps.size()).arg(inside?QString("，併入原測試項目"):QString()));
}
void FlowDialog::removeSelectedStep()
{
    const int row = ui->stepsTable->currentRow();
    if (m_busy || row < 0 || row >= m_steps.size()) return;
    const bool wasLoading = m_loading; m_loading = true;
    m_steps.removeAt(row); ui->stepsTable->removeRow(row);
    m_loading = wasLoading;
    const int selected = m_steps.isEmpty() ? -1 : qMin(row, static_cast<int>(m_steps.size()) - 1);
    if (selected >= 0) ui->stepsTable->selectRow(selected);
    else m_lastValue = QString();
    showEditor(selected);
    ui->fileLabel->setText("流程已修改，請保存流程");
    ui->statusLabel->setText("已刪除所選步驟；其他步驟的目標設定、測試結果與讀取值已保留。");
}
void FlowDialog::moveSelectedStep(int offset)
{
    if (m_busy || (offset != -1 && offset != 1)) return;
    readEditor(); const int row = ui->stepsTable->currentRow(), next = row + offset;
    if (row < 0 || next < 0 || next >= m_steps.size()) return;
    TestFlow moved = currentFlow(); moved.steps.move(row, next); QString groupError;
    int start=row, end=row, destination=next;
    if (!validateFlowItems(moved, groupError)) {
        // Crossing a category boundary moves complete categories. Inside a
        // category the ordinary one-step reorder above remains unchanged.
        const QString source=m_steps[row].testItemId;
        if (!source.isEmpty()) {
            while(start>0 && m_steps[start-1].testItemId==source)--start;
            while(end+1<m_steps.size() && m_steps[end+1].testItemId==source)++end;
        }
        const int adjacent=offset<0?start-1:end+1;
        if(adjacent<0 || adjacent>=m_steps.size())return;
        int edge=adjacent; const QString target=m_steps[adjacent].testItemId;
        if(!target.isEmpty()) {
            while(edge+offset>=0 && edge+offset<m_steps.size()
                  && m_steps[edge+offset].testItemId==target)edge+=offset;
        }
        destination=offset<0?edge:edge-(end-start);
    }
    QVector<int> order;
    for(int i=0;i<m_steps.size();++i)if(i<start || i>end)order.append(i);
    for(int i=start;i<=end;++i)order.insert(destination+i-start,i);
    moved=currentFlow();moved.steps.clear();
    for(int old:order)moved.steps.append(m_steps[old]);
    if(!validateFlowItems(moved,groupError)) {
        ui->statusLabel->setText("分類配置無法移動，請先調整測試項目的範圍。");return;
    }
    const bool wasLoading=m_loading;m_loading=true;
    QVector<QVector<QTableWidgetItem *>> cells;
    for(int i=0;i<m_steps.size();++i){
        QVector<QTableWidgetItem *> values;
        for(int column=0;column<ui->stepsTable->columnCount();++column)
            values.append(ui->stepsTable->takeItem(i,column));
        cells.append(values);
    }
    for(int i=0;i<order.size();++i)
        for(int column=0;column<ui->stepsTable->columnCount();++column)
            ui->stepsTable->setItem(i,column,cells[order[i]][column]);
    m_steps=moved.steps;m_loading=wasLoading;
    const int selected=order.indexOf(row);
    ui->stepsTable->selectRow(selected);showEditor(selected);
    ui->fileLabel->setText("流程已修改，請保存流程");
    ui->statusLabel->setText(std::abs(selected-row)>1 || end>start
        ? "已跨過相鄰分類；分類內的步驟順序、目標設定與結果已保留。"
        : "已移動所選步驟，請保存流程。");
}
bool FlowDialog::insertionSelfTest()
{
    TestFlow flow; flow.executable = QCoreApplication::applicationFilePath();
    FlowStep first; first.type = "clickUIA"; first.name = "既有按鈕"; first.selector = "Start";
    first.automationId = "OriginalStartId"; first.windowTitle = "Original main window"; first.region = QRect(11, 22, 3, 4);
    FlowStep last; last.type = "checkImage"; last.name = "既有預覽"; last.windowTitle = "Original preview";
    last.region = QRect(44, 55, 120, 90); last.condition = "contrast"; last.expected = "8";
    flow.steps = {first, last}; applyFlow(flow); m_lastValue = "64";
    ui->stepsTable->item(0, 2)->setText("PASS"); ui->stepsTable->item(0, 3)->setText("原本讀到 64");
    ui->stepsTable->item(1, 2)->setText("FAIL"); ui->stepsTable->item(1, 3)->setText("原本讀到 0");
    auto equal = [](const FlowStep &a, const FlowStep &b) {
        return a.type == b.type && a.name == b.name && a.selector == b.selector && a.automationId == b.automationId
            && a.windowTitle == b.windowTitle && a.region == b.region && a.text == b.text && a.condition == b.condition
            && a.expected == b.expected && a.timeoutMs == b.timeoutMs;
    };
    auto preserved = [&](int firstRow, int lastRow) {
        return equal(m_steps[firstRow], first) && equal(m_steps[lastRow], last) && m_lastValue == "64"
            && ui->stepsTable->item(firstRow, 2)->text() == "PASS" && ui->stepsTable->item(firstRow, 3)->text() == "原本讀到 64"
            && ui->stepsTable->item(lastRow, 2)->text() == "FAIL" && ui->stepsTable->item(lastRow, 3)->text() == "原本讀到 0";
    };
    FlowStep added; added.type = "delay"; added.name = "插入的新步驟";
    insertSteps(1, {added, added});
    if (!preserved(0, 3) || ui->stepsTable->item(1, 2)->text() != "未執行" || !ui->stepsTable->item(2, 3)->text().isEmpty()) return false;
    insertSteps(0, {added}); if (!preserved(1, 4)) return false;
    insertSteps(static_cast<int>(m_steps.size()), {added});
    if (!preserved(1, 4) || m_steps.size() != 6 || ui->stepsTable->item(5, 2)->text() != "未執行") return false;
    ui->stepsTable->selectRow(2); removeSelectedStep(); if (!preserved(1, 3)) return false;
    ui->stepsTable->selectRow(0); removeSelectedStep(); if (!preserved(0, 2)) return false;
    ui->stepsTable->selectRow(3); removeSelectedStep(); if (!preserved(0, 2)) return false;
    ui->stepsTable->selectRow(0); moveSelectedStep(1); if (!preserved(1, 2)) return false;
    ui->stepsTable->selectRow(2); moveSelectedStep(-1); if (!preserved(2, 1)) return false;
    moveSelectedStep(1);
    if (!preserved(1, 2) || m_steps.size()!=3) return false;
    TestFlow grouped;grouped.executable=QCoreApplication::applicationFilePath();grouped.name="工廠測試";grouped.version="2";
    grouped.testItems={{"g","設備檢查","保持接線","檢查中","檢查設備"}};
    FlowStep g;g.type="delay";g.name="內部步驟";g.testItemId="g";grouped.steps={g,g};
    applyFlow(grouped);insertSteps(1,{added});
    const auto after=currentFlow();
    if(after.name!=grouped.name || after.version!="2" || after.testItems.size()!=1
        || after.testItems[0].failureHint!="檢查設備" || after.steps[1].testItemId!="g")return false;
    TestFlow scan;scan.testItems={{"new","新增掃描項目","不需操作","等待","通知工程"}};
    FlowStep scanned;scanned.type="delay";scanned.name="掃描插入測試";scanned.testItemId="new";scan.steps={scanned};
    for(int index:{0,1,2}) {
        applyFlow(grouped);ui->stepsTable->item(0,2)->setText("PASS");insertScanDraft(index,scan);
        const auto inserted=currentFlow();QString error;
        if(inserted.steps.size()!=3 || inserted.steps[index].name!=scanned.name || !validateFlowItems(inserted,error))return false;
        if(inserted.steps[index].testItemId!=(index==1?QString("g"):QString("new")))return false;
        if(ui->stepsTable->item(index==0?1:0,2)->text()!="PASS")return false;
    }
    TestFlow categories;categories.executable=QCoreApplication::applicationFilePath();
    categories.testItems={{"a","分類 A",{},{},{}},{"b","分類 B",{},{},{}}};
    FlowStep free=added;free.name="獨立步驟";
    FlowStep a1=added;a1.name="A1";a1.testItemId="a";
    FlowStep a2=a1;a2.name="A2";
    FlowStep b1=added;b1.name="B1";b1.testItemId="b";
    FlowStep b2=b1;b2.name="B2";
    categories.steps={free,a1,a2,b1,b2};applyFlow(categories);
    setReadResult(0,"保留詳細資料\n原始資料");ui->stepsTable->item(0,2)->setText("PASS");
    ui->stepsTable->selectRow(0);moveSelectedStep(1);
    if(m_steps[2].name!="獨立步驟" || ui->stepsTable->currentRow()!=2
       || ui->stepsTable->item(2,2)->text()!="PASS"
       || ui->stepsTable->item(2,3)->data(Qt::UserRole).toString()!="保留詳細資料\n原始資料")return false;
    moveSelectedStep(-1);if(m_steps[0].name!="獨立步驟")return false;
    ui->stepsTable->selectRow(2);moveSelectedStep(1);
    QString problem;
    if(m_steps[1].name!="B1" || m_steps[2].name!="B2" || m_steps[3].name!="A1"
       || m_steps[4].name!="A2" || ui->stepsTable->currentRow()!=4
       || !validateFlowItems(currentFlow(),problem))return false;
    ui->stepsTable->selectRow(3);moveSelectedStep(-1);
    if(m_steps[1].name!="A1" || m_steps[2].name!="A2")return false;
    ui->stepsTable->selectRow(1);moveSelectedStep(1);
    if(m_steps[1].name!="A2" || m_steps[2].name!="A1")return false;
    return true;
}
TestFlow FlowDialog::currentFlow() const
{
    TestFlow flow; flow.executable = ui->exeEdit->text(); flow.arguments = ui->argsEdit->text(); flow.workingDirectory = ui->cwdEdit->text();
    flow.ocrExecutable = ui->ocrEdit->text(); flow.ocrLanguage = ui->languageEdit->text(); flow.reuseExisting = ui->reuseExistingCheck->isChecked(); flow.steps = m_steps;
    flow.name = m_flowName; flow.version = m_flowVersion; flow.testItems = m_testItems; return flow;
}
void FlowDialog::applyFlow(const TestFlow &flow)
{
    ui->exeEdit->setText(flow.executable); ui->argsEdit->setText(flow.arguments); ui->cwdEdit->setText(flow.workingDirectory);
    ui->ocrEdit->setText(flow.ocrExecutable); ui->languageEdit->setText(flow.ocrLanguage);
    ui->reuseExistingCheck->setChecked(flow.reuseExisting);
    m_flowName = flow.name; m_flowVersion = flow.version; m_testItems = flow.testItems;
    m_steps = flow.steps; updateTable(); if (!m_steps.isEmpty()) ui->stepsTable->selectRow(0); else showEditor(-1);
}
void FlowDialog::updateTable(bool preserveResults)
{
    QVector<QPair<QString, QString>> previous;
    if (preserveResults) {
        for (int row = 0; row < ui->stepsTable->rowCount(); ++row) {
            const auto *result = ui->stepsTable->item(row, 2), *detail = ui->stepsTable->item(row, 3);
            previous.append({result ? result->text() : "未執行", detail ? (detail->data(Qt::UserRole).toString().isEmpty()?detail->text():detail->data(Qt::UserRole).toString()) : QString()});
        }
    }
    const bool old = m_loading; m_loading = true; ui->stepsTable->setRowCount(static_cast<int>(m_steps.size()));
    for (int i = 0; i < m_steps.size(); ++i) {
        ui->stepsTable->setItem(i, 0, new QTableWidgetItem(flowTypeName(m_steps[i].type)));
        auto *name = new QTableWidgetItem(flowStepDisplayName(m_steps[i]));
        QString itemName = "未分組";
        for (const auto &item : m_testItems) if (item.id == m_steps[i].testItemId) name->setToolTip("現場測試項目：" + item.name);
        for (const auto &item : m_testItems) if (item.id == m_steps[i].testItemId) itemName = flowItemDisplayName(currentFlow(), item);
        ui->stepsTable->setItem(i, 1, name);
        ui->stepsTable->setItem(i, 5, new QTableWidgetItem(itemName));
        ui->stepsTable->setItem(i, 2, new QTableWidgetItem(i < previous.size() ? previous[i].first : "未執行"));
        auto *read=new QTableWidgetItem(i < previous.size() ? previous[i].second.section('\n',0,0) : QString());
        if(i<previous.size()){read->setData(Qt::UserRole,previous[i].second);read->setToolTip(previous[i].second);}
        ui->stepsTable->setItem(i, 3, read);
        auto *binding = new QTableWidgetItem(flowCheckSummary(m_steps[i]).isEmpty() ? bindingSummary(m_steps[i]) : "PASS：" + flowCheckSummary(m_steps[i])); binding->setToolTip(binding->text() + "\n" + bindingSummary(m_steps[i])); ui->stepsTable->setItem(i, 4, binding);
    }
    m_loading = old;
    if (!m_loading) showEditor(ui->stepsTable->currentRow());
    refreshReadDetails();
}
void FlowDialog::setReadResult(int row,const QString &detail)
{
    if(row<0 || row>=ui->stepsTable->rowCount())return;
    auto *cell=ui->stepsTable->item(row,3);if(!cell)return;
    cell->setData(Qt::UserRole,detail);cell->setToolTip(detail);cell->setText(detail.section('\n',0,0));
    if(ui->stepsTable->currentRow()==row)refreshReadDetails();
}
void FlowDialog::refreshReadDetails()
{
    const int row=ui->stepsTable->currentRow();const auto *cell=row>=0?ui->stepsTable->item(row,3):nullptr;
    const QString detail=cell?(cell->data(Qt::UserRole).toString().isEmpty()?cell->text():cell->data(Qt::UserRole).toString()):QString();
    m_readDetails->setVisible(!detail.isEmpty());
    if(m_readDetails->toPlainText()!=detail)m_readDetails->setPlainText(detail);
}
void FlowDialog::markStepForRetest(int row)
{
    if (row < 0 || row >= ui->stepsTable->rowCount()) return;
    ui->stepsTable->item(row, 2)->setText("待重測");
    setReadResult(row,"步驟設定已更新，請重新試跑此步驟。");
}
void FlowDialog::readEditor()
{
    if (m_loading) return; const int row = ui->stepsTable->currentRow(); if (row < 0 || row >= m_steps.size()) return;
    auto &s = m_steps[row];
    if (s.text != ui->textEdit->text()) s.valueLabel.clear();
    if (s.type != ui->typeCombo->currentData().toString() || s.selector != ui->selectorEdit->text()
        || s.automationId != ui->idEdit->text() || s.text != ui->textEdit->text()) m_lastValue = QString();
    s.type = ui->typeCombo->currentData().toString(); s.name = ui->nameEdit->text(); s.selector = ui->selectorEdit->text();
    s.automationId = ui->idEdit->text(); s.text = ui->textEdit->text(); s.condition = ui->conditionCombo->currentData().toString();
    s.expected = ui->expectedEdit->text(); s.timeoutMs = ui->timeoutSpin->value(); s.region = QRect(ui->xSpin->value(), ui->ySpin->value(), ui->widthSpin->value(), ui->heightSpin->value());
    ui->stepsTable->item(row, 0)->setText(flowTypeName(s.type)); ui->stepsTable->item(row, 1)->setText(flowStepDisplayName(s));
    if(auto *summary=ui->stepsTable->item(row,4))summary->setText(flowCheckSummary(s).isEmpty()?bindingSummary(s):"PASS："+flowCheckSummary(s));
    refreshFields(); refreshBinding();
}
void FlowDialog::showEditor(int row)
{
    if (m_loading) return; ui->stepGroup->setEnabled(!m_busy && row >= 0);
    if (row < 0 || row >= m_steps.size()) {
        ui->pickButton->setEnabled(false); refreshWindows(); return;
    }
    m_loading = true; const auto &s = m_steps[row]; ui->typeCombo->setCurrentIndex(ui->typeCombo->findData(s.type));
    ui->nameEdit->setText(s.name); ui->selectorEdit->setText(s.selector); ui->idEdit->setText(s.automationId); ui->textEdit->setText(s.text);
    ui->conditionCombo->setCurrentIndex(ui->conditionCombo->findData(s.condition)); ui->expectedEdit->setText(s.expected); ui->timeoutSpin->setValue(s.timeoutMs);
    ui->xSpin->setValue(s.region.x()); ui->ySpin->setValue(s.region.y()); ui->widthSpin->setValue(qMax(0, s.region.width())); ui->heightSpin->setValue(qMax(0, s.region.height())); m_loading = false;
    refreshFields(); refreshWindows(); refreshBinding();
}
void FlowDialog::refreshWindows()
{
    const int row = ui->stepsTable->currentRow();
    const QString selected = row >= 0 && row < m_steps.size() ? m_steps[row].windowTitle : QString();
    const bool wasLoading = m_loading; m_loading = true;
    ui->windowCombo->clear(); ui->windowCombo->addItem("自動（唯一主視窗）", QString());
    const auto windows = flowWindows(ui->exeEdit->text());
    for (const auto &window : windows)
        if (ui->windowCombo->findData(window.title) < 0) ui->windowCombo->addItem(window.title, window.title);
    int index = ui->windowCombo->findData(selected);
    if (index < 0) {ui->windowCombo->addItem("尚未開啟：" + selected, selected); index = ui->windowCombo->count() - 1;}
    ui->windowCombo->setCurrentIndex(index); m_loading = wasLoading;
    refreshBinding();
}
void FlowDialog::refreshBinding()
{
    const int row = ui->stepsTable->currentRow();
    if (row < 0 || row >= m_steps.size()) {ui->bindingLabel->setText("請選取步驟以查看目標設定。"); return;}
    const auto &step = m_steps[row]; QString summary = bindingSummary(step);
    if(step.type=="checkVideo" || step.type=="checkImage" || step.type=="readOCR")
        summary += "\n擷取："+QString(step.captureMode=="window"?"背景視窗（可遮住，不可最小化）":"前景桌面")
            +(step.type=="readOCR"?QString("；OCR 依辨識速度執行"):QString("；先擷取 %2 秒再分析，目標 %1 FPS，可由「影像擷取設定」修改").arg(step.sampleFps).arg(step.captureDurationMs/1000.0));
    if(step.type=="checkVideo" || step.type=="checkImage" || step.type=="readOCR")
        summary += step.captureMode=="desktop"?"；前景桌面相容模式":step.activateBeforeCapture?"；已啟用前景相容動作":"；背景執行，不主動切前景";
    ui->bindingLabel->setText("待測視窗：" + (step.windowTitle.isEmpty() ? "自動（唯一主視窗）" : step.windowTitle) + "\n" + summary);
    if (auto *item = ui->stepsTable->item(row, 4)) {item->setText(summary); item->setToolTip(ui->bindingLabel->text());}
}
void FlowDialog::refreshFields()
{
    const auto type = ui->typeCombo->currentData().toString();
    const bool uia = type == "clickUIA" || type == "readUIA", read = type == "readUIA" || type == "readOCR";
    const bool image = type == "checkImage" || type == "checkVideo";
    const bool coordinates = type == "clickPoint" || type == "readOCR" || image, condition = read || type == "assert" || image;
    ui->selectorEdit->setEnabled(uia); ui->idEdit->setEnabled(uia); ui->pickButton->setEnabled(uia);
    ui->textEdit->setEnabled(read || type == "typeText" || type == "hotkey");
    ui->previewNumberButton->setEnabled(read && !m_busy);
    ui->conditionCombo->setEnabled(condition); ui->expectedEdit->setEnabled(condition);
    ui->xSpin->setEnabled(coordinates); ui->ySpin->setEnabled(coordinates);
    ui->widthSpin->setEnabled(type == "readOCR" || image); ui->heightSpin->setEnabled(type == "readOCR" || image); ui->regionButton->setEnabled(!m_busy);
    ui->timeoutSpin->setEnabled(read || type == "waitWindow" || type == "delay");
    ui->stepGroup->setTitle(type=="checkVideo" ? "參考影片比較：框選完整影片內容（排除邊框／黑邊）；正常速度播放同一短片；由影像擷取設定指定秒數，完成後離線分析。" : image ? "影像檢查：請框選預覽內部、設定有內容／有變化及門檻；不需要 OCR。" : read ? "讀取設定：預期值留空只讀取；有預期值則等待符合。擷取正規式取第一個括號群組。" : "所選步驟設定（自動套用；保存流程才會寫入 JSON）");
}
void FlowDialog::setAdvanced(bool advanced)
{
    for(QWidget *widget:QList<QWidget *>{ui->windowLabel,ui->windowCombo,ui->refreshWindowsButton,
        ui->bindingLabel,ui->pickButton,ui->regionButton})widget->setVisible(advanced);
    ui->easyButton->setVisible(advanced);
    ui->stepGroup->setVisible(advanced); ui->addButton->setVisible(advanced); ui->stepsTable->setColumnHidden(0, !advanced);
    for (QWidget *widget : QList<QWidget *>{ui->argsLabel, ui->argsEdit, ui->cwdEdit, ui->ocrLabel, ui->ocrEdit, ui->browseOcrButton,
        ui->languageLabel, ui->languageEdit, ui->sampleCombo, ui->sampleSourceCombo, ui->sampleApplyCombo, ui->sampleButton, ui->sampleHintLabel}) widget->setVisible(advanced);
    ui->targetGroup->setTitle(advanced ? "待測 APP 與辨識設定" : "選擇待測 APP");
    ui->introLabel->setText(advanced ? "進階設定：手動取點／框選區域 → 單步試跑。此介面不會關機／重開機。"
        : "掃描 APP → 勾選項目與設定 PASS 條件 → 試跑並保存。手動取點與框選收在進階設定。");
}
void FlowDialog::setBusy(bool busy)
{
    m_busy = busy;
    if (auto *itemsButton = findChild<QPushButton *>("testItemsButton")) itemsButton->setEnabled(!busy);
    if (auto *scanButton = findChild<QPushButton *>("appScannerButton")) scanButton->setEnabled(!busy);
    if(auto *button=findChild<QPushButton *>("captureSettingsButton"))button->setEnabled(!busy);
    if(auto *button=findChild<QPushButton *>("reanalyzeButton"))button->setEnabled(!busy);
    ui->windowCombo->setEnabled(!busy); ui->refreshWindowsButton->setEnabled(!busy);
    ui->targetGroup->setEnabled(!busy); ui->stepGroup->setEnabled(!busy && ui->stepsTable->currentRow() >= 0); ui->stepsTable->setEnabled(!busy);
    for (auto *button : {ui->openButton, ui->saveButton, ui->insertBeforeButton, ui->insertAfterButton, ui->sampleButton, ui->addButton, ui->removeButton, ui->upButton, ui->downButton, ui->singleButton, ui->runButton}) button->setEnabled(!busy);
    ui->sampleCombo->setEnabled(!busy); ui->sampleApplyCombo->setEnabled(!busy); ui->sampleSourceCombo->setEnabled(!busy && ui->sampleCombo->currentIndex() < 6);
    ui->stopButton->setEnabled(busy && !m_picking);
    ui->easyButton->setEnabled(!busy); ui->advancedCheck->setEnabled(!busy);
    if (busy) {ui->pickButton->setEnabled(false); ui->regionButton->setEnabled(false);} else refreshFields();
}
void FlowDialog::log(const QString &line)
{
    const auto message = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz") + " | " + line;
    ui->logEdit->appendPlainText(message);
    if (!m_reportDirectory.isEmpty()) {
        QFile file(m_reportDirectory + "/flow.log");
        if (file.open(QIODevice::Append)) file.write(message.toUtf8() + '\n');
    }
}
void FlowDialog::previewNumber()
{
    const int row = ui->stepsTable->currentRow();
    if (m_busy || row < 0 || row >= m_steps.size()) return;
    auto flow = currentFlow();
    auto &step = flow.steps[row];
    if (step.type != "readUIA" && step.type != "readOCR") return;
    step.text.clear();
    step.expected.clear();
    QString error;
    if (!validateFlow(flow, error, row)) {
        QMessageBox::warning(this, "無法預覽讀取", error);
        return;
    }
    const auto temporary = std::make_shared<QTemporaryDir>();
    if (!temporary->isValid()) {
        QMessageBox::warning(this, "無法預覽讀取", "無法建立預覽暫存資料夾。");
        return;
    }
    auto *runner = new FlowRunner(this);
    runner->flow = flow;
    runner->singleStep = row;
    runner->evidenceDirectory = temporary->path();
    m_runner = runner;
    connect(runner, &FlowRunner::valueRead, runner, [runner](const QString &raw) {
        runner->setProperty("previewText", raw);
    });
    connect(runner, &FlowRunner::stepFinished, runner, [runner](int, const QString &result, const QString &detail) {
        if (result == "FAIL") runner->setProperty("previewError", detail);
    });
    connect(runner, &QThread::finished, this, [this, runner, temporary, row] {
        const auto raw = runner->property("previewText");
        const auto error = runner->property("previewError").toString();
        m_runner = nullptr;
        runner->deleteLater();
        setBusy(false);
        ui->statusLabel->setText("讀取預覽已結束");
        if (raw.isValid()) selectNumber(raw.toString(), row);
        else if (!error.isEmpty()) QMessageBox::warning(this, "無法預覽讀取", error);
    });
    setBusy(true);
    ui->statusLabel->setText("正在讀取原始文字…");
    if (step.type == "readOCR")
        requestFlowForeground(findFlowWindow(flow.executable, 0, step.windowTitle));
    runner->start();
}

void FlowDialog::selectNumber(const QString &raw, int row)
{
    QStringList labels, patterns;
    const auto fields = flowNumberFields(raw);
    for (const auto &field : fields) {labels << field.label; patterns << field.pattern;}

    QDialog dialog(this);
    dialog.setWindowTitle("讀取預覽與數字選取");
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(labels.isEmpty() ? "原始文字中找不到可選取的數字。" : "從原始文字選取要判定的數字。", &dialog));
    auto *preview = new QPlainTextEdit(raw, &dialog);
    preview->setReadOnly(true);
    layout->addWidget(preview);
    auto *choices = new QComboBox(&dialog);
    choices->setPlaceholderText("請選擇要讀取的欄位與數字");
    choices->addItems(labels);
    choices->setCurrentIndex(-1);
    layout->addWidget(choices);
    choices->setVisible(!labels.isEmpty());
    auto *buttons = new QDialogButtonBox(labels.isEmpty() ? QDialogButtonBox::Close
        : QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    if (auto *ok = buttons->button(QDialogButtonBox::Ok)) {
        ok->setEnabled(false);
        connect(choices, &QComboBox::currentIndexChanged, ok, [ok](int index) {
            ok->setEnabled(index >= 0);
        });
    }
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(620, 360);
    if (dialog.exec() != QDialog::Accepted || choices->currentIndex() < 0) return;

    const auto pattern = patterns[choices->currentIndex()];
    auto matches = QRegularExpression(pattern).globalMatch(raw);
    const auto match = matches.hasNext() ? matches.next() : QRegularExpressionMatch();
    if (!match.hasMatch() || matches.hasNext()) {
        QMessageBox::warning(this, "無法唯一識別數字", "多行內容符合相同規則，請縮小讀取區域或手動設定擷取正規式。");
        return;
    }
    if (ui->stepsTable->currentRow() != row) return;
    ui->textEdit->setText(pattern);
    readEditor();
    m_steps[row].valueLabel=fields[choices->currentIndex()].name;
    updateTable(true);
    markStepForRetest(row);
    ui->statusLabel->setText("擷取規則已更新，可單步試跑後保存 JSON。");
}

void FlowDialog::runFlow(bool single)
{
    if (m_busy) return;
    // Field changes already apply on edit; do not clear the last read value here.
    const auto flow = currentFlow(); QString error;
    const int row = ui->stepsTable->currentRow(); if (single && row < 0) return;
    if (!validateFlow(flow, error, single ? row : -1, single && !m_lastValue.isNull())) {QMessageBox::warning(this, "流程設定錯誤", error); return;}
    if (!single) m_lastValue = QString();
    for (int i = single ? row : 0; i < (single ? row + 1 : m_steps.size()); ++i) {
        ui->stepsTable->item(i, 2)->setText("未執行"); setReadResult(i,{});
    }
    m_reportDirectory = QCoreApplication::applicationDirPath() + "/flow_results/"
        + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!QDir().mkpath(m_reportDirectory) || !saveFlow(m_reportDirectory + "/flow.json", flow, error)) {QMessageBox::critical(this, "無法保存測試證據", error); return;}
    auto *runner = new FlowRunner(this); m_runner = runner; runner->flow = flow; runner->singleStep = single ? row : -1;
    runner->replayCaptureFile=m_replayCaptureFile;runner->initialValue = m_lastValue; runner->evidenceDirectory = m_reportDirectory;
    connect(runner, &FlowRunner::progress, this, &FlowDialog::log);
    connect(runner, &FlowRunner::stepStarted, this, [this](int index) {
        m_activeStep = index; m_stepElapsed.start(); m_waitTimer.start();
        ui->stepsTable->item(index, 2)->setText("執行中");
        setReadResult(index,"正在取得資料／等待條件符合…");
    });
    connect(runner, &FlowRunner::stepObserved, this, [this](int index, const QString &value) {
        setReadResult(index,value);
        if(index>=0 && index<m_steps.size() && (m_steps[index].type=="checkImage" || m_steps[index].type=="checkVideo"))ui->statusLabel->setText(value.section('\n',0,0));
    });
    connect(runner, &FlowRunner::windowResolved, this, [this](int index, const QString &title) {
        m_steps[index].windowTitle = title;
        if (ui->stepsTable->currentRow() == index) refreshWindows();
        ui->stepsTable->item(index, 4)->setToolTip("已依 AutomationId 重新定位至視窗：" + title);
        ui->fileLabel->setText("視窗設定已修正，請保存流程");
    });
    connect(runner, &FlowRunner::stepFinished, this, [this](int index, const QString &result, const QString &value) {
        m_waitTimer.stop();
        ui->stepsTable->item(index, 2)->setText(result); setReadResult(index,value);
    });
    connect(runner, &FlowRunner::valueRead, this, [this](const QString &value) {m_lastValue = value;});
    connect(runner, &FlowRunner::completed, this, [this](bool success, bool stopped) {
        m_waitTimer.stop(); m_activeStep = -1;
        const QString status = stopped ? "STOP" : success ? "PASS" : "FAIL";
        ui->statusLabel->setText(status + " — 紀錄：" + m_reportDirectory); log("流程結果：" + status);
        QFile result(m_reportDirectory + "/result.txt"); if (result.open(QIODevice::WriteOnly)) result.write(status.toUtf8());
    });
    connect(runner, &QThread::finished, this, [this, runner] {m_runner = nullptr; m_reportDirectory.clear(); setBusy(false); runner->deleteLater();});
    setBusy(true); ui->statusLabel->setText(single ? "單步試跑中" : "流程執行中"); log(single ? "單步試跑開始" : "完整流程開始");
    // Capture activation is performed by the shared runner per script step.
    runner->start();
}
void FlowDialog::sample()
{
    const int index = ui->sampleCombo->currentIndex();
    if (ui->sampleApplyCombo->currentIndex() == 1 || m_steps.isEmpty()) {
        FlowStep launch; launch.type = "launch"; launch.name = "啟動 APP";
        FlowStep wait; wait.type = "waitWindow"; wait.name = "等待主視窗"; wait.timeoutMs = 60000;
        m_steps = {launch, wait};
        updateTable();
    }
    insertSteps(static_cast<int>(m_steps.size()), flowSampleSteps(index, ui->sampleSourceCombo->currentIndex() == 1));
    log("已載入範例：" + ui->sampleCombo->currentText() + "。" + flowSampleHint(index));
    ui->statusLabel->setText("請取控制項／框選區域，必要時在前方加入 Start 點擊，再試跑。" );
}
void FlowDialog::reject()
{
    if (m_busy) {if (m_runner) m_runner->stop(); ui->statusLabel->setText("正在停止，結束後可關閉"); return;}
    QDialog::reject();
}
void FlowDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {if (m_runner) m_runner->stop(); event->ignore();} else QDialog::closeEvent(event);
}
