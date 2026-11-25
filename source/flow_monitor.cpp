#include "flow_monitor.h"
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

namespace {
enum {TitleRole=Qt::UserRole, TypeRole, StateRole, DetailRole, TimeRole, BadgeRole};
QString stateName(const QString &s) {
    if(s=="running") return "執行中";
    if(s=="pass") return "已通過";
    if(s=="fail") return "失敗";
    if(s=="stopping") return "停止中";
    if(s=="stopped") return "已停止";
    if(s=="skipped") return "未執行";
    return "待執行";
}
QColor stateColor(const QString &s) {
    if(s=="running") return QColor("#2563eb");
    if(s=="pass") return QColor("#15803d");
    if(s=="fail") return QColor("#c02632");
    if(s=="stopped" || s=="stopping") return QColor("#a16207");
    return QColor("#64748b");
}
class CardDelegate final : public QStyledItemDelegate {
public:
    explicit CardDelegate(QObject *parent):QStyledItemDelegate(parent){}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {return {320,100};}
    void paint(QPainter *p,const QStyleOptionViewItem &o,const QModelIndex &i) const override {
        p->save();p->setRenderHint(QPainter::Antialiasing);
        const QString state=i.data(StateRole).toString();const QColor color=stateColor(state);
        const QRect r=o.rect.adjusted(6,5,-8,-5);
        const bool active=state=="running" || state=="stopping";
        QColor background=active?QColor("#eff6ff"):state=="fail"?QColor("#fff1f2")
            :state=="pass"?QColor("#f0fdf4"):state=="stopped"?QColor("#fffbeb"):QColor("#ffffff");
        p->setBrush(background);p->setPen(QPen(active?color:QColor("#e2e8f0"),active?2:1));
        p->drawRoundedRect(r,10,10);
        if(o.state & QStyle::State_Selected) {p->setPen(QPen(QColor("#94a3b8"),1,Qt::DashLine));p->setBrush(Qt::NoBrush);p->drawRoundedRect(r.adjusted(3,3,-3,-3),8,8);}
        QRect circle(r.left()+14,r.top()+18,34,34);
        p->setBrush(state=="pending" || state=="skipped"?QColor("#f1f5f9"):color);p->setPen(Qt::NoPen);p->drawEllipse(circle);
        p->setPen(state=="pending" || state=="skipped"?color:QColor("#ffffff"));
        QFont font=o.font;font.setBold(true);font.setPointSize(11);p->setFont(font);
        if(state=="pass") {
            QPainterPath check;check.moveTo(circle.left()+9,circle.top()+17);check.lineTo(circle.left()+15,circle.top()+23);check.lineTo(circle.left()+25,circle.top()+11);
            p->setPen(QPen(Qt::white,2.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p->drawPath(check);
        } else p->drawText(circle,Qt::AlignCenter,active?QStringLiteral(">") : state=="fail"?QStringLiteral("!"):i.data(BadgeRole).toString());
        const int x=circle.right()+14;
        p->setPen(QColor("#0f172a"));
        QRect title(x,r.top()+12,r.right()-x-94,24);
        p->drawText(title,Qt::AlignVCenter,p->fontMetrics().elidedText(i.data(TitleRole).toString(),Qt::ElideRight,title.width()));
        font.setPointSize(9);font.setBold(false);p->setFont(font);p->setPen(color);
        p->drawText(QRect(r.right()-90,r.top()+13,78,22),Qt::AlignRight|Qt::AlignVCenter,stateName(state));
        p->setPen(QColor("#64748b"));
        p->drawText(QRect(x,r.top()+38,r.right()-x-12,19),Qt::AlignVCenter,
            p->fontMetrics().elidedText(i.data(TypeRole).toString(),Qt::ElideRight,r.right()-x-12));
        QString detail=i.data(DetailRole).toString();detail.replace('\n',' ');
        const QString time=i.data(TimeRole).toString();
        p->drawText(QRect(x,r.top()+60,r.right()-x-85,20),Qt::AlignVCenter,p->fontMetrics().elidedText(detail,Qt::ElideRight,r.right()-x-85));
        p->drawText(QRect(r.right()-80,r.top()+60,68,20),Qt::AlignRight|Qt::AlignVCenter,time);
        p->restore();
    }
};
QString stepDescription(const FlowStep &s) {
    if (s.type == "loop")
        return QString("跳回第 %1 步 · 設定 %2 次").arg(s.targetStep).arg(s.jumpCount);
    QStringList parts;
    if(!s.windowTitle.isEmpty()) parts << "視窗："+s.windowTitle;
    if(!s.automationId.isEmpty()) parts << "控制項 ID："+s.automationId;
    if(!s.selector.isEmpty()) parts << "控制項："+s.selector;
    if(!s.text.isEmpty()) parts << "內容："+s.text;
    if(!s.expected.isEmpty()) parts << "判定："+s.condition+" "+s.expected;
    if(s.type=="delay") parts << QString("等待 %1 秒").arg(s.timeoutMs/1000.0);
    else if(s.timeoutMs>0) parts << QString("逾時 %1 秒").arg(s.timeoutMs/1000.0);
    if(!s.region.isNull()) parts << QString("範圍 (%1, %2) %3×%4").arg(s.region.x()).arg(s.region.y()).arg(s.region.width()).arg(s.region.height());
    return parts.join(" · ");
}
}

QJsonObject flowMonitorPlan(const TestFlow &flow) {
    QJsonArray steps;
    for(const auto &s:flow.steps) {
        QJsonObject step{{"title",s.name.isEmpty()?flowTypeName(s.type):flowStepDisplayName(s)},
            {"type",flowTypeName(s.type)},{"detail",flowCheckSummary(s).isEmpty()?stepDescription(s):"PASS："+flowCheckSummary(s)+" · "+stepDescription(s)},{"testItemId",s.testItemId}};
        if (s.type == "loop") {
            step.insert("targetStep", s.targetStep);
            step.insert("jumpCount", s.jumpCount);
        }
        steps.append(step);
    }
    QJsonArray items;
    for(const auto &item:flow.testItems) items.append(QJsonObject{{"id",item.id},{"name",flowItemDisplayName(flow,item)},{"instruction",item.instruction},{"runningText",item.runningText},{"failureHint",item.failureHint}});
    return {{"event","plan"},{"steps",steps},{"testItems",items},{"name",flow.name},{"version",flow.version}};
}
FlowMonitor::FlowMonitor(QWidget *parent):QWidget(parent) {
    setObjectName("flowMonitor");
    auto *layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(8);
    m_heading=new QLabel("流程總覽");m_heading->setStyleSheet("font-size: 18px; font-weight: 600; color: #0f172a;");layout->addWidget(m_heading);
    m_current=new QLabel("匯入 JSON 後，即可預覽完整步驟。");m_current->setWordWrap(true);m_current->setMinimumHeight(32);layout->addWidget(m_current);
    m_guidance=new QPlainTextEdit;m_guidance->setReadOnly(true);m_guidance->setMinimumHeight(90);m_guidance->setMaximumHeight(130);
    m_guidance->setStyleSheet("QPlainTextEdit {background: #eff6ff; color: #1e3a5f; border: 1px solid #dbeafe; border-radius: 8px; padding: 8px; font-size: 16px;}");
    layout->addWidget(m_guidance);
    m_progress=new QProgressBar;m_progress->setRange(0,1);m_progress->setValue(0);m_progress->setFormat("尚未開始");m_progress->setFixedHeight(18);layout->addWidget(m_progress);
    m_loopSummary = new QLabel;
    m_loopSummary->setObjectName("loopSummary");
    m_loopSummary->setTextFormat(Qt::PlainText);
    m_loopSummary->setWordWrap(true);
    m_loopSummary->setStyleSheet("background: #eff6ff; color: #1e3a5f; border-radius: 8px; padding: 8px; font-weight: 600;");
    m_loopSummary->hide();
    layout->addWidget(m_loopSummary);
    m_list=new QListWidget;m_list->setItemDelegate(new CardDelegate(m_list));m_list->setSpacing(0);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setStyleSheet("QListWidget {background: #f8fafc; border: none;} QListWidget::item {border: none;}");layout->addWidget(m_list,1);
    m_details=new QLabel("點選步驟，查看設定與執行結果。");m_details->setTextFormat(Qt::PlainText);m_details->setWordWrap(true);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);m_details->setMaximumHeight(100);
    m_details->setStyleSheet("background: #f1f5f9; color: #334155; padding: 10px; border-radius: 8px;");layout->addWidget(m_details);
    connect(m_list,&QListWidget::currentRowChanged,this,[this]{showDetails();});
    m_timer=new QTimer(this);m_timer->setInterval(500);connect(m_timer,&QTimer::timeout,this,[this]{refresh();});
}
void FlowMonitor::setPlan(const QJsonObject &plan) {
    m_timer->stop();m_entries.clear();m_active=-1;m_started=false;m_list->clear();m_testItems.clear();
    for(const auto &value:plan.value("testItems").toArray()) {const auto item=value.toObject();m_testItems.insert(item["id"].toString(),item);}
    m_details->setText("點選步驟，查看設定與執行結果。");m_details->setToolTip({});
    const auto steps=plan.value("steps").toArray();
    if(!steps.isEmpty()) {
        m_entries.append({"準備測試","環境與啟動等待","檢查設定，等待開始執行 JSON"});
        for(const auto &value:steps) {
            const auto s=value.toObject();
            Entry e;
            e.title=s["title"].toString();e.type=s["type"].toString();e.detail=s["detail"].toString();e.itemId=s["testItemId"].toString();
            e.targetStep = s["targetStep"].toInt();
            e.jumpCount = s["jumpCount"].toInt();
            m_entries.append(e);
        }
        m_entries.append({"結果與下一步","試跑／電源控制",m_tail});
    }
    rebuildDisplay();
    refreshLoopSummary();
    refresh();
    if(m_entries.isEmpty()) {m_current->setText(plan.value("error").toString("請先匯入 JSON 流程。"));m_details->setText("匯入後顯示設定步驟；執行中會自動追蹤目前位置。");}
}
void FlowMonitor::refreshLoopSummary() {
    QStringList lines;
    for (int row = 1; row < m_entries.size() - 1; ++row) {
        const auto &entry = m_entries[row];
        if (entry.targetStep > 0)
            lines.append(QString("第 %1 步 → 第 %2 步　已跳轉 %3 / %4 次")
                .arg(row).arg(entry.targetStep).arg(entry.completedJumps).arg(entry.jumpCount));
    }
    m_loopSummary->setText(lines.join('\n'));
    m_loopSummary->setVisible(!lines.isEmpty());
}
void FlowMonitor::setTailDescription(const QString &description) {
    m_tail=description;if(!m_entries.isEmpty()) m_entries.last().detail=description;refresh();
}
void FlowMonitor::begin(const QString &preparation) {
    m_started=true;if(!m_entries.isEmpty()) {m_entries[0].observed=preparation;activate(0);}
}
void FlowMonitor::activate(int row) {
    if(row<0 || row>=m_entries.size()) return;
    m_active=row;m_entries[row].state="running";m_elapsed.restart();m_timer->start();
    m_list->setCurrentRow(displayRow(row));refresh();
    QTimer::singleShot(0,this,[this,row] {
        if(row==m_active) m_list->scrollToItem(m_list->item(displayRow(row)),QAbstractItemView::PositionAtCenter);
    });
}
void FlowMonitor::setState(int row,const QString &state,const QString &detail) {
    if(row<0 || row>=m_entries.size()) return;
    if(m_active==row) {m_entries[row].elapsed=m_elapsed.elapsed();m_active=-1;m_timer->stop();}
    m_entries[row].state=state;if(!detail.isEmpty())m_entries[row].observed=detail;refresh();
}
void FlowMonitor::applyEvent(const QJsonObject &event) {
    const auto kind=event["event"].toString();
    if(kind=="plan") {setPlan(event);begin("已讀取本次實際執行的 JSON");return;}
    const int row=event.value("index").toInt(-1)+1;
    if(row<1 || row>=m_entries.size()-1)return;
    if(kind=="start") {if(row==1)setState(0,"pass");activate(row);}
    else if(kind=="loopJump") {
        auto &entry = m_entries[row];
        const int completed = event.value("completedJumps").toInt(-1);
        if (entry.targetStep > 0 && completed > entry.completedJumps && completed <= entry.jumpCount) {
            entry.completedJumps = completed;
            refreshLoopSummary();
        }
    }
    else if(kind=="observed") {m_entries[row].observed=event["value"].toString();refresh();}
    else if(kind=="finish") {
        const QString result=event["result"].toString();
        if(result!="PASS" && result!="FAIL" && result!="STOP") return;
        setState(row,result=="PASS"?"pass":result=="FAIL"?"fail":"stopped",event["value"].toString());
        if(result!="PASS") for(int i=row+1;i<m_entries.size();++i) if(m_entries[i].state=="pending")m_entries[i].state="skipped";
        refresh();
    }
}
void FlowMonitor::phase(const QString &description) {
    if(m_entries.isEmpty())return;
    if(m_active!=m_entries.size()-1)activate(int(m_entries.size())-1);
    m_entries.last().observed=description;refresh();
}
void FlowMonitor::requestStop() {
    if(m_active>=0) m_entries[m_active].state="stopping";
    refresh();
}
void FlowMonitor::finish(bool success,bool stopped,const QString &description) {
    if(m_entries.isEmpty()) return;
    bool stepFailure=false;for(const auto &e:m_entries)if(e.state=="fail" || e.state=="stopped")stepFailure=true;
    if(m_active>=0) setState(m_active,stopped?"stopped":success?"pass":"fail",description);
    else if(!stepFailure) setState(int(m_entries.size())-1,stopped?"stopped":success?"pass":"fail",description);
    for(auto &e:m_entries) if(e.state=="pending")e.state="skipped";
    for(int i=0;i<m_entries.size();++i) if(m_entries[i].state=="fail" || m_entries[i].state=="stopped") {
        m_list->setCurrentRow(displayRow(i));
        QTimer::singleShot(0,this,[this,i] {if(m_active<0 && displayRow(i)>=0)m_list->scrollToItem(m_list->item(displayRow(i)),QAbstractItemView::PositionAtCenter);});
        break;
    }
    m_timer->stop();refresh();
}
QString FlowMonitor::rowState(int row) const {return row>=0 && row<m_entries.size()?m_entries[row].state:QString();}
void FlowMonitor::setEngineering(bool enabled) {
    m_engineering=enabled;m_details->setVisible(enabled);m_guidance->setVisible(!enabled);
    rebuildDisplay();refresh();
}
int FlowMonitor::displayRow(int rawRow) const {
    for(int i=0;i<m_displayRows.size();++i)if(m_displayRows[i].contains(rawRow))return i;
    return -1;
}
int FlowMonitor::testItemCount() const {
    int count=0;QString previous;
    for(int i=1;i<m_entries.size()-1;++i) {
        const auto id=m_entries[i].itemId;
        if(id.isEmpty() || id!=previous)++count;
        previous=id;
    }
    return count;
}
void FlowMonitor::rebuildDisplay() {
    int focus=m_active;
    if(focus<0)for(int i=0;i<m_entries.size();++i)if(m_entries[i].state=="fail" || m_entries[i].state=="stopped"){focus=i;break;}
    m_displayRows.clear();m_list->clear();
    for(int i=0;i<m_entries.size();++i) {
        const auto &e=m_entries[i];
        if(!m_engineering && !e.itemId.isEmpty() && i>1 && !m_displayRows.isEmpty()
            && m_entries[m_displayRows.last().first()].itemId==e.itemId) m_displayRows.last().append(i);
        else m_displayRows.append(QVector<int>{i});
    }
    for(const auto &rows:m_displayRows) {Q_UNUSED(rows);m_list->addItem(new QListWidgetItem);}
    const int current=displayRow(focus);
    if(current>=0)m_list->setCurrentRow(current);
    else if(!m_displayRows.isEmpty())m_list->setCurrentRow(0);
    m_details->setVisible(m_engineering);m_guidance->setVisible(!m_engineering);
}
QString FlowMonitor::displayState(const QVector<int> &rows) const {
    bool allPassed=true,skipped=false,anyPassed=false;
    for(int row:rows) {
        const auto state=m_entries[row].state;
        if(state=="fail")return "fail";
        if(state=="stopped")return "stopped";
        if(state=="stopping")return "stopping";
        if(state=="running")return "running";
        allPassed=allPassed && state=="pass";anyPassed=anyPassed || state=="pass";skipped=skipped || state=="skipped";
    }
    if(allPassed)return "pass";
    if(skipped)return "skipped";
    return anyPassed?"running":"pending";
}
void FlowMonitor::refresh() {
    int passed=0;const int count=qMax(0,int(m_displayRows.size())-2);
    for(int i=0;i<m_displayRows.size();++i) {
        const auto &rows=m_displayRows[i];const auto &e=m_entries[rows.first()];auto *item=m_list->item(i);
        const auto definition=m_testItems.value(e.itemId);const bool grouped=!m_engineering && !definition.isEmpty();
        const QString state=displayState(rows),title=grouped?definition["name"].toString():e.title;
        qint64 ms=0;for(int row:rows){const auto &sub=m_entries[row];ms+=sub.elapsed+(row==m_active?m_elapsed.elapsed():0);}
        const bool middle=i>0 && i<m_displayRows.size()-1;
        const QString subtitle=m_engineering?(middle?QString("步驟 %1 · ").arg(i)+e.type:e.type)
            :middle?QString("測試項目 %1 / %2").arg(i).arg(count):QString("測試階段");
        QString detail;
        if(m_engineering) detail=e.observed.isEmpty()?e.detail:e.observed;
        else if(grouped) {
            if(state=="fail")detail=definition["failureHint"].toString();
            else if(state=="running" || state=="stopping")detail=definition["runningText"].toString();
            else detail=definition["instruction"].toString();
        } else if(!middle)detail=e.observed.isEmpty()?e.detail:e.observed;
        else detail=state=="fail"?"請切換工程檢視查看失敗原因。":"依腳本自動執行，不需操作。";
        item->setData(TitleRole,title);item->setData(TypeRole,subtitle);item->setData(StateRole,state);item->setData(DetailRole,detail);
        item->setData(BadgeRole,i==0?QString("準"):i==m_displayRows.size()-1?QString("終"):QString::number(i));
        item->setData(TimeRole,ms>0?QString::number(ms/1000.0,'f',1)+" s":QString());
        item->setToolTip(title+'\n'+detail);item->setData(Qt::AccessibleTextRole,title+" "+stateName(state));
        if(middle && state=="pass")++passed;
    }
    const QString unit=m_engineering?"步驟":"項目";
    m_heading->setText(QString(m_engineering?"工程步驟  ·  %1 個步驟":"測試項目  ·  %1 個項目").arg(count));
    m_progress->setRange(0,qMax(1,count));m_progress->setValue(passed);
    m_progress->setFormat(QString("已通過 %1 / %2 %3").arg(passed).arg(count).arg(unit));
    int raw=m_active;
    if(raw<0) {
        for(int i=0;i<m_entries.size();++i)if(m_entries[i].state!="pending" && m_entries[i].state!="skipped")raw=i;
        for(int i=0;i<m_entries.size();++i)if(m_entries[i].state=="fail" || m_entries[i].state=="stopped"){raw=i;break;}
    }
    const int index=displayRow(raw);
    if(index>=0) {
        const auto *item=m_list->item(index);const QString state=item->data(StateRole).toString();
        m_current->setText(stateName(state)+" · "+item->data(TitleRole).toString());
        m_current->setStyleSheet(QString("font-size: %1px; font-weight:600; color:").arg(m_engineering?16:23)+stateColor(state).name()+";");
    } else {
        m_current->setText(m_started?"等待測試開始…":"測試已就緒 · 請先確認接線，再開始測試");
        m_current->setStyleSheet("font-size: 20px; font-weight:600; color:#334155;");
    }
    showDetails();
}
void FlowMonitor::showDetails() {
    const int selected=m_list->currentRow();
    if(selected<0 || selected>=m_displayRows.size()) {m_guidance->setPlainText("請先載入測試腳本；建立者可在工程檢視設定現場說明。");return;}
    int display=selected;
    if(!m_engineering && m_active>=0)display=displayRow(m_active);
    const auto &rows=m_displayRows[display];const auto &e=m_entries[rows.first()];
    const auto definition=m_testItems.value(e.itemId);const QString state=displayState(rows);
    if(!m_engineering) {
        QStringList text;
        if(!definition.isEmpty()) {
            if(!definition["instruction"].toString().isEmpty())text<<"請這樣做："+definition["instruction"].toString();
            if((state=="running" || state=="stopping") && !definition["runningText"].toString().isEmpty())text<<"目前進度："+definition["runningText"].toString();
            if(state=="fail")text<<"請先檢查："+(definition["failureHint"].toString().isEmpty()?QString("請通知工程人員查看詳細原因。"):definition["failureHint"].toString());
        } else if(rows.first()==0 || rows.first()==m_entries.size()-1)text<< (e.observed.isEmpty()?e.detail:e.observed);
        else text<<"此項依腳本自動執行，不需操作。";
        if(state=="pass")text<<"此項檢查已通過。";
        if(state=="stopped")text<<"測試已停止；重新開始時會從流程開頭執行。";
        if(state=="fail" && definition.isEmpty())text<<"檢查未通過，請通知工程人員。";
        if(text.isEmpty())text<<"依腳本自動執行，不需操作。";
        const QString content=text.join('\n');if(m_guidance->toPlainText()!=content)m_guidance->setPlainText(content);
    } else {
        QString detail=e.title+" · "+stateName(e.state)+'\n'+e.detail+(e.observed.isEmpty()?QString():"\n結果："+e.observed);
        if(!definition.isEmpty())detail="所屬項目："+definition["name"].toString()+'\n'+detail;
        m_details->setText(detail);m_details->setToolTip(detail);
    }
}
bool FlowMonitor::selfTest() {
    TestFlow f;FlowStep a;a.name="啟動 APP";a.type="launch";FlowStep b;b.name="讀取狀態";b.type="readUIA";f.steps={a,b};
    FlowStep jump;
    jump.type = "loop";
    jump.targetStep = 1;
    jump.jumpCount = 500;
    TestFlow loopFlow;
    loopFlow.steps = {a, jump};
    setEngineering(true);
    setPlan(flowMonitorPlan(loopFlow));
    if (m_list->item(2)->data(DetailRole).toString() != "跳回第 1 步 · 設定 500 次"
        || rowState(2) != "pending") return false;
    m_list->setCurrentRow(2);
    if (!m_details->text().contains("跳回第 1 步 · 設定 500 次")) return false;
    setEngineering(false);
    setPlan(flowMonitorPlan(f));begin("等待啟動");if(currentRow()!=0)return false;
    applyEvent({{"event","start"},{"index",0}});if(rowState(0)!="pass" || currentRow()!=1)return false;
    applyEvent({{"event","finish"},{"index",0},{"result","PASS"}});
    applyEvent({{"event","start"},{"index",1}});
    applyEvent({{"event","finish"},{"index",1},{"result","FAIL"},{"value","找不到控制項"}});
    finish(false,false,"失敗");if(rowState(1)!="pass" || rowState(2)!="fail" || rowState(3)!="skipped")return false;
    setPlan(flowMonitorPlan(f));begin("等待");applyEvent({{"event","start"},{"index",0}});requestStop();finish(false,true,"手動停止");
    if(rowState(1)!="stopped" || rowState(2)!="skipped")return false;
    setPlan(flowMonitorPlan(f));begin("等待");applyEvent({{"event","start"},{"index",0}});finish(false,false,"worker crash");
    if(rowState(1)!="fail" || rowState(2)!="skipped")return false;
    f.testItems={{"group","確認設備正常","保持連接","正在檢查","檢查電源"}};
    for(auto &step:f.steps)step.testItemId="group";
    setEngineering(false);setPlan(flowMonitorPlan(f));begin("準備");
    if(testItemCount()!=1)return false;
    applyEvent({{"event","start"},{"index",0}});applyEvent({{"event","finish"},{"index",0},{"result","PASS"}});
    if(displayState(m_displayRows[1])!="running")return false; // One child passed does not pass the item.
    applyEvent({{"event","start"},{"index",1}});applyEvent({{"event","finish"},{"index",1},{"result","FAIL"},{"value","raw technical error"}});
    finish(false,false,"失敗");
    if(displayState(m_displayRows[1])!="fail" || !m_guidance->toPlainText().contains("檢查電源")
        || m_guidance->toPlainText().contains("raw technical error"))return false;
    setEngineering(true);if(m_displayRows.size()!=4 || testItemCount()!=1 || rowState(2)!="fail")return false;
    setEngineering(false);return testItemCount()==1 && displayState(m_displayRows[1])=="fail" && m_guidance->toPlainText().contains("檢查電源");
}
