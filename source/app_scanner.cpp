#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include "app_scanner.h"
#include "flow_runner.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QUuid>
#include <QVBoxLayout>

namespace {
struct Release {template<class T> void operator()(T *p) const {if(p)p->Release();}};
template<class T>using Com=std::unique_ptr<T,Release>;
QString cachedText(IUIAutomationElement *element,PROPERTYID property) {
    VARIANT value;VariantInit(&value);QString text;
    if(SUCCEEDED(element->GetCachedPropertyValue(property,&value)) && value.vt==VT_BSTR)
        text=QString::fromWCharArray(value.bstrVal?value.bstrVal:L"");
    VariantClear(&value);return text;
}
bool cachedBool(IUIAutomationElement *element,PROPERTYID property,bool fallback=false) {
    VARIANT value;VariantInit(&value);bool result=fallback;
    if(SUCCEEDED(element->GetCachedPropertyValue(property,&value)) && value.vt==VT_BOOL)result=value.boolVal==VARIANT_TRUE;
    VariantClear(&value);return result;
}
int cachedInt(IUIAutomationElement *element,PROPERTYID property) {
    VARIANT value;VariantInit(&value);int result=0;
    if(SUCCEEDED(element->GetCachedPropertyValue(property,&value)) && value.vt==VT_I4)result=value.lVal;
    VariantClear(&value);return result;
}
void classifyControl(AppControl &control,int type,bool password,bool invokeAvailable) {
    control.controlType=type;
    control.readable=!password && (type==UIA_TextControlTypeId || type==UIA_EditControlTypeId || type==UIA_DocumentControlTypeId);
    // Invoke is a capability, not a semantic button type. Some providers expose
    // it on text controls too; never turn those into click candidates.
    control.invoke=!password && !control.readable && type==UIA_ButtonControlTypeId && invokeAvailable;
}
void resolveBindings(QVector<AppControl> &controls) {
    for(auto &c:controls) {
        if(!c.invoke && !c.readable)continue;
        int ids=0,names=0,pairs=0;
        for(const auto &other:controls) {
            if(!c.id.isEmpty() && c.id==other.id)++ids;
            if(!c.name.isEmpty() && c.name==other.name)++names;
            if(!c.id.isEmpty() && c.id==other.id && !c.name.isEmpty() && c.name==other.name)++pairs;
        }
        if(ids==1) {c.unique=true;c.selector.clear();}
        else if(c.invoke && ((!c.id.isEmpty() && pairs==1) || (c.id.isEmpty() && names==1))) {
            c.unique=true;c.selector=c.name.startsWith("regex:")?"regex:^"+QRegularExpression::escape(c.name)+'$':c.name;
        } else {c.unique=false;c.reason=c.readable?"缺少唯一 ID，請用手動取點／OCR 設定文字檢查":"名稱或 ID 重複，無法唯一定位；請手動指定";}
        if(c.readable && ids!=1 && !c.invoke)c.unique=false;
    }
}
bool appendChoice(TestFlow &flow,const AppControl &c,bool click,const QString &title,const QString &expected,
                  const QString &instruction,const QString &running,const QString &failure,QString &error,
                  const QString &condition="contains",const QString &extraction={}) {
    if(!c.unique || (click?!c.invoke:!c.readable)) {error="控制項無法唯一定位或不支援所選操作，請重新掃描。";return false;}
    if(title.trimmed().isEmpty()) {error="請填寫工廠看得懂的測試項目名稱。";return false;}
    const bool numeric=condition!="contains" && condition!="equals";
    if(!click && expected.trimmed().isEmpty()) {error=numeric?"請填寫數值門檻。":"請填寫預期文字，或改選數值判定並指定數字欄位。";return false;}
    if(!click && numeric) {
        QString parsed,problem;evaluateFlowValue("0",condition,expected,parsed,problem);
        if(!problem.isEmpty()){error=problem;return false;}
        if(extraction.isEmpty()){error="請先從「要判定的數值」選擇欄位，不能直接取整段文字的第一個數字。";return false;}
        const QRegularExpression regex(extraction);auto matches=regex.globalMatch(c.value);
        if(!regex.isValid() || regex.captureCount()!=1 || !matches.hasNext()){error="所選數字無法從掃描文字讀取，請重新選取。";return false;}
        matches.next();if(matches.hasNext()){error="所選數字符合多個位置，請縮小讀取範圍。";return false;}
    }
    const QString id=QUuid::createUuid().toString(QUuid::Id128);
    flow.testItems.append({id,title.trimmed(),instruction,running,failure});
    FlowStep step;step.type=click?"clickUIA":"readUIA";step.name=title.trimmed();step.automationId=c.id;step.selector=c.selector;
    step.windowTitle=c.windowTitle;step.timeoutMs=click?10000:30000;step.testItemId=id;
    if(!click){step.condition=condition;step.expected=expected;if(numeric)step.text=extraction;}
    if(!click && numeric)for(const auto &field:flowNumberFields(c.value))if(field.pattern==extraction)step.valueLabel=field.name;
    if(flowAutoName(step.name)){step.name=flowStepDisplayName(step);flow.testItems.last().name=step.name;}
    flow.steps.append(step);return true;
}
}

