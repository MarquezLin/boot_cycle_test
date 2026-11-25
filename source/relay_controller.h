#pragma once
#include <QString>
#include <QStringList>
#include <functional>

struct RelaySettings {
    bool normallyClosed = true; // COM-NC: flash-close; COM-NO: flash-break.
    QString port;
    QString usbId, deviceInstance;
    QString agentUrl;
    int baud = 9600, address = 1, channel = 1, ticks = 200, delaySeconds = 30;
};

RelaySettings loadRelaySettings(const QString &directory);
QString describeRelayCommand(const RelaySettings &settings, QString &error);
bool relaySettingsValid(const RelaySettings &settings, QString &error);
bool resolveRelayPort(RelaySettings &settings, QString &error, bool checkAccess = false);
bool relayAgentSettingsValid(const RelaySettings &settings, QString &error);
bool checkRelayAgent(const RelaySettings &settings, int &channel, QString &error, bool requireSerial = false,
    bool *retryable = nullptr);
bool scheduleRelayAgent(const RelaySettings &settings, QString &requestId, QString &error, bool &stoppedByAgent,
    bool *retryable = nullptr);
bool cancelRelayAgent(const RelaySettings &settings, const QString &requestId, QString &error);
QString relayAgentRequestState(const RelaySettings &settings, const QString &requestId, QString &error,
                               bool *retryable = nullptr);
bool sendRelayPulse(const RelaySettings &settings,
    const std::function<void(const QString &)> &log, QString &error);
bool setRelayPower(const RelaySettings &settings, bool powered,
    const std::function<void(const QString &)> &log, QString &error);
bool relaySelfTest();
bool relayDialogSelfTest();
class QWidget;
void showRelaySettings(const QString &directory, QWidget *parent);
inline constexpr quint16 relayDiscoveryPort = 45454;
inline constexpr char relayDiscoveryRequest[] = "GVFG-Relay-Discovery/1";
void showRelayAgentSettings(const QString &directory, QWidget *parent,
    const std::function<QStringList()> &discover);
