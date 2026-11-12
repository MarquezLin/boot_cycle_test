#include "relay_controller.h"

#include <QDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

bool relayAgentSettingsValid(const RelaySettings &settings, QString &error)
{
    error.clear();
    const QUrl url(settings.agentUrl, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != "http" || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
        || (!url.path().isEmpty() && url.path() != "/") || url.port(80) <= 0)
        error = "請輸入控制端 的 HTTP 網址，例如 http://192.168.50.20:8080";
    else if (settings.delaySeconds < 15 || settings.delaySeconds > 3600
        || settings.ticks < 1 || settings.ticks > 65535)
        error = "網路排程的關機緩衝須為 15～3600 秒，斷電維持須為 0.1～6553.5 秒";
    return error.isEmpty();
}

static QJsonObject requestAgent(const RelaySettings &settings, const QString &id, bool cancel, QString &error,
    bool *stoppedByAgent = nullptr, bool *retryable = nullptr)
{
    const bool probe = id.isEmpty();
    QUrl url(settings.agentUrl);
    url.setPath(probe ? "/health" : "/power-cycle");
    QUrlQuery query;
    if (!probe) query.addQueryItem("id", id);
    if (!probe && !cancel) {
        query.addQueryItem("delay", QString::number(settings.delaySeconds));
        query.addQueryItem("ticks", QString::number(settings.ticks));
    }
    if (!probe) url.setQuery(query);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);

    for (int attempt = 0; attempt < (probe ? 1 : 2); ++attempt) {
        auto *reply = probe ? manager.get(request)
            : cancel ? manager.deleteResource(request) : manager.post(request, QByteArray());
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(&timeout, &QTimer::timeout, reply, [&] {
            timedOut = true;
            reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start(2500);
        if (!reply->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto result = QJsonDocument::fromJson(reply->readAll()).object();
        if (retryable) {
            const auto code = reply->error();
            *retryable = (status == 0 || status == 200) && (timedOut || code == QNetworkReply::ConnectionRefusedError
                || code == QNetworkReply::RemoteHostClosedError || code == QNetworkReply::HostNotFoundError
                || code == QNetworkReply::TimeoutError || code == QNetworkReply::TemporaryNetworkFailureError
                || code == QNetworkReply::NetworkSessionFailedError || code == QNetworkReply::UnknownNetworkError);
        }
        error = timedOut ? "連線逾時，請確認 Agent 已開始監聽、網路線已連接，且防火牆允許此連接埠。"
            : reply->error() == QNetworkReply::ConnectionRefusedError ? "對方拒絕連線，請確認 Agent 已開始監聽及連接埠正確。"
            : reply->error() == QNetworkReply::RemoteHostClosedError ? "控制端 在回覆前中斷連線。"
            : reply->error() == QNetworkReply::HostNotFoundError ? "找不到控制端，請確認網址或主機名稱。"
            : reply->errorString();
        delete reply;
        if (status == 200) {
            const int channel = result.value("channel").toInt();
            const bool confirmed = probe ? result.value("state").toString() == "online" && channel >= 1 && channel <= 4
                : result.value("id").toString() == id
                    && result.value("state").toString() == (cancel ? "cancelled" : "scheduled");
            if (confirmed) {
                if (!probe && !cancel && result.contains("remainingMs")
                    && result.value("remainingMs").toInteger(-1) < (settings.delaySeconds - 5) * 1000LL) {
                    if (retryable) *retryable = false;
                    error = "重連後確認排程時，剩餘關機緩衝不足，將嘗試取消排程。";
                    return {};
                }
                error.clear();
                return result;
            }
            if (!retryable || !*retryable) error = "回覆內容不符";
        }
        if (status >= 400 && !result.value("error").toString().isEmpty()) {
            error = result.value("error").toString();
            if (stoppedByAgent) *stoppedByAgent = status == 409 && result.value("code").toString() == "stop-requested";
            return {};
        }
    }
    error = "未收到可確認的控制端 回覆（" + error + "）";
    return {};
}

bool checkRelayAgent(const RelaySettings &settings, int &channel, QString &error, bool requireSerial, bool *retryable)
{
    if (retryable) *retryable = false;
    if (!relayAgentSettingsValid(settings, error)) return false;
    const auto reply = requestAgent(settings, {}, false, error, nullptr, retryable);
    channel = reply.value("channel").toInt();
    if (reply.isEmpty()) return false;
    if (requireSerial && !reply.value("serialReady").toBool()) {
        error = reply.value("serialError").toString("Agent 未提供 RS485 就緒狀態，請更新 Agent。");
        return false;
    }
    return true;
}

bool scheduleRelayAgent(const RelaySettings &settings, QString &requestId, QString &error, bool &stoppedByAgent, bool *retryable)
{
    stoppedByAgent = false;
    if (retryable) *retryable = false;
    if (!relayAgentSettingsValid(settings, error)) return false;
    if (requestId.isEmpty()) requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return !requestAgent(settings, requestId, false, error, &stoppedByAgent, retryable).isEmpty();
}

bool cancelRelayAgent(const RelaySettings &settings, const QString &requestId, QString &error)
{
    return !requestAgent(settings, requestId, true, error).isEmpty();
}

void showRelayAgentSettings(const QString &directory, QWidget *parent,
    const std::function<QStringList()> &discover)
{
    const auto initial = loadRelaySettings(directory);
    QDialog dialog(parent);
    dialog.setWindowTitle("控制端 連線設定");
    QFormLayout layout(&dialog);
    QComboBox url;
    QStringList discovered = discover();
    url.addItem("自動搜尋", QString());
    for (const auto &address : discovered) url.addItem(address, address);
    url.addItem("手動輸入網址", "manual");
    QLineEdit manual(initial.agentUrl);
    manual.setPlaceholderText("http://192.168.50.20:8080");
    if (!initial.agentUrl.isEmpty()) url.setCurrentIndex(url.count() - 1);
    manual.setEnabled(url.currentData().toString() == "manual");
    QSpinBox delay;
    delay.setRange(15, 3600);
    delay.setValue(initial.delaySeconds);
    delay.setSuffix(" 秒");
    QDoubleSpinBox duration;
    duration.setRange(0.1, 6553.5);
    duration.setDecimals(1);
    duration.setSingleStep(0.1);
    duration.setValue(initial.ticks / 10.0);
    duration.setSuffix(" 秒");
    QPushButton test("測試連線");
    QLabel status;
    status.setWordWrap(true);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout.addRow("控制端 網址", &url);
    layout.addRow("手動網址", &manual);
    layout.addRow("關機緩衝", &delay);
    layout.addRow("斷電維持", &duration);
    layout.addRow(&test);
    layout.addRow(&status);
    layout.addRow(&buttons);
    dialog.resize(440, dialog.sizeHint().height());

    auto current = [&] {
        RelaySettings settings;
        settings.agentUrl = url.currentData().toString();
        if (settings.agentUrl == "manual") settings.agentUrl = manual.text().trimmed();
        settings.delaySeconds = delay.value();
        settings.ticks = qRound(duration.value() * 10);
        return settings;
    };
    auto clearStatus = [&] { status.clear(); };
    QObject::connect(&manual, &QLineEdit::textChanged, &dialog, clearStatus);
    QObject::connect(&url, &QComboBox::currentIndexChanged, &dialog, [&] {
        clearStatus();
        manual.setEnabled(url.currentData().toString() == "manual");
    });
    QTimer refreshDiscovery;
    QObject::connect(&refreshDiscovery, &QTimer::timeout, &dialog, [&] {
        const auto latest = discover();
        if (latest == discovered) return;
        const QString selected = url.currentData().toString();
        discovered = latest;
        const QSignalBlocker blocker(&url);
        url.clear();
        url.addItem("自動搜尋", QString());
        for (const auto &address : discovered) url.addItem(address, address);
        if (!selected.isEmpty() && selected != "manual" && !discovered.contains(selected))
            url.addItem(selected, selected);
        url.addItem("手動輸入網址", "manual");
        url.setCurrentIndex(url.findData(selected));
        if (selected.isEmpty())
            status.setText(discovered.isEmpty() ? "正在搜尋控制端…" : "已找到控制端，可測試連線。");
    });
    refreshDiscovery.start(500);
    QObject::connect(&test, &QPushButton::clicked, &dialog, [&] {
        test.setEnabled(false);
        status.setText("正在連線…");
        int channel = 0;
        QString error;
        auto settings = current();
        const auto latest = discover();
        if (url.currentIndex() == 0 && latest.size() == 1) settings.agentUrl = latest.first();
        if (url.currentIndex() == 0 && latest.size() > 1) error = "找到多台 Agent，請從清單選擇一台。";
        else if (url.currentIndex() == 0 && latest.isEmpty()) error = "尚未找到 Agent，請確認同網段及 UDP 45454 已放行。";
        const bool ok = error.isEmpty() && checkRelayAgent(settings, channel, error);
        status.setText(ok ? QString("連線成功，已綁定 CH%1。").arg(channel) : "連線失敗：" + error);
        test.setEnabled(true);
    });
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto settings = current();
        QString error;
        if (url.currentIndex() != 0 && !relayAgentSettingsValid(settings, error)) { status.setText(error); return; }
        QSettings ini(directory + "/test_settings.ini", QSettings::IniFormat);
        ini.beginGroup("Relay");
        ini.setValue("AgentUrl", settings.agentUrl);
        ini.remove("AgentKey");
        ini.setValue("DelaySeconds", settings.delaySeconds);
        ini.setValue("DurationTicks", settings.ticks);
        ini.sync();
        if (ini.status() != QSettings::NoError) { status.setText("無法寫入 test_settings.ini"); return; }
        dialog.accept();
    });
    dialog.exec();
}
