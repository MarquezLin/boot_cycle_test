#pragma once
#include "flow_model.h"
#include <QJsonObject>
#include <QWidget>
#include <QElapsedTimer>
#include <QVector>
#include <QMap>
class QLabel;
class QListWidget;
class QProgressBar;
class QTimer;
class QPlainTextEdit;

// The worker publishes the plan it actually loaded, rather than UI log guesses.
QJsonObject flowMonitorPlan(const TestFlow &flow);

class FlowMonitor final : public QWidget {
public:
    explicit FlowMonitor(QWidget *parent = nullptr);
    void setPlan(const QJsonObject &plan);
    void setTailDescription(const QString &description);
    void begin(const QString &preparation);
    void applyEvent(const QJsonObject &event);
    void phase(const QString &description);
    void requestStop();
    void finish(bool success, bool stopped, const QString &description);
    int currentRow() const { return m_active; }
    QString rowState(int row) const;
    bool selfTest();
    void setEngineering(bool enabled);
    bool engineering() const {return m_engineering;}
    bool hasResults() const {return m_started;}
    int testItemCount() const;
private:
    struct Entry {QString title, type, detail, observed, state = "pending"; qint64 elapsed = 0; QString itemId;};
    void activate(int row);
    void setState(int row, const QString &state, const QString &detail = {});
    void refresh();
    void showDetails();
    void rebuildDisplay();
    int displayRow(int rawRow) const;
    QString displayState(const QVector<int> &rows) const;
    QLabel *m_heading, *m_current, *m_details;
    QProgressBar *m_progress;
    QListWidget *m_list;
    QTimer *m_timer;
    QPlainTextEdit *m_guidance;
    QVector<QVector<int>> m_displayRows;
    QMap<QString,QJsonObject> m_testItems;
    QVector<Entry> m_entries;
    int m_active = -1;
    bool m_started = false;
    bool m_engineering = false;
    QString m_tail;
    QElapsedTimer m_elapsed;
};