AppScanResult scanAppControls(quintptr handle,const std::atomic_bool &cancel) {
    AppScanResult result;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(initialized)){result.error="無法初始化掃描執行緒 COM";return result;}
    struct Uninit{~Uninit(){CoUninitialize();}} uninit;
    const HWND window=reinterpret_cast<HWND>(handle);
    if(!IsWindow(window)){result.error="待測視窗已關閉，請重新整理。";return result;}
    IUIAutomation *raw=nullptr;
    if(FAILED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)))){result.error="無法建立 UI Automation";return result;}
    Com<IUIAutomation> automation(raw);
    IUIAutomation2 *raw2=nullptr;
    if(SUCCEEDED(automation->QueryInterface(IID_PPV_ARGS(&raw2)))) {Com<IUIAutomation2> settings(raw2);settings->put_ConnectionTimeout(1500);settings->put_TransactionTimeout(1500);}
    IUIAutomationElement *rawRoot=nullptr;IUIAutomationCacheRequest *rawCache=nullptr;IUIAutomationCondition *rawCondition=nullptr;
    if(FAILED(automation->ElementFromHandle(window,&rawRoot))){result.error="無法讀取視窗；請確認權限與 APP 是否支援 UIA。";return result;}
    Com<IUIAutomationElement> root(rawRoot);
    if(FAILED(automation->CreateCacheRequest(&rawCache))){result.error="無法建立控制項快取";return result;}
    Com<IUIAutomationCacheRequest> cache(rawCache);
    for(PROPERTYID property:{UIA_NamePropertyId,UIA_AutomationIdPropertyId,UIA_ControlTypePropertyId,UIA_IsEnabledPropertyId,
        UIA_IsPasswordPropertyId,UIA_IsInvokePatternAvailablePropertyId,UIA_ValueValuePropertyId})cache->AddProperty(property);
    cache->put_TreeScope(TreeScope_Element);
    if(FAILED(automation->CreateTrueCondition(&rawCondition))){result.error="無法建立掃描條件";return result;}
    Com<IUIAutomationCondition> condition(rawCondition);IUIAutomationElementArray *rawElements=nullptr;
    if(FAILED(root->FindAllBuildCache(TreeScope_Subtree,condition.get(),cache.get(),&rawElements))){result.error="控制項掃描失敗；可改用手動取點或 OCR。";return result;}
    Com<IUIAutomationElementArray> elements(rawElements);int count=0;if(elements)elements->get_Length(&count);
    if(count>1500){result.error="視窗超過 1500 個控制項，請選較小的子視窗或使用手動設定；不會以不完整掃描建立草稿。";return result;}
    wchar_t title[2048]{};GetWindowTextW(window,title,2048);const QString windowTitle=QString::fromWCharArray(title);
    QElapsedTimer timer;timer.start();
    for(int i=0;i<count;++i) {
        if(cancel.load()){result.error="已取消掃描";return result;}
        if(timer.elapsed()>15000){result.error="掃描逾時，未建立候選；請縮小視窗或使用手動設定。";return result;}
        IUIAutomationElement *elementRaw=nullptr;if(FAILED(elements->GetElement(i,&elementRaw)))continue;
        Com<IUIAutomationElement> element(elementRaw);if(!element)continue;++result.examined;
        AppControl c;c.name=cachedText(element.get(),UIA_NamePropertyId);c.id=cachedText(element.get(),UIA_AutomationIdPropertyId);c.windowTitle=windowTitle;
        // Retain non-candidate elements for accurate duplicate-name/ID detection.
        const bool password=cachedBool(element.get(),UIA_IsPasswordPropertyId);
        const int type=cachedInt(element.get(),UIA_ControlTypePropertyId);
        classifyControl(c,type,password,cachedBool(element.get(),UIA_IsInvokePatternAvailablePropertyId));
        c.enabled=cachedBool(element.get(),UIA_IsEnabledPropertyId,true);
        if(!password) {c.value=cachedText(element.get(),UIA_ValueValuePropertyId);if(c.value.isEmpty())c.value=c.name;}
        else c.name="受保護輸入欄位";
        result.controls.append(c);
    }
    resolveBindings(result.controls);return result;
}

