#pragma once
#include "flow_model.h"
#include <QDialog>
#include <QVector>
#include <atomic>
#include <memory>
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QLabel;
class QPushButton;
class QProcess;
class QCloseEvent;

struct AppControl {
    QString name, id, value, selector, windowTitle, reason;
    int controlType=0;
    bool invoke=false, readable=false, unique=false, enabled=true;
    QStringList options;
};
struct AppScanResult {
    QVector<AppControl> controls;
    int examined=0;
    QString error;
};
AppScanResult scanAppControls(quintptr window, const std::atomic_bool &cancel);
int runAppScannerWorker(quintptr window);
AppScanResult readAppScanResult(const QByteArray &data);
bool appScannerSelfTest();

class AppScannerDialog final : public QDialog {
public:
    AppScannerDialog(const TestFlow &base, QWidget *parent=nullptr);
    ~AppScannerDialog() override;
    TestFlow draft() const {return m_draft;}
    int insertionIndex() const;
    bool uiSelfTest();
protected:
    void reject() override;
    void closeEvent(QCloseEvent *event) override;
private:
    void refreshWindows();
    void scan();
    void populate(const AppScanResult &result);
    void select(int row);
    void store();
    void move(int direction);
    void generate();
    void updatePreview();
    struct Choice {AppControl control;QString title,expected,instruction,runningText,failureHint;bool click=false,checked=false;QString condition="contains",extraction;bool autoTitle=true;};
    TestFlow m_base,m_draft;
    QVector<Choice> m_choices;
    QComboBox *m_windows;
    QComboBox *m_position;
    QComboBox *m_condition,*m_number;
    QComboBox *m_option;
    QLabel *m_optionLabel;
    QTableWidget *m_table;
    QLineEdit *m_title,*m_expected;
    QPlainTextEdit *m_instruction,*m_failure;
    QLabel *m_status,*m_binding;
    QLabel *m_numberLabel,*m_expectedLabel,*m_preview;
    QPushButton *m_scan,*m_refresh,*m_generate;
    QProcess *m_worker = nullptr;
    QString m_scanError;
    int m_selected=-1;
    bool m_loading=false,m_busy=false;
};
