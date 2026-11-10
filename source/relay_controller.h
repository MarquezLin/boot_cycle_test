#pragma once
#include <QString>
#include <functional>

struct RelaySettings {
    bool enabled = false;
    bool normallyClosed = true; // COM-NC: flash-close; COM-NO: flash-break.
    QString port;
    int baud = 9600, address = 255, channel = 1, ticks = 200, delaySeconds = 15;
};

RelaySettings loadRelaySettings(const QString &directory);
QString describeRelayCommand(const RelaySettings &settings, QString &error);
bool relaySettingsValid(const RelaySettings &settings, QString &error);
bool sendRelayPulse(const RelaySettings &settings,
    const std::function<void(const QString &)> &log, QString &error);
bool relaySelfTest();
bool relayDialogSelfTest();
class QWidget;
void showRelaySettings(const QString &directory, QWidget *parent);