AppScannerDialog::AppScannerDialog(const TestFlow &base,QWidget *parent):QDialog(parent),m_base(base),m_cancel(std::make_shared<std::atomic_bool>(false)) {
    setWindowTitle("掃描 APP，勾選建立測試腳本");resize(1100,800);setMinimumSize(850,650);
    auto *layout=new QVBoxLayout(this);auto *intro=new QLabel("1 選擇已開啟的待測視窗並掃描 → 2 勾選、調整順序及確認預期文字 → 3 加入編輯器試跑。\n掃描只讀取控制項；產生草稿也不會點擊按鈕。不需 AI 帳號。");intro->setWordWrap(true);layout->addWidget(intro);
    auto *row=new QHBoxLayout;m_windows=new QComboBox;m_scan=new QPushButton("掃描視窗");m_refresh=new QPushButton("重新整理");
    row->addWidget(m_windows,1);row->addWidget(m_refresh);row->addWidget(m_scan);layout->addLayout(row);
    m_status=new QLabel("請先在編輯器開啟待測 APP（F6），再選擇視窗。");m_status->setWordWrap(true);layout->addWidget(m_status);
    m_table=new QTableWidget(0,4);m_table->setHorizontalHeaderLabels({"選用／建議測試項目","操作類型","目前讀到（不是通過規格）","定位狀態"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);m_table->setSelectionMode(QAbstractItemView::SingleSelection);m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);m_table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2,QHeaderView::Stretch);m_table->horizontalHeader()->setSectionResizeMode(3,QHeaderView::ResizeToContents);layout->addWidget(m_table,1);
    auto *order=new QHBoxLayout;auto *up=new QPushButton("所選項目上移");auto *down=new QPushButton("所選項目下移");order->addWidget(up);order->addWidget(down);order->addStretch();layout->addLayout(order);
    auto *form=new QFormLayout;m_title=new QLineEdit;m_expected=new QLineEdit;m_expected->setPlaceholderText("請填寫應出現的文字；會等待 30 秒內包含此文字");
    form->addRow("現場測試項目名稱",m_title);
    m_condition=new QComboBox;
    for(const auto &pair:QList<QPair<QString,QString>>{{"文字包含","contains"},{"文字等於","equals"},{"數值大於 >","gt"},{"數值大於等於 ≥","ge"},{"數值小於 <","lt"},{"數值小於等於 ≤","le"},{"數值等於 =","numberEquals"},{"數值範圍（含邊界）","range"}})m_condition->addItem(pair.first,pair.second);
    form->addRow("PASS 判定方式",m_condition);m_number=new QComboBox;m_number->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_number->setMinimumContentsLength(25);
    m_numberLabel=new QLabel("要判定的數值");form->addRow(m_numberLabel,m_number);
    m_expectedLabel=new QLabel("預期文字");form->addRow(m_expectedLabel,m_expected);
    m_preview=new QLabel;m_preview->setTextFormat(Qt::PlainText);m_preview->setWordWrap(true);form->addRow(m_preview);
    m_instruction=new QPlainTextEdit;m_instruction->setMaximumHeight(60);m_failure=new QPlainTextEdit;m_failure->setMaximumHeight(60);
    form->addRow("操作者說明",m_instruction);form->addRow("失敗排查提示",m_failure);layout->addLayout(form);
    m_binding=new QLabel;m_binding->setWordWrap(true);m_binding->setTextFormat(Qt::PlainText);layout->addWidget(m_binding);
    auto *position=new QFormLayout;m_position=new QComboBox;m_position->setObjectName("scanInsertionPosition");
    m_position->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);m_position->setMinimumContentsLength(30);
    for(int i=0;i<m_base.steps.size();++i) {
        const bool inside=i>0 && !m_base.steps[i].testItemId.isEmpty() && m_base.steps[i-1].testItemId==m_base.steps[i].testItemId;
        m_position->addItem(QString("第 %1 步之前：%2%3").arg(i+1).arg(flowStepDisplayName(m_base.steps[i]),inside?QString("（併入原測試項目）"):QString()),i);
    }
    m_position->addItem("流程最後",int(m_base.steps.size()));m_position->setCurrentIndex(m_position->count()-1);
    position->addRow("插入到既有流程的位置",m_position);layout->addLayout(position);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Cancel);m_generate=buttons->addButton("插入腳本草稿",QDialogButtonBox::AcceptRole);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&AppScannerDialog::reject);connect(m_generate,&QPushButton::clicked,this,[this]{generate();});
    connect(m_refresh,&QPushButton::clicked,this,[this]{refreshWindows();});connect(m_scan,&QPushButton::clicked,this,[this]{scan();});
    connect(m_table,&QTableWidget::currentCellChanged,this,[this](int index,int,int,int){store();select(index);});
    connect(m_table,&QTableWidget::itemChanged,this,[this](QTableWidgetItem *item){if(!m_loading && item->column()==0 && item->row()<m_choices.size())m_choices[item->row()].checked=item->checkState()==Qt::Checked;});
    connect(up,&QPushButton::clicked,this,[this]{move(-1);});connect(down,&QPushButton::clicked,this,[this]{move(1);});
    connect(m_condition,&QComboBox::currentIndexChanged,this,[this]{if(!m_loading){store();select(m_selected);}});
    connect(m_number,&QComboBox::currentIndexChanged,this,[this]{if(!m_loading){store();updatePreview();}});
    connect(m_expected,&QLineEdit::textEdited,this,[this]{store();updatePreview();});
    refreshWindows();select(-1);
}
AppScannerDialog::~AppScannerDialog(){m_cancel->store(true);if(m_worker)m_worker->wait();}
int AppScannerDialog::insertionIndex() const {return m_position->currentData().toInt();}
void AppScannerDialog::refreshWindows() {
    const auto selected=m_windows->currentData();m_windows->clear();
    for(const auto &w:flowWindows(m_base.executable))m_windows->addItem(w.title,QVariant::fromValue<qulonglong>(w.handle));
    if(m_windows->findData(selected)>=0)m_windows->setCurrentIndex(m_windows->findData(selected));
    m_scan->setEnabled(m_windows->count()>0);m_generate->setEnabled(!m_choices.isEmpty());
    if(!m_windows->count())m_status->setText("找不到待測 APP 視窗。請先回編輯器按 F6 開啟 APP，再重新整理。");
}
void AppScannerDialog::scan() {
    if(m_busy)return;const quintptr handle=quintptr(m_windows->currentData().toULongLong());
    if(!handle || findFlowWindow(m_base.executable,0,m_windows->currentText())!=handle) {
        m_status->setText("視窗不存在或同名視窗不唯一；請關閉多餘視窗再重新整理。");return;
    }
    m_busy=true;m_cancel->store(false);m_scan->setEnabled(false);m_refresh->setEnabled(false);m_windows->setEnabled(false);m_generate->setEnabled(false);
    m_status->setText("正在讀取 UI Automation 控制項，不操作待測 APP…");
    auto result=std::make_shared<AppScanResult>();auto cancel=m_cancel;
    m_worker.reset(QThread::create([handle,result,cancel]{*result=scanAppControls(handle,*cancel);}));
    connect(m_worker.get(),&QThread::finished,this,[this,result] {m_busy=false;m_scan->setEnabled(true);m_refresh->setEnabled(true);m_windows->setEnabled(true);populate(*result);});m_worker->start();
}
void AppScannerDialog::populate(const AppScanResult &result) {
    QSignalBlocker blocker(m_table);
    m_loading=true;m_selected=-1;m_choices.clear();m_table->setRowCount(0);
    if(result.error.isEmpty())for(const auto &c:result.controls) {
        if(!c.invoke && !c.readable)continue;
        Choice choice;choice.control=c;choice.click=c.invoke && !c.readable;
        const QString label=c.name.isEmpty()?c.id:c.name;
        choice.title=(choice.click?"執行「":"確認「")+label.left(70)+(choice.click?"」按鈕":"」顯示文字");
        choice.instruction="由程式自動執行，不需操作。";choice.runningText=choice.click?"正在送出按鈕操作。":"正在等待預期文字出現。";
        choice.failureHint="請確認 APP 狀態與設備連接；仍失敗時通知工程人員。";m_choices.append(choice);
    }
    m_table->setRowCount(int(m_choices.size()));
    for(int i=0;i<m_choices.size();++i) {
        const auto &choice=m_choices[i];auto *name=new QTableWidgetItem(choice.title);
        name->setFlags(Qt::ItemIsSelectable|Qt::ItemIsEnabled|(choice.control.unique?Qt::ItemIsUserCheckable:Qt::NoItemFlags));name->setCheckState(choice.checked?Qt::Checked:Qt::Unchecked);m_table->setItem(i,0,name);
        m_table->setItem(i,1,new QTableWidgetItem(choice.click?"自動按鈕操作":"文字包含檢查"));
        auto *value=new QTableWidgetItem(choice.control.value.left(180));value->setToolTip(choice.control.value);m_table->setItem(i,2,value);
        const QString reason=choice.control.unique?(choice.control.enabled?QString("可定位"):QString("可定位／目前停用")):choice.control.reason;
        m_table->setItem(i,3,new QTableWidgetItem(reason));
    }
    m_loading=false;m_generate->setEnabled(!m_choices.isEmpty());
    m_status->setText(!result.error.isEmpty()?result.error:QString("已讀取 %1 個控制項，列出 %2 個候選。請勾選並依執行順序排列；按鈕操作成功不代表功能已驗證，建議接著加入文字檢查。").arg(result.examined).arg(m_choices.size()));
    if(result.error.isEmpty() && m_choices.isEmpty())m_status->setText(QString("已讀取 %1 個控制項，但沒有可建議的按鈕或文字。請使用原有手動取點／OCR，或切換至實際操作視窗後重掃。").arg(result.examined));
    blocker.unblock();if(!m_choices.isEmpty())m_table->selectRow(0);else select(-1);
}
void AppScannerDialog::store() {
    if(m_loading || m_selected<0 || m_selected>=m_choices.size())return;
    auto &c=m_choices[m_selected];if(c.title!=m_title->text())c.autoTitle=false;
    c.title=m_title->text();c.expected=m_expected->text();c.instruction=m_instruction->toPlainText();c.failureHint=m_failure->toPlainText();
    c.condition=m_condition->currentData().toString();
    if(c.condition!="contains" && c.condition!="equals")c.extraction=m_number->currentData().toString();
    if(!c.click && c.autoTitle && !c.expected.isEmpty()) {
        FlowStep summary;summary.type="readUIA";summary.condition=c.condition;summary.expected=c.expected;summary.text=c.extraction;
        summary.valueLabel=m_number->currentData(Qt::UserRole+2).toString();
        c.title=flowCheckSummary(summary);m_title->setText(c.title);
    }
    m_table->item(m_selected,0)->setText(c.title);
    m_table->item(m_selected,1)->setText(c.click?"自動按鈕操作":m_condition->currentText());
}
void AppScannerDialog::select(int row) {
    m_loading=true;m_selected=row;const bool valid=row>=0 && row<m_choices.size();const Choice c=valid?m_choices[row]:Choice{};
    m_title->setText(c.title);m_expected->setText(c.expected);m_expected->setEnabled(valid && !c.click && c.control.unique);
    m_condition->setCurrentIndex(qMax(0,m_condition->findData(c.condition)));m_condition->setEnabled(valid && !c.click && c.control.unique);
    m_number->clear();m_number->setPlaceholderText("請選擇欄位，例如 CH0 Audio 的 received");
    for(const auto &field:flowNumberFields(c.control.value)){m_number->addItem(field.label,field.pattern);m_number->setItemData(m_number->count()-1,field.value,Qt::UserRole+1);m_number->setItemData(m_number->count()-1,field.name,Qt::UserRole+2);}
    m_number->setCurrentIndex(c.extraction.isEmpty()?-1:m_number->findData(c.extraction));
    const bool numeric=valid && !c.click && c.condition!="contains" && c.condition!="equals";
    m_number->setVisible(numeric);m_numberLabel->setVisible(numeric);m_number->setEnabled(c.control.unique);
    m_expectedLabel->setText(numeric?(c.condition=="range"?"範圍（最小值,最大值）":"PASS 門檻"):"預期文字");
    m_expected->setPlaceholderText(numeric?(c.condition=="range"?"例如 59,61":"例如 0 或 100"):"例如 Running；30 秒內符合才 PASS");
    m_title->setEnabled(valid);m_instruction->setEnabled(valid);m_failure->setEnabled(valid);
    m_instruction->setPlainText(c.instruction);m_failure->setPlainText(c.failureHint);
    m_binding->setText(valid?QString("視窗：%1 · 控制項：%2 · ID：%3\n%4").arg(c.control.windowTitle,c.control.name,c.control.id,
        c.click?QString("此項只確認按鈕命令執行成功；實際功能請另外加入判定。"):QString("請指定真正的通過條件；數值判定須明確選擇欄位，不會自動取第一個數字。")):QString());m_loading=false;updatePreview();
}
void AppScannerDialog::updatePreview() {
    if(m_selected<0 || m_selected>=m_choices.size() || m_choices[m_selected].click){m_preview->clear();return;}
    const QString condition=m_condition->currentData().toString();const bool numeric=condition!="contains" && condition!="equals";
    if(numeric && m_number->currentIndex()<0){m_preview->setText("請選擇要讀取的數值欄位，再填寫門檻。");return;}
    if(m_expected->text().trimmed().isEmpty()){m_preview->setText(numeric?"請填寫 PASS 門檻。":"請填寫預期文字，或改選數值判定。");return;}
    const QString value=numeric?m_number->currentData(Qt::UserRole+1).toString():m_choices[m_selected].control.value;
    QString parsed,error;const bool pass=evaluateFlowValue(value,condition,m_expected->text(),parsed,error);
    m_preview->setText(!error.isEmpty()?error:QString("%1%2 → %3（掃描當下的預覽；執行時會重新讀取，最多等待 30 秒）")
        .arg(numeric?"選取值：":"目前文字：",value.left(100),pass?"PASS":"尚未符合"));
}
void AppScannerDialog::move(int direction) {
    if(m_busy)return;store();const int row=m_table->currentRow(),next=row+direction;
    if(row<0 || next<0 || next>=m_choices.size())return;
    m_loading=true;m_choices.swapItemsAt(row,next);
    for(int column=0;column<m_table->columnCount();++column){auto *a=m_table->takeItem(row,column);auto *b=m_table->takeItem(next,column);m_table->setItem(row,column,b);m_table->setItem(next,column,a);}
    m_loading=false;m_selected=-1;m_table->selectRow(next);select(next);
}
void AppScannerDialog::generate() {
    store();TestFlow draft=m_base;draft.steps.clear();draft.testItems.clear();QString error;
    for(const auto &c:m_choices)if(c.checked && !appendChoice(draft,c.control,c.click,c.title,c.expected,c.instruction,c.runningText,c.failureHint,error,c.condition,c.extraction)) {
        QMessageBox::warning(this,"草稿尚未完成",c.title+"\n"+error);return;
    }
    if(draft.steps.isEmpty()){QMessageBox::information(this,"尚未選擇","請至少勾選一個可定位的測試項目。");return;}
    m_draft=draft;accept();
}
void AppScannerDialog::reject(){if(m_busy){m_cancel->store(true);m_status->setText("正在取消；目前的 UIA 呼叫返回後即可關閉。");return;}QDialog::reject();}
void AppScannerDialog::closeEvent(QCloseEvent *event){if(m_busy){reject();event->ignore();}else event->accept();}

