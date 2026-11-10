#pragma once
#include "flow_model.h"
#include <QDialog>
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QLabel;
class FlowItemsDialog final : public QDialog {
public:
    FlowItemsDialog(const TestFlow &flow, int selectedStep, QWidget *parent=nullptr);
    TestFlow resultFlow();
    bool selfTest();
private:
    void store();
    void select(int row);
    void refreshList();
    void assign();
    TestFlow m_flow;
    int m_selected=-1;
    bool m_loading=false;
    QListWidget *m_list;
    QLineEdit *m_name,*m_version,*m_itemName;
    QPlainTextEdit *m_instruction,*m_running,*m_failure;
    QSpinBox *m_first,*m_last;
    QLabel *m_range;
};
