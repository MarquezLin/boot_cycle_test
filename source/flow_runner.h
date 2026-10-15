#pragma once
#include "flow_model.h"
#include <QImage>
#include <QThread>
#include <atomic>

struct FlowWindowInfo {quintptr handle; QString title; bool owned;};
QVector<FlowWindowInfo> flowWindows(const QString &executable, quint32 pid = 0);
quintptr findFlowWindow(const QString &executable, quint32 pid = 0, const QString &title = {});
QString flowWindowError(const QString &executable, quint32 pid = 0, const QString &title = {});
void requestFlowForeground(quintptr window);
quintptr flowWindowAtPoint(const QString &executable, const QPoint &point);
QImage captureFlowWindow(quintptr window, QString &error);
bool pickFlowControl(quintptr window, int screenX, int screenY, QString &name, QString &id, QString &error);
QPoint flowClientPoint(quintptr window, const QPoint &screenPoint);

class FlowRunner final : public QThread {
    Q_OBJECT
public:
    explicit FlowRunner(QObject *parent = nullptr) : QThread(parent) {}
    TestFlow flow;
    int singleStep = -1;
    QString initialValue, evidenceDirectory;
    void stop() { m_stop = true; }
signals:
    void progress(const QString &line);
    void stepStarted(int index);
    void stepObserved(int index, const QString &value);
    void windowResolved(int index, const QString &title);
    void stepFinished(int index, const QString &result, const QString &value);
    void valueRead(const QString &value);
    void completed(bool success, bool stopped);
protected:
    void run() override;
private:
    std::atomic_bool m_stop = false;
};