bool appScannerSelfTest() {
    for(int type:{UIA_TextControlTypeId,UIA_EditControlTypeId,UIA_DocumentControlTypeId}) {
        AppControl text;classifyControl(text,type,false,true);
        if(!text.readable || text.invoke)return false;
    }
    AppControl typedButton;classifyControl(typedButton,UIA_ButtonControlTypeId,false,true);
    if(!typedButton.invoke || typedButton.readable)return false;
    classifyControl(typedButton,UIA_PaneControlTypeId,false,true);if(typedButton.invoke || typedButton.readable)return false;
    classifyControl(typedButton,UIA_EditControlTypeId,true,true);if(typedButton.invoke || typedButton.readable)return false;
    QVector<AppControl> controls;
    AppControl button;button.name="Start";button.id="unique-start";button.invoke=true;button.windowTitle="Fixture";controls.append(button);
    AppControl text;text.name="Status";text.id="unique-status";text.value="Stopped";text.readable=true;text.windowTitle="Fixture";controls.append(text);
    AppControl duplicate=button;duplicate.id.clear();controls.append(duplicate);controls.append(duplicate);
    resolveBindings(controls);
    if(!controls[0].unique || !controls[1].unique || controls[2].unique || controls[3].unique)return false;
    TestFlow flow;flow.executable=QCoreApplication::applicationFilePath();QString error;
    if(appendChoice(flow,controls[1],false,"確認狀態",{},"不需操作","等待","通知工程",error))return false;
    if(!appendChoice(flow,controls[0],true,"啟動設備",{},"不需操作","啟動中","確認設備",error)
        || !appendChoice(flow,controls[1],false,"確認運作狀態","Running","不需操作","等待狀態","檢查輸入",error))return false;
    AppControl counters=controls[1];counters.value="CH0 | Running | 1920x1080\nCH0 Video | received=124 frames\nCH0 Audio | 48000 Hz 2 ch 16-bit | received=208 frames\nCH0 Preview | 60.68 FPS";
    const auto numbers=flowNumberFields(counters.value);QString audioPattern;
    for(const auto &field:numbers)if(field.label.contains("CH0 Audio") && field.label.contains("received="))audioPattern=field.pattern;
    if(audioPattern.isEmpty() || appendChoice(flow,counters,false,"音訊計數","0","不需操作","等待","檢查音訊",error,"gt"))return false;
    TestFlow numeric;numeric.executable=flow.executable;
    if(!appendChoice(numeric,counters,false,"確認音訊資料接收","0","不需操作","等待","檢查音訊",error,"gt",audioPattern))return false;
    const QString changed="CH0 | Running | 3840x2160\nCH0 Video | received=999 frames\nCH0 Audio | 44100 Hz 1 ch 24-bit | received=0 frames\nCH0 Preview | 59.9 FPS";
    const auto extracted=QRegularExpression(numeric.steps[0].text).match(changed);
    QString parsed,problem;
    if(extracted.captured(1)!="0" || evaluateFlowValue(extracted.captured(1),"gt","0",parsed,problem))return false;
    if(!flowNumberFields("received=2 frames\nreceived=3 frames").isEmpty())return false;
    QTemporaryDir folder(QCoreApplication::applicationDirPath()+"/scanner_check_XXXXXX");TestFlow restored;
    if(!folder.isValid() || !saveFlow(folder.path()+"/draft.json",flow,error) || !loadFlow(folder.path()+"/draft.json",restored,error))return false;
    TestFlow restoredNumeric;
    if(!saveFlow(folder.path()+"/numeric.json",numeric,error) || !loadFlow(folder.path()+"/numeric.json",restoredNumeric,error)
        || restoredNumeric.steps[0].text!=audioPattern || restoredNumeric.steps[0].condition!="gt" || restoredNumeric.steps[0].expected!="0")return false;
    return restored.steps.size()==2 && restored.steps[0].automationId=="unique-start" && restored.steps[0].type=="clickUIA"
        && restored.steps[1].expected=="Running" && restored.steps[1].condition=="contains" && restored.testItems.size()==2 && validateFlow(restored,error);
}
bool AppScannerDialog::uiSelfTest() {
    if(insertionIndex()!=m_base.steps.size())return false;
    for(int i=0;i<m_base.steps.size();++i){m_position->setCurrentIndex(i);if(insertionIndex()!=i)return false;}
    m_position->setCurrentIndex(m_position->count()-1);
    AppScanResult result;AppControl b;b.name="Start";b.id="start";b.invoke=true;b.unique=true;b.windowTitle="示範 APP";
    AppControl t;t.name="設備狀態";t.id="status";t.readable=true;t.invoke=true;t.unique=true;t.value="Stopped";t.windowTitle=b.windowTitle;
    result.examined=2;result.controls={b,t};populate(result);
    m_table->item(0,0)->setCheckState(Qt::Checked);m_table->selectRow(1);m_expected->setText("Running");m_table->item(1,0)->setCheckState(Qt::Checked);store();generate();
    if(m_draft.steps.size()!=2 || m_draft.steps[1].expected!="Running" || m_draft.testItems.size()!=2)return false;
    m_table->selectRow(1);move(-1);generate();
    if(m_draft.steps[0].type!="readUIA" || m_draft.steps[0].expected!="Running" || m_draft.steps[1].type!="clickUIA")return false;
    m_table->selectRow(0);move(1);generate();
    if(m_draft.steps[0].type!="clickUIA" || m_draft.steps[1].expected!="Running")return false;
    t.value="CH0 | Running | 1920x1080\nCH0 Video | received=124 frames\nCH0 Audio | 48000 Hz 2 ch 16-bit | received=208 frames\nCH0 Preview | 60.68 FPS";
    result.controls={t};populate(result);m_table->item(0,0)->setCheckState(Qt::Checked);
    m_title->setText("確認音訊資料接收");m_condition->setCurrentIndex(m_condition->findData("gt"));
    if(m_number->currentIndex()!=-1)return false;
    int audio=-1;for(int i=0;i<m_number->count();++i)if(m_number->itemText(i).contains("CH0 Audio") && m_number->itemText(i).contains("received="))audio=i;
    if(audio<0)return false;m_number->setCurrentIndex(audio);m_expected->setText("0");store();updatePreview();generate();
    return m_draft.steps.size()==1 && m_draft.steps[0].condition=="gt" && m_draft.steps[0].expected=="0"
        && QRegularExpression(m_draft.steps[0].text).match(t.value).captured(1)=="208";
}
