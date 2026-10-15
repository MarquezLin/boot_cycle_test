#include "flow_dialog.h"
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
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QStandardPaths>

namespace {
QString bindingSummary(const FlowStep &step)
{
    if (step.type == "clickUIA" || step.type == "readUIA") {
        if (step.selector.isEmpty() && step.automationId.isEmpty()) return "尚未指定控制項：請按「指一下文字／按鈕位置」。";
        if (step.selector == "@point") return QString("已指定位置 (%1, %2)；名稱與 AutomationId 未指定，依位置查找 UIA 控制項。").arg(step.region.x()).arg(step.region.y());
        return "已設定控制項；名稱：" + (step.selector.isEmpty() ? "未指定" : step.selector)
            + "；AutomationId：" + (step.automationId.isEmpty() ? "未指定" : step.automationId);
    }
    if (step.type == "readOCR" || step.type == "checkImage") {
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
    return fit && actual ? 0 : 1;
}

FlowDialog::FlowDialog(const QString &target, QWidget *parent) : QDialog(parent), ui(std::make_unique<Ui::FlowDialog>())
{
    ui->setupUi(this);
    ui->bootImportButton->hide();
    connect(ui->bootImportButton, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_bootImporter) return;
        readEditor();
        if (m_bootImporter(currentFlow())) {ui->statusLabel->setText("已匯入主要開關機測試。回主畫面按「試跑（不關機）」確認。"); log("目前流程已匯入主要開關機測試。");}
    });
    m_waitTimer.setInterval(250);
    connect(&m_waitTimer, &QTimer::timeout, this, [this] {
        if (m_activeStep < 0 || m_activeStep >= m_steps.size()) return;
        const auto &step = m_steps[m_activeStep];
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
    ui->exeEdit->setText(target);
    for (auto *edit : {ui->exeEdit, ui->argsEdit, ui->cwdEdit, ui->ocrEdit, ui->languageEdit})
        connect(edit, &QLineEdit::textEdited, this, [this] {m_lastValue = QString();});
    auto browse = [this](QLineEdit *edit) {
        const auto path = QFileDialog::getOpenFileName(this, "選擇 EXE", edit->text(), "執行檔 (*.exe)");
        if (!path.isEmpty()) edit->setText(path);
    };
    connect(ui->browseExeButton, &QPushButton::clicked, this, [=] {browse(ui->exeEdit);});
    connect(ui->browseOcrButton, &QPushButton::clicked, this, [=] {browse(ui->ocrEdit);});
    connect(ui->addButton, &QPushButton::clicked, this, [this] {readEditor(); FlowStep step; step.name = "新步驟"; insertSteps(static_cast<int>(m_steps.size()), {step});});
    connect(ui->removeButton, &QPushButton::clicked, this, &FlowDialog::removeSelectedStep);
    connect(ui->upButton, &QPushButton::clicked, this, [this] {moveSelectedStep(-1);});
    connect(ui->downButton, &QPushButton::clicked, this, [this] {moveSelectedStep(1);});
    connect(ui->stepsTable, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) {showEditor(row);});
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
    connect(ui->runButton, &QPushButton::clicked, this, [this] {runFlow(false);});
    connect(ui->stopButton, &QPushButton::clicked, this, [this] {m_waitTimer.stop(); if (m_runner) m_runner->stop(); ui->statusLabel->setText("正在停止，等待目前操作返回…");});
    connect(ui->regionButton, &QPushButton::clicked, this, [this] {
        const int selected = ui->stepsTable->currentRow();
        const auto type = selected >= 0 ? m_steps[selected].type : QString();
        const bool addText = type != "readUIA" && type != "readOCR" && type != "checkImage" && type != "clickPoint";
        const QString title = selected >= 0 ? m_steps[selected].windowTitle : QString();
        const auto window = findFlowWindow(ui->exeEdit->text(), 0, title); QString error;
        if (!window) {QMessageBox::warning(this, "請選擇待測視窗", flowWindowError(ui->exeEdit->text(), 0, title)); refreshWindows(); return;}
        const auto image = captureFlowWindow(window, error);
        if (image.isNull()) {QMessageBox::warning(this, "無法擷取", error); return;}
        QDialog dialog(this); Ui::RegionDialog regionUi; setupRegionDialog(dialog, regionUi, image);
        auto *canvas = static_cast<RegionCanvas *>(regionUi.imageScroll->widget());
        if (dialog.exec() != QDialog::Accepted) return;
        const auto rect = canvas->selected();
        if (!rect.isValid()) {QMessageBox::warning(this, "尚未框選", "請拖曳框選有效區域"); return;}
        if (type != "clickPoint" && type != "checkImage") {
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
            if (ui->typeCombo->currentData() != "checkImage") ui->typeCombo->setCurrentIndex(ui->typeCombo->findData("readOCR"));
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
    const auto saved = settings.value("Flow/LastFile").toString(); TestFlow loaded; QString error;
    if (!saved.isEmpty() && loadFlow(saved, loaded, error)) {applyFlow(loaded); m_file = saved; ui->fileLabel->setText(QFileInfo(saved).fileName());}
    else {
        FlowStep launch; launch.type = "launch"; launch.name = "啟動待測 APP";
        FlowStep wait; wait.type = "waitWindow"; wait.name = "等待主視窗";
        m_steps = {launch, wait}; updateTable(); ui->stepsTable->selectRow(0);
    }
    setAdvanced(false);
}
FlowDialog::~FlowDialog() {if (m_runner) {m_runner->stop(); m_runner->wait();}}
QString FlowDialog::sourceFileName() const
{
    return m_file.isEmpty() ? m_importName : QFileInfo(m_file).fileName();
}

void FlowDialog::setBootImporter(std::function<bool(const TestFlow &)> importer)
{
    m_bootImporter = std::move(importer); ui->bootImportButton->setVisible(bool(m_bootImporter));
}
void FlowDialog::setEditingCopy(const TestFlow &flow, const QString &sourceName)
{
    applyFlow(flow); m_file.clear(); m_lastValue = QString(); m_importName = sourceName;
    ui->fileLabel->setText("匯入流程的編輯副本；修改後請重新匯入");
}
void FlowDialog::buildInsertion(bool after)
{
    if (m_busy) return;
    const int row = ui->stepsTable->currentRow();
    const int index = row < 0 ? static_cast<int>(m_steps.size()) : row + (after ? 1 : 0);
    EasyBuilder builder(this, true);
    if (builder.exec() != QDialog::Accepted) return;
    readEditor(); insertSteps(index, easyPlanSteps(builder.plan()));
}
void FlowDialog::insertSteps(int index, const QVector<FlowStep> &steps)
{
    if (steps.isEmpty() || index < 0 || index > m_steps.size()) return;
    const bool wasLoading = m_loading; m_loading = true;
    for (int offset = 0; offset < steps.size(); ++offset) {
        m_steps.insert(index + offset, steps[offset]);
        // Shift existing table cells along with their step instead of resetting results.
        ui->stepsTable->insertRow(index + offset);
    }
    updateTable(true); m_loading = wasLoading;
    ui->stepsTable->selectRow(index); showEditor(index);
    ui->statusLabel->setText(QString("已插入 %1 個步驟；原有目標設定與測試結果已保留。請設定新步驟的目標並單步試跑。").arg(steps.size()));
    ui->fileLabel->setText("流程已修改，請保存流程");
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
    if (m_busy) return;
    readEditor(); const int row = ui->stepsTable->currentRow(), next = row + offset;
    if (row < 0 || next < 0 || next >= m_steps.size()) return;
    const bool wasLoading = m_loading; m_loading = true;
    m_steps.swapItemsAt(row, next);
    for (int column = 0; column < ui->stepsTable->columnCount(); ++column) {
        auto *current = ui->stepsTable->takeItem(row, column);
        auto *adjacent = ui->stepsTable->takeItem(next, column);
        ui->stepsTable->setItem(row, column, adjacent); ui->stepsTable->setItem(next, column, current);
    }
    m_loading = wasLoading;
    ui->stepsTable->selectRow(next); showEditor(next);
    ui->fileLabel->setText("流程已修改，請保存流程");
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
    return preserved(1, 2) && m_steps.size() == 3;
}
TestFlow FlowDialog::currentFlow() const
{
    TestFlow flow; flow.executable = ui->exeEdit->text(); flow.arguments = ui->argsEdit->text(); flow.workingDirectory = ui->cwdEdit->text();
    flow.ocrExecutable = ui->ocrEdit->text(); flow.ocrLanguage = ui->languageEdit->text(); flow.reuseExisting = ui->reuseExistingCheck->isChecked(); flow.steps = m_steps; return flow;
}
void FlowDialog::applyFlow(const TestFlow &flow)
{
    ui->exeEdit->setText(flow.executable); ui->argsEdit->setText(flow.arguments); ui->cwdEdit->setText(flow.workingDirectory);
    ui->ocrEdit->setText(flow.ocrExecutable); ui->languageEdit->setText(flow.ocrLanguage);
    ui->reuseExistingCheck->setChecked(flow.reuseExisting);
    m_steps = flow.steps; updateTable(); if (!m_steps.isEmpty()) ui->stepsTable->selectRow(0); else showEditor(-1);
}
void FlowDialog::updateTable(bool preserveResults)
{
    QVector<QPair<QString, QString>> previous;
    if (preserveResults) {
        for (int row = 0; row < ui->stepsTable->rowCount(); ++row) {
            const auto *result = ui->stepsTable->item(row, 2), *detail = ui->stepsTable->item(row, 3);
            previous.append({result ? result->text() : "未執行", detail ? detail->text() : QString()});
        }
    }
    const bool old = m_loading; m_loading = true; ui->stepsTable->setRowCount(static_cast<int>(m_steps.size()));
    for (int i = 0; i < m_steps.size(); ++i) {
        ui->stepsTable->setItem(i, 0, new QTableWidgetItem(flowTypeName(m_steps[i].type)));
        ui->stepsTable->setItem(i, 1, new QTableWidgetItem(m_steps[i].name));
        ui->stepsTable->setItem(i, 2, new QTableWidgetItem(i < previous.size() ? previous[i].first : "未執行"));
        ui->stepsTable->setItem(i, 3, new QTableWidgetItem(i < previous.size() ? previous[i].second : QString()));
        auto *binding = new QTableWidgetItem(bindingSummary(m_steps[i])); binding->setToolTip(binding->text()); ui->stepsTable->setItem(i, 4, binding);
    }
    m_loading = old;
    if (!m_loading) showEditor(ui->stepsTable->currentRow());
}
void FlowDialog::markStepForRetest(int row)
{
    if (row < 0 || row >= ui->stepsTable->rowCount()) return;
    ui->stepsTable->item(row, 2)->setText("待重測");
    ui->stepsTable->item(row, 3)->setText("框選區域或判定已更新，請重新試跑此步驟。");
}
void FlowDialog::readEditor()
{
    if (m_loading) return; const int row = ui->stepsTable->currentRow(); if (row < 0 || row >= m_steps.size()) return;
    auto &s = m_steps[row];
    if (s.type != ui->typeCombo->currentData().toString() || s.selector != ui->selectorEdit->text()
        || s.automationId != ui->idEdit->text() || s.text != ui->textEdit->text()) m_lastValue = QString();
    s.type = ui->typeCombo->currentData().toString(); s.name = ui->nameEdit->text(); s.selector = ui->selectorEdit->text();
    s.automationId = ui->idEdit->text(); s.text = ui->textEdit->text(); s.condition = ui->conditionCombo->currentData().toString();
    s.expected = ui->expectedEdit->text(); s.timeoutMs = ui->timeoutSpin->value(); s.region = QRect(ui->xSpin->value(), ui->ySpin->value(), ui->widthSpin->value(), ui->heightSpin->value());
    ui->stepsTable->item(row, 0)->setText(flowTypeName(s.type)); ui->stepsTable->item(row, 1)->setText(s.name);
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
    const auto &step = m_steps[row]; const auto summary = bindingSummary(step);
    ui->bindingLabel->setText("待測視窗：" + (step.windowTitle.isEmpty() ? "自動（唯一主視窗）" : step.windowTitle) + "\n" + summary);
    if (auto *item = ui->stepsTable->item(row, 4)) {item->setText(summary); item->setToolTip(ui->bindingLabel->text());}
}
void FlowDialog::refreshFields()
{
    const auto type = ui->typeCombo->currentData().toString();
    const bool uia = type == "clickUIA" || type == "readUIA", read = type == "readUIA" || type == "readOCR";
    const bool image = type == "checkImage";
    const bool coordinates = type == "clickPoint" || type == "readOCR" || image, condition = read || type == "assert" || image;
    ui->selectorEdit->setEnabled(uia); ui->idEdit->setEnabled(uia); ui->pickButton->setEnabled(uia);
    ui->textEdit->setEnabled(read || type == "typeText" || type == "hotkey");
    ui->conditionCombo->setEnabled(condition); ui->expectedEdit->setEnabled(condition);
    ui->xSpin->setEnabled(coordinates); ui->ySpin->setEnabled(coordinates);
    ui->widthSpin->setEnabled(type == "readOCR" || image); ui->heightSpin->setEnabled(type == "readOCR" || image); ui->regionButton->setEnabled(!m_busy);
    ui->timeoutSpin->setEnabled(read || image || type == "waitWindow" || type == "delay");
    ui->stepGroup->setTitle(image ? "影像檢查：請框選預覽內部、設定有內容／有變化及門檻；不需要 OCR。" : read ? "讀取設定：預期值留空只讀取；有預期值則等待符合。擷取正規式取第一個括號群組。" : "所選步驟設定（自動套用；保存流程才會寫入 JSON）");
}
void FlowDialog::setAdvanced(bool advanced)
{
    ui->stepGroup->setVisible(advanced); ui->addButton->setVisible(advanced); ui->stepsTable->setColumnHidden(0, !advanced);
    for (QWidget *widget : QList<QWidget *>{ui->argsLabel, ui->argsEdit, ui->cwdEdit, ui->ocrLabel, ui->ocrEdit, ui->browseOcrButton,
        ui->languageLabel, ui->languageEdit, ui->sampleCombo, ui->sampleSourceCombo, ui->sampleApplyCombo, ui->sampleButton, ui->sampleHintLabel}) widget->setVisible(advanced);
    ui->targetGroup->setTitle(advanced ? "待測 APP 與辨識設定" : "選擇待測 APP");
    ui->introLabel->setText("建立檢查 → 指一下文字位置或框選預覽 → 單步試跑。此介面不會關機／重開機。" );
}
void FlowDialog::setBusy(bool busy)
{
    m_busy = busy;
    ui->windowCombo->setEnabled(!busy); ui->refreshWindowsButton->setEnabled(!busy);
    ui->targetGroup->setEnabled(!busy); ui->stepGroup->setEnabled(!busy && ui->stepsTable->currentRow() >= 0); ui->stepsTable->setEnabled(!busy);
    for (auto *button : {ui->openButton, ui->saveButton, ui->bootImportButton, ui->insertBeforeButton, ui->insertAfterButton, ui->sampleButton, ui->addButton, ui->removeButton, ui->upButton, ui->downButton, ui->singleButton, ui->runButton}) button->setEnabled(!busy);
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
void FlowDialog::runFlow(bool single)
{
    if (m_busy) return;
    // Field changes already apply on edit; do not clear the last read value here.
    const auto flow = currentFlow(); QString error;
    const int row = ui->stepsTable->currentRow(); if (single && row < 0) return;
    if (!validateFlow(flow, error, single ? row : -1, single && !m_lastValue.isNull())) {QMessageBox::warning(this, "流程設定錯誤", error); return;}
    if (!single) m_lastValue = QString();
    for (int i = single ? row : 0; i < (single ? row + 1 : m_steps.size()); ++i) {
        ui->stepsTable->item(i, 2)->setText("未執行"); ui->stepsTable->item(i, 3)->setText({});
    }
    m_reportDirectory = QCoreApplication::applicationDirPath() + "/flow_results/"
        + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz") + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8);
    if (!QDir().mkpath(m_reportDirectory) || !saveFlow(m_reportDirectory + "/flow.json", flow, error)) {QMessageBox::critical(this, "無法保存測試證據", error); return;}
    auto *runner = new FlowRunner(this); m_runner = runner; runner->flow = flow; runner->singleStep = single ? row : -1;
    runner->initialValue = m_lastValue; runner->evidenceDirectory = m_reportDirectory;
    connect(runner, &FlowRunner::progress, this, &FlowDialog::log);
    connect(runner, &FlowRunner::stepStarted, this, [this](int index) {
        m_activeStep = index; m_stepElapsed.start(); m_waitTimer.start();
        ui->stepsTable->item(index, 2)->setText("執行中");
        ui->stepsTable->item(index, 3)->setText("正在取得資料／等待條件符合…");
    });
    connect(runner, &FlowRunner::stepObserved, this, [this](int index, const QString &value) {
        ui->stepsTable->item(index, 3)->setText(value);
    });
    connect(runner, &FlowRunner::windowResolved, this, [this](int index, const QString &title) {
        m_steps[index].windowTitle = title;
        if (ui->stepsTable->currentRow() == index) refreshWindows();
        ui->stepsTable->item(index, 4)->setToolTip("已依 AutomationId 重新定位至視窗：" + title);
        ui->fileLabel->setText("視窗設定已修正，請保存流程");
    });
    connect(runner, &FlowRunner::stepFinished, this, [this](int index, const QString &result, const QString &value) {
        m_waitTimer.stop();
        ui->stepsTable->item(index, 2)->setText(result); ui->stepsTable->item(index, 3)->setText(value);
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
    // Transfer focus while handling the user's button click on the UI thread.
    requestFlowForeground(findFlowWindow(flow.executable, 0, single ? m_steps[row].windowTitle : QString()));
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
