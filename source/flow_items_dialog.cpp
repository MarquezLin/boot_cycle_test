#include "flow_items_dialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>

FlowItemsDialog::FlowItemsDialog(const TestFlow &flow,int selectedStep,QWidget *parent):QDialog(parent),m_flow(flow) {
    setWindowTitle("測試項目與現場說明");resize(980,720);setMinimumSize(780,560);
    auto *layout=new QVBoxLayout(this);
    auto *metadata=new QFormLayout;m_name=new QLineEdit(flow.name);m_version=new QLineEdit(flow.version);
    m_name->setPlaceholderText("例如：HDMI 擷取卡出廠測試");m_version->setPlaceholderText("例如：1.0");
    metadata->addRow("測試名稱",m_name);metadata->addRow("腳本版本",m_version);layout->addLayout(metadata);
    auto *hint=new QLabel("將連續的工程步驟整理成工廠看得懂的測試項目。選定範圍後按「套用步驟範圍」。\n未分組的步驟仍會各自顯示；操作說明僅用於顯示，不會新增等待或改變判定條件。");
    hint->setWordWrap(true);layout->addWidget(hint);
    auto *body=new QHBoxLayout;layout->addLayout(body,1);
    auto *left=new QVBoxLayout;m_list=new QListWidget;left->addWidget(m_list,1);
    auto *add=new QPushButton("新增測試項目");auto *remove=new QPushButton("刪除項目（保留步驟）");left->addWidget(add);left->addWidget(remove);body->addLayout(left,1);
    auto *scroll=new QScrollArea;scroll->setWidgetResizable(true);auto *contents=new QWidget;auto *right=new QVBoxLayout(contents);
    auto *form=new QFormLayout;m_itemName=new QLineEdit;m_itemName->setPlaceholderText("例如：確認音訊擷取正常");form->addRow("測試項目名稱",m_itemName);
    auto area=[&](const QString &label,const QString &placeholder) {auto *edit=new QPlainTextEdit;edit->setPlaceholderText(placeholder);edit->setMinimumHeight(60);edit->setMaximumHeight(100);form->addRow(label,edit);return edit;};
    m_instruction=area("操作者要做什麼","請保持訊號線連接，不需操作。");
    m_running=area("執行中提示","正在等待音訊資料，最長約 30 秒。");
    m_failure=area("失敗排查提示","請確認訊號源已開啟，並檢查連接線。");right->addLayout(form);
    auto *range=new QHBoxLayout;m_first=new QSpinBox;m_last=new QSpinBox;
    m_first->setRange(1,qMax(1,int(flow.steps.size())));m_last->setRange(1,qMax(1,int(flow.steps.size())));
    m_first->setValue(qMax(1,selectedStep+1));m_last->setValue(qMax(1,selectedStep+1));
    range->addWidget(new QLabel("包含工程步驟"));range->addWidget(m_first);range->addWidget(new QLabel("至"));range->addWidget(m_last);
    auto *apply=new QPushButton("套用步驟範圍");range->addWidget(apply);right->addLayout(range);
    m_range=new QLabel;m_range->setWordWrap(true);right->addWidget(m_range);
    auto *steps=new QPlainTextEdit;steps->setReadOnly(true);steps->setMaximumHeight(150);
    QStringList names;for(int i=0;i<flow.steps.size();++i)names<<QString("%1  %2 · %3").arg(i+1).arg(flowStepDisplayName(flow.steps[i]),flowTypeName(flow.steps[i].type));
    steps->setPlainText(names.join('\n'));right->addWidget(steps,1);scroll->setWidget(contents);body->addWidget(scroll,3);
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this] {store();QString error;
        if(!validateFlowItems(m_flow,error)) {QMessageBox::warning(this,"項目設定無效",error);return;}accept();});
    connect(m_list,&QListWidget::currentRowChanged,this,[this](int row){store();select(row);});
    connect(add,&QPushButton::clicked,this,[this] {store();FlowTestItem item;item.id=QUuid::createUuid().toString(QUuid::Id128);item.name="新測試項目";
        m_flow.testItems.append(item);refreshList();m_list->setCurrentRow(int(m_flow.testItems.size())-1);});
    connect(remove,&QPushButton::clicked,this,[this] {if(m_selected<0)return;const int row=m_selected;const QString id=m_flow.testItems[row].id;
        for(auto &s:m_flow.steps)if(s.testItemId==id)s.testItemId.clear();m_flow.testItems.removeAt(row);m_selected=-1;refreshList();
        if(m_list->count())m_list->setCurrentRow(qMin(row,m_list->count()-1));else select(-1);});
    connect(apply,&QPushButton::clicked,this,[this]{assign();});
    connect(m_itemName,&QLineEdit::textEdited,this,[this]{store();if(m_selected>=0)m_list->item(m_selected)->setText(m_itemName->text());});
    int initial=-1;
    if(selectedStep>=0 && selectedStep<m_flow.steps.size()){
        const QString id=m_flow.steps[selectedStep].testItemId;
        for(int i=0;i<m_flow.testItems.size();++i)if(m_flow.testItems[i].id==id)initial=i;
        if(initial<0){
            // An ungrouped step opens its own local draft instead of selecting
            // an unrelated existing category. Cancel leaves the source intact.
            FlowTestItem item;item.id=QUuid::createUuid().toString(QUuid::Id128);
            item.name=flowStepDisplayName(m_flow.steps[selectedStep]);
            if(item.name.isEmpty())item.name=QString("測試步驟 %1").arg(selectedStep+1);
            m_flow.testItems.append(item);m_flow.steps[selectedStep].testItemId=item.id;
            initial=int(m_flow.testItems.size())-1;
        }
    }
    if(initial<0 && !m_flow.testItems.isEmpty())initial=0;
    refreshList();m_list->setCurrentRow(initial);select(initial);
}
void FlowItemsDialog::store() {
    if(m_loading)return;m_flow.name=m_name->text().trimmed();m_flow.version=m_version->text().trimmed();
    if(m_selected<0 || m_selected>=m_flow.testItems.size())return;
    auto &item=m_flow.testItems[m_selected];item.name=m_itemName->text().trimmed();item.instruction=m_instruction->toPlainText();item.runningText=m_running->toPlainText();item.failureHint=m_failure->toPlainText();
}
void FlowItemsDialog::refreshList() {
    QSignalBlocker blocker(m_list);m_list->clear();for(const auto &item:m_flow.testItems)m_list->addItem(flowItemDisplayName(m_flow,item));
    if(m_selected>=0 && m_selected<m_list->count())m_list->setCurrentRow(m_selected);
}
void FlowItemsDialog::select(int row) {
    m_loading=true;m_selected=row;const bool valid=row>=0 && row<m_flow.testItems.size();
    for(QWidget *w:QList<QWidget*>{m_itemName,m_instruction,m_running,m_failure,m_first,m_last})w->setEnabled(valid);
    const auto item=valid?m_flow.testItems[row]:FlowTestItem{};
    m_itemName->setText(valid?flowItemDisplayName(m_flow,item):QString());m_instruction->setPlainText(item.instruction);m_running->setPlainText(item.runningText);m_failure->setPlainText(item.failureHint);
    int first=-1,last=-1;for(int i=0;valid && i<m_flow.steps.size();++i)if(m_flow.steps[i].testItemId==item.id){if(first<0)first=i;last=i;}
    if(first>=0){m_first->setValue(first+1);m_last->setValue(last+1);}
    m_range->setText(first>=0?QString("目前包含步驟 %1～%2；儲存後會一起顯示為「%3」。").arg(first+1).arg(last+1).arg(item.name):"尚未分配步驟；請選取範圍並按套用。未分配的項目不會出現在工廠畫面。");
    QStringList checks;
    for(const auto &step:m_flow.steps)if(valid && step.testItemId==item.id) {const auto check=flowCheckSummary(step);if(!check.isEmpty())checks<<check;}
    if(!checks.isEmpty())m_range->setText(m_range->text()+"\nPASS 條件："+checks.join("；"));
    m_loading=false;
}
void FlowItemsDialog::assign() {
    store();if(m_selected<0 || m_flow.steps.isEmpty())return;const int first=m_first->value()-1,last=m_last->value()-1;
    if(first>last){QMessageBox::warning(this,"範圍無效","起始步驟不可大於結束步驟。");return;}
    TestFlow next=m_flow;const QString id=next.testItems[m_selected].id;
    for(auto &s:next.steps)if(s.testItemId==id)s.testItemId.clear();
    for(int i=first;i<=last;++i)next.steps[i].testItemId=id;
    QString error;if(!validateFlowItems(next,error)){QMessageBox::warning(this,"無法分組",error);return;}
    m_flow=next;select(m_selected);
}
TestFlow FlowItemsDialog::resultFlow(){store();return m_flow;}
bool FlowItemsDialog::selfTest() {
    TestFlow source;source.testItems={{"existing","既有分類",{},{},{}}};
    FlowStep grouped;grouped.type="delay";grouped.name="原步驟";grouped.testItemId="existing";
    FlowStep ungrouped;ungrouped.type="delay";ungrouped.name="獨立步驟";source.steps={grouped,grouped,ungrouped};
    FlowItemsDialog selectedGroup(source,1);
    if(selectedGroup.m_selected!=0 || selectedGroup.m_first->value()!=1 || selectedGroup.m_last->value()!=2)return false;
    FlowItemsDialog selectedUngrouped(source,2);
    const auto draft=selectedUngrouped.resultFlow();QString selectionError;
    if(selectedUngrouped.m_first->value()!=3 || selectedUngrouped.m_last->value()!=3
       || draft.steps[2].testItemId.isEmpty() || draft.steps[2].testItemId=="existing"
       || draft.steps[0].testItemId!="existing" || !source.steps[2].testItemId.isEmpty()
       || !validateFlowItems(draft,selectionError))return false;
    if(m_flow.steps.size()<2)return false;
    FlowTestItem item;item.id="ui-test";item.name="確認設備連線";m_flow.testItems={item};refreshList();select(0);
    m_name->setText("工廠測試");m_version->setText("1.0");m_instruction->setPlainText("請保持連接");m_running->setPlainText("正在檢查");m_failure->setPlainText("檢查訊號源");
    m_first->setValue(1);m_last->setValue(2);assign();const auto result=resultFlow();
    return result.name=="工廠測試" && result.version=="1.0" && result.steps[0].testItemId=="ui-test" && result.steps[1].testItemId=="ui-test"
        && result.testItems[0].instruction=="請保持連接" && result.testItems[0].runningText=="正在檢查" && result.testItems[0].failureHint=="檢查訊號源";
}
