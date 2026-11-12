#include "relay_controller.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QIcon>
#include <QPainter>
#include <QNetworkInterface>
#include <QNetworkDatagram>
#include <QUdpSocket>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QThread>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <array>
#include <memory>

namespace {
enum class Light { None, Gray, Green, Blue, Amber, Red };

QIcon statusLight(Light light)
{
    static const auto icons = [] {
        const QColor colors[]{Qt::transparent, QColor("#82909f"), QColor("#2ca76c"),
            QColor("#3786dd"), QColor("#dc941c"), QColor("#dc5050")};
        std::array<QIcon, 6> result;
        for (size_t i = 1; i < result.size(); ++i) {
            QPixmap dot(28, 28);
            dot.setDevicePixelRatio(2);
            dot.fill(Qt::transparent);
            QPainter painter(&dot);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors[i]);
            painter.drawEllipse(QRectF(2, 2, 10, 10));
            painter.end();
            result[i] = QIcon(dot);
        }
        return result;
    }();
    return icons[size_t(light)];
}

QString remainingTime(qint64 milliseconds)
{
    const auto seconds = qMax<qint64>(0, (milliseconds + 999) / 1000);
    return QString("%1:%2").arg(seconds / 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'));
}

class RelayAgent : public QDialog
{
public:
    explicit RelayAgent(bool autoListen)
    {
        setWindowTitle("GVFG Relay Agent（控制端）");
        resize(960, 600);
        auto *form = new QFormLayout(this);
        powerBanner.setTextFormat(Qt::PlainText);
        powerBanner.setWordWrap(true);
        powerBanner.setStyleSheet("QLabel { background: #fff1d1; color: #553700; border: 1px solid #dc941c;"
                                  " border-radius: 6px; padding: 10px; font-size: 19px; font-weight: 700; }");
        form->addRow(&powerBanner);
        bind.setEditable(true);
        populateAddresses(config.value("Bind", "192.168.50.20").toString());
        port.setRange(1, 65535);
        port.setValue(config.value("Port", 8080).toInt());
        form->addRow("控制端 IPv4", &bind);
        form->addRow(&refreshAddresses);
        form->addRow("HTTP 埠", &port);
        const auto legacy = loadRelaySettings(QCoreApplication::applicationDirPath());
        serialSettings = legacy;
        devices.setHorizontalHeaderLabels({"待測 PC IPv4", "負載接點", "排程狀態", "電腦名稱", "測試狀態", "已測／目標"});
        devices.horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        devices.setIconSize(QSize(14, 14));
        devices.setSelectionMode(QAbstractItemView::SingleSelection);
        devices.setSelectionBehavior(QAbstractItemView::SelectRows);
        for (int row = 0; row < devices.rowCount(); ++row) {
            const int channel = row + 1;
            const QString prefix = QString("Channels/%1/").arg(channel);
            const bool migrate = channel == legacy.channel;
            const QString address = config.value(prefix + "Peer",
                migrate ? config.value("Peer") : QVariant()).toString();
            devices.setVerticalHeaderItem(row, new QTableWidgetItem(QString("CH%1").arg(channel)));
            devices.setItem(row, 0, new QTableWidgetItem(address));
            auto *contact = new QComboBox;
            contact->addItem("COM–NC", true);
            contact->addItem("COM–NO", false);
            contact->setCurrentIndex(config.value(prefix + "NormallyClosed", legacy.normallyClosed).toBool() ? 0 : 1);
            devices.setCellWidget(row, 1, contact);
            auto *status = new QTableWidgetItem;
            status->setFlags(status->flags() & ~Qt::ItemIsEditable);
            devices.setItem(row, 2, status);
            for (int column : {3, 4, 5}) {
                auto *display = new QTableWidgetItem;
                display->setFlags(display->flags() & ~Qt::ItemIsEditable);
                devices.setItem(row, column, display);
            }
        }
        devices.setMinimumHeight(165);
        form->addRow(new QLabel("每列綁定一台待測 PC。IP 留空表示停用該通道。"));
        form->addRow(&devices);
        unbound.setTextFormat(Qt::PlainText);
        unbound.setWordWrap(true);
        form->addRow(&unbound);
        form->addRow(&startTest);
        form->addRow(&stopTest);
        serialStatus.setTextFormat(Qt::PlainText);
        serialStatus.setWordWrap(true);
        serialStatus.setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow("RS485 串口", &serialStatus);
        form->addRow(&serial);
        form->addRow(&listen);
        form->addRow(&cancel);
        form->addRow(&state);
        log.setReadOnly(true);
        log.setMaximumBlockCount(500);
        form->addRow(&log);

        countdown.setSingleShot(true);
        countdown.setTimerType(Qt::PreciseTimer);
        clock.start();
        server.setMaxPendingConnections(16);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = server.nextPendingConnection()) acceptSocket(socket);
        });
        connect(&countdown, &QTimer::timeout, this, [this] { dispatchNext(); });
        connect(&discovery, &QUdpSocket::readyRead, this, [this] { replyToDiscovery(); });
        connect(&presence, &QTimer::timeout, this, [this] {
            for (auto it = reports.begin(); it != reports.end();) {
                if (!routes.contains(it.key()) && clock.elapsed() - it->seen > 60000) it = reports.erase(it);
                else ++it;
            }
            updateUi();
        });
        presence.start(1000);
        connect(&serialPresence, &QTimer::timeout, this, [this] {
            refreshSerialStatus();
            updateUi();
        });
        serialPresence.start(3000);
        connect(&refreshAddresses, &QPushButton::clicked, this, [this] {
            populateAddresses(bind.currentText());
        });
        connect(&serial, &QPushButton::clicked, this, [this] {
            showRelaySettings(QCoreApplication::applicationDirPath(), this);
            serialSettings = loadRelaySettings(QCoreApplication::applicationDirPath());
            refreshSerialStatus();
        });
        connect(&listen, &QPushButton::clicked, this, [this] {
            if (server.isListening()) {
                server.close();
                discovery.close();
                reports.clear();
                for (auto *socket : server.findChildren<QTcpSocket *>()) socket->abort();
            } else {
                startListening();
            }
            updateUi();
        });
        connect(&devices, &QTableWidget::itemSelectionChanged, this, [this] { updateUi(); });
        connect(&startTest, &QPushButton::clicked, this, [this] { queueCommand("start"); });
        connect(&stopTest, &QPushButton::clicked, this, [this] { queueCommand("stop"); });
        connect(&cancel, &QPushButton::clicked, this, [this] {
            const QString id = pending.value(devices.currentRow() + 1);
            if (!id.isEmpty() && requests[id].state == "scheduled") cancelPending(id);
        });
        if (autoListen) startListening();
        refreshSerialStatus();
        updateUi();
    }

    ~RelayAgent() override
    {
        if (worker) worker->wait();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (!pending.isEmpty()) {
            log.appendPlainText("仍有排程。可選擇通道取消待執行排程，或等候送出指令。");
            event->ignore();
            return;
        }
        QDialog::closeEvent(event);
    }

    void reject() override
    {
        if (pending.isEmpty()) QDialog::reject();
    }

private:
    struct Route {
        int channel;
        bool normallyClosed;
    };
    struct Request {
        int channel;
        int delay;
        int ticks;
        qint64 deadline;
        QByteArray state;
    };
    struct SendResult { bool sent = false; QString error; QStringList log; };
    struct DeviceReport {
        QString name, state, command, id;
        qint64 seen = 0, commandUntil = 0;
        QString cycles;
    };

    bool onTestNetwork(const QHostAddress &address) const
    {
        if (!server.isListening()) return false;
        for (const auto &iface : QNetworkInterface::allInterfaces())
            for (const auto &entry : iface.addressEntries())
                if (entry.ip() == server.serverAddress() && entry.prefixLength() >= 0
                    && address.isInSubnet(entry.ip(), entry.prefixLength())) return true;
        return false;
    }

    void replyToDiscovery()
    {
        while (discovery.hasPendingDatagrams()) {
            const auto packet = discovery.receiveDatagram(128);
            if (packet.data() != relayDiscoveryRequest || !onTestNetwork(packet.senderAddress())) continue;
            const auto reply = QJsonDocument(QJsonObject{{"protocol", QLatin1String(relayDiscoveryRequest)},
                {"address", server.serverAddress().toString()}, {"port", int(server.serverPort())}}).toJson(QJsonDocument::Compact);
            discovery.writeDatagram(reply, packet.senderAddress(), packet.senderPort());
        }
    }

    bool online(const DeviceReport &report) const { return clock.elapsed() - report.seen < 7000; }

    void queueCommand(const QString &command)
    {
        const int row = devices.currentRow();
        if (row < 0 || !server.isListening()) return;
        const QHostAddress address(devices.item(row, 0)->text().trimmed());
        auto report = reports.find(address);
        if (!routes.contains(address) || report == reports.end() || !online(*report)) return;
        if (command == "start") {
            refreshSerialStatus();
            if (!serialReady) {
                log.appendPlainText("無法開始測試：" + serialStatusError);
                updateUi();
                return;
            }
        }
        report->command = command;
        report->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        report->commandUntil = clock.elapsed() + 15000;
        const QString pendingRequest = pending.value(row + 1);
        if (command == "stop" && !pendingRequest.isEmpty() && requests[pendingRequest].state == "scheduled")
            cancelPending(pendingRequest);
        log.appendPlainText(QString("CH%1 %2：已要求%3測試").arg(row + 1).arg(address.toString(),
            command == "start" ? QString("開始") : QString("停止")));
        updateUi();
    }

    static QStringList localIpv4()
    {
        QStringList addresses;
        for (const auto &address : QNetworkInterface::allAddresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol)
                addresses.append(address.toString());
        }
        addresses.removeDuplicates();
        addresses.sort();
        return addresses;
    }

    void populateAddresses(const QString &selected)
    {
        bind.clear();
        bind.addItems(localIpv4());
        bind.setCurrentText(selected);
    }

    void startListening()
    {
        const QHostAddress local(bind.currentText().trimmed());
        const auto settings = loadRelaySettings(QCoreApplication::applicationDirPath());
        QString error;
        if (local.protocol() != QAbstractSocket::IPv4Protocol || local == QHostAddress::AnyIPv4) {
            log.appendPlainText("請填入控制端 的 IPv4 位址。");
            return;
        }
        const QStringList addresses = localIpv4();
        if (!addresses.contains(local.toString())) {
            log.appendPlainText(QString("Windows 目前沒有 %1。請儲存乙太網路的 IPv4 設定、確認網路線已接好，再按「重新整理本機 IP」。目前可用：%2")
                .arg(local.toString(), addresses.join("、")));
            return;
        }
        QHash<QHostAddress, Route> configured;
        for (int row = 0; row < devices.rowCount(); ++row) {
            const QString address = devices.item(row, 0)->text().trimmed();
            if (address.isEmpty()) continue;
            const QHostAddress client(address);
            if (client.protocol() != QAbstractSocket::IPv4Protocol || client == QHostAddress::AnyIPv4)
                error = "請填入有效的待測 PC IPv4 位址";
            else if (configured.contains(client))
                error = "每台待測 PC 的 IP 只能使用一次";
            if (!error.isEmpty()) {
                log.appendPlainText(QString("CH%1：%2").arg(row + 1).arg(error));
                return;
            }
            configured.insert(client, {row + 1, isNormallyClosed(row)});
        }
        if (!server.listen(local, quint16(port.value()))) {
            log.appendPlainText(QString("無法監聽 %1:%2：%3").arg(local.toString()).arg(port.value()).arg(server.errorString()));
            return;
        }
        routes = std::move(configured);
        if (!discovery.bind(QHostAddress(QHostAddress::AnyIPv4), relayDiscoveryPort))
            log.appendPlainText("無法開啟 UDP 45454 自動搜尋：" + discovery.errorString() + "。可使用手動網址。");
        serialSettings = settings;
        auto resolved = serialSettings;
        if (!resolveRelayPort(resolved, error))
            log.appendPlainText("已開始監聽，可測試網路連線。接受冷開機排程前請完成 RS485 設定：" + error);
        refreshSerialStatus();
        config.setValue("Bind", local.toString());
        config.setValue("Port", port.value());
        config.remove("Key");
        for (int row = 0; row < devices.rowCount(); ++row) {
            const QString prefix = QString("Channels/%1/").arg(row + 1);
            config.setValue(prefix + "Peer", devices.item(row, 0)->text().trimmed());
            config.remove(prefix + "Key");
            config.setValue(prefix + "NormallyClosed", isNormallyClosed(row));
        }
        config.sync();
        if (config.status() != QSettings::NoError) log.appendPlainText("無法儲存控制端設定。");
    }

    bool isNormallyClosed(int row) const
    {
        return static_cast<QComboBox *>(devices.cellWidget(row, 1))->currentData().toBool();
    }

    void refreshSerialStatus()
    {
        const QString saved = serialSettings.port;
        if (saved.isEmpty() && serialSettings.usbId.isEmpty()) {
            serialReady = false;
            serialStatusError = "尚未設定 COM 埠，請先完成 RS485 設定";
            serialStatus.setText("● 尚未設定 COM 埠");
            serialStatus.setStyleSheet("font-weight: 600; color: #9a6411;");
            serialStatus.setToolTip("在「RS485 設定及測試」選擇 COM 埠後按 Save。");
            return;
        }
        auto detected = serialSettings;
        serialReady = resolveRelayPort(detected, serialStatusError, server.isListening() && worker == nullptr);
        QString text = serialReady ? "● 目前使用 " + detected.port : "● 串口未就緒：" + serialStatusError;
        if (serialReady && saved != detected.port && !saved.isEmpty()) text += "（原設定 " + saved + "）";
        if (!serialReady && !saved.isEmpty()) text += "（已儲存 " + saved + "）";
        if (!serialSettings.usbId.isEmpty()) text += " ｜ VID:PID " + serialSettings.usbId;
        serialStatus.setText(text);
        serialStatus.setStyleSheet(serialReady ? "font-weight: 600; color: #16804c;"
                                         : "font-weight: 600; color: #bd3c3c;");
        serialStatus.setToolTip(serialReady ? "已偵測到串口。可用「測試通訊（不斷電）」確認繼電器有回覆。" : serialStatusError);
    }

    void updateUi()
    {
        const bool running = server.isListening();
        const qint64 now = clock.elapsed();
        QStringList activePowerActions;
        bind.setEnabled(!running);
        refreshAddresses.setEnabled(!running);
        port.setEnabled(!running);
        devices.setEditTriggers(running ? QAbstractItemView::NoEditTriggers : QAbstractItemView::DoubleClicked);
        for (int row = 0; row < devices.rowCount(); ++row) {
            devices.cellWidget(row, 1)->setEnabled(!running);
            const QString id = pending.value(row + 1);
            auto *schedule = devices.item(row, 2);
            QString scheduleText = lastResult.value(row + 1, "待命");
            Light scheduleLight = scheduleText == "傳送未確認" ? Light::Red
                : scheduleText == "模組已確認" ? Light::Green : Light::Gray;
            if (!id.isEmpty()) {
                const auto &request = requests[id];
                scheduleText = request.state == "sending" ? "送出斷電指令"
                    : request.deadline <= now ? "等待 RS485" : remainingTime(request.deadline - now);
                scheduleLight = Light::Amber;
                activePowerActions.append(QString("CH%1  %2").arg(row + 1).arg(scheduleText));
            }
            schedule->setText(scheduleText);
            schedule->setIcon(statusLight(scheduleLight));
            schedule->setToolTip(!id.isEmpty() ? "倒數結束後送出 RS485 斷電指令。" : scheduleText);
            const QHostAddress address(devices.item(row, 0)->text().trimmed());
            const auto report = reports.constFind(address);
            devices.item(row, 3)->setText(report == reports.cend() ? QString() : report->name);
            QString label;
            Light light = Light::None;
            if (!address.isNull()) {
                label = "尚未報到";
                light = Light::Gray;
                if (report != reports.cend()) {
                    if (!online(*report)) label = "失聯";
                    else if (report->commandUntil > clock.elapsed() && !report->command.isEmpty()) label = "等待指令確認";
                    else if (report->state == "running") label = "測試中";
                    else if (report->state == "reconnecting") label = "等待重連";
                    else if (report->state == "stopping") label = "正在停止";
                    else if (report->state == "stopped") label = "已停止";
                    else if (report->state == "failed") label = "測試失敗";
                    else if (report->state == "passed") label = "測試通過";
                    else if (report->state == "shutdown") label = "已要求關機";
                    else label = "在線待命";
                    if (label == "測試失敗") light = Light::Red;
                    else if (label == "測試中") light = Light::Blue;
                    else if (label == "等待指令確認" || label == "正在停止" || label == "已要求關機" || label == "等待重連") light = Light::Amber;
                    else if (label == "在線待命" || label == "測試通過") light = Light::Green;
                }
            }
            devices.item(row, 4)->setText(label);
            devices.item(row, 4)->setIcon(statusLight(light));
            auto *cycles = devices.item(row, 5);
            cycles->setText(report == reports.cend() || report->cycles.isEmpty() ? "—" : report->cycles);
            cycles->setToolTip(report != reports.cend() && !online(*report)
                ? "待測端失聯，顯示最後一次回報的正式輪數。"
                : "本次正式測試已執行輪數／目標輪數。");
        }
        powerBanner.setText(activePowerActions.isEmpty() ? QString()
            : "已收到斷電排程 · 斷電倒數\n" + activePowerActions.join("   ｜   "));
        powerBanner.setVisible(!activePowerActions.isEmpty());
        QStringList discovered;
        for (auto it = reports.cbegin(); it != reports.cend(); ++it)
            if (!routes.contains(it.key()) && online(it.value()))
                discovered.append(it->name + " (" + it.key().toString() + ")");
        unbound.setText(discovered.isEmpty() ? QString() : "已發現未綁定：" + discovered.join("、"));
        unbound.setVisible(!discovered.isEmpty());
        serial.setEnabled(!running);
        listen.setEnabled(pending.isEmpty());
        const QString selected = pending.value(devices.currentRow() + 1);
        cancel.setEnabled(!selected.isEmpty() && requests[selected].state == "scheduled");
        const int row = devices.currentRow();
        const auto current = reports.constFind(row < 0 ? QHostAddress() : QHostAddress(devices.item(row, 0)->text().trimmed()));
        const bool reachable = running && current != reports.cend() && online(*current);
        const bool waiting = reachable && !current->command.isEmpty() && current->commandUntil > clock.elapsed();
        startTest.setEnabled(serialReady && reachable && !waiting && selected.isEmpty()
            && current->state != "running" && current->state != "reconnecting"
            && current->state != "stopping" && current->state != "shutdown");
        startTest.setToolTip(serialReady ? QString() : serialStatusError);
        stopTest.setEnabled(reachable && !(waiting && current->command == "stop"));
        listen.setText(running ? "停止監聽" : "開始監聽");
        state.setText(running ? QString("監聽中：%1:%2｜%3 台待測 PC｜%4 筆排程")
            .arg(server.serverAddress().toString()).arg(server.serverPort()).arg(routes.size()).arg(pending.size())
            : "尚未監聽");
    }

    void cancelPending(const QString &id)
    {
        auto &request = requests[id];
        request.state = "cancelled";
        pending.remove(request.channel);
        lastResult[request.channel] = "已取消";
        log.appendPlainText(QString("CH%1 %2 已取消").arg(request.channel).arg(id));
        updateUi();
        dispatchNext();
    }

    void dispatchNext()
    {
        countdown.stop();
        if (worker || pending.isEmpty()) return;
        QString id;
        qint64 deadline = 0;
        for (const auto &candidate : std::as_const(pending)) {
            const qint64 due = requests[candidate].deadline;
            if (id.isEmpty() || due < deadline) {
                id = candidate;
                deadline = due;
            }
        }
        const qint64 remaining = deadline - clock.elapsed();
        if (remaining > 0) {
            countdown.start(int(remaining));
            return;
        }
        auto &request = requests[id];
        request.state = "sending";
        auto settings = serialSettings;
        settings.channel = request.channel;
        settings.normallyClosed = contacts.value(request.channel);
        settings.ticks = request.ticks;
        auto result = std::make_shared<SendResult>();
        worker = QThread::create([settings, result] {
            auto resolved = settings;
            if (!resolveRelayPort(resolved, result->error)) return;
            result->sent = sendRelayPulse(resolved,
                [result](const QString &message) { result->log.append(message); }, result->error);
        });
        worker->setParent(this);
        connect(worker, &QThread::finished, this, [this, id, result] {
            worker->deleteLater();
            worker = nullptr;
            auto &finished = requests[id];
            const int channel = finished.channel;
            const QString prefix = QString("CH%1 %2 ").arg(channel).arg(id);
            for (const auto &message : std::as_const(result->log)) log.appendPlainText(prefix + message);
            finished.state = result->sent ? "sent" : "failed";
            lastResult[channel] = result->sent ? "模組已確認" : "傳送未確認";
            log.appendPlainText(prefix + (result->sent ? QString("模組已確認定時命令") : result->error));
            // A missing ACK does not prove the relay ignored the command.
            readyAfter[channel] = clock.elapsed() + qint64(finished.ticks) * 100;
            pending.remove(channel);
            updateUi();
            dispatchNext();
        });
        worker->start();
        updateUi();
    }

    static void respond(QTcpSocket *socket, int status, const QJsonObject &json)
    {
        const QByteArray body = QJsonDocument(json).toJson(QJsonDocument::Compact);
        socket->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK\r\n" : " Rejected\r\n")
            + "Connection: close\r\nContent-Type: application/json\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void acceptSocket(QTcpSocket *socket)
    {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (server.findChildren<QTcpSocket *>().size() > 16) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        socket->setReadBufferSize(4096);
        QTimer::singleShot(4000, socket, [socket] { socket->abort(); });
        auto receive = [this, socket] {
            if (socket->state() != QAbstractSocket::ConnectedState) return;
            const QByteArray bytes = socket->peek(4096);
            const qsizetype end = bytes.indexOf("\r\n\r\n");
            if (end < 0) {
                if (bytes.size() == 4096) respond(socket, 400, {{"error", "HTTP 標頭過長"}});
                return;
            }
            socket->read(end + 4);
            handleRequest(socket, bytes.left(end));
        };
        connect(socket, &QTcpSocket::readyRead, this, receive);
        receive();
    }

    void handleRequest(QTcpSocket *socket, const QByteArray &headers)
    {
        const auto lines = headers.split('\n');
        const auto first = lines.value(0).trimmed().split(' ');
        const auto method = first.value(0);
        const auto url = QUrl::fromEncoded(first.value(1), QUrl::StrictMode);
        if (first.size() == 3 && first.value(2) == "HTTP/1.1" && method == "POST"
            && url.isValid() && url.path() == "/report") {
            handleReport(socket, QUrlQuery(url));
            return;
        }
        const auto route = routes.constFind(socket->peerAddress());
        if (route == routes.cend()) {
            respond(socket, 403, {{"error", "待測 PC 的來源 IP " + socket->peerAddress().toString() + " 尚未綁定通道"}});
            return;
        }
        if (first.size() == 3 && first.value(2) == "HTTP/1.1" && method == "GET"
            && url.isValid() && url.path() == "/health" && !url.hasQuery()) {
            auto resolved = serialSettings;
            QString error;
            const bool ready = resolveRelayPort(resolved, error, worker == nullptr);
            respond(socket, 200, {{"state", "online"}, {"channel", route->channel},
                {"serialReady", ready}, {"serialError", error}, {"port", ready ? resolved.port : QString()}});
            return;
        }
        const QUrlQuery query(url);
        const QString id = query.queryItemValue("id");
        const QUuid uuid(id);
        if (first.size() != 3 || first.value(2) != "HTTP/1.1" || !url.isValid()
            || url.path() != "/power-cycle" || (method != "POST" && method != "DELETE" && method != "GET")
            || uuid.isNull() || id != uuid.toString(QUuid::WithoutBraces)) {
            respond(socket, 400, {{"error", "無效的排程請求"}});
            return;
        }

        auto previous = requests.find(id);
        const int channel = route->channel;
        if (previous != requests.end() && previous->channel != channel) {
            respond(socket, 404, {{"error", "找不到此排程"}});
            return;
        }
        if (method == "GET") {
            if (previous == requests.end())
                respond(socket, 404, {{"error", "找不到此排程"}});
            else
                respond(socket, 200, {{"id", id}, {"state", QString::fromLatin1(previous->state)}});
            return;
        }
        if (method == "DELETE") {
            if (previous == requests.end())
                previous = requests.insert(id, {channel, 0, 0, 0, "cancelled"});
            if (previous->state == "scheduled" && id == pending.value(channel)) cancelPending(id);
            if (previous != requests.end() && previous->state == "cancelled")
                respond(socket, 200, {{"id", id}, {"state", "cancelled"}});
            else
                respond(socket, 409, {{"error", "此排程已無法取消"}});
            return;
        }

        bool delayOk = false, ticksOk = false;
        const int delay = query.queryItemValue("delay").toInt(&delayOk);
        const int ticks = query.queryItemValue("ticks").toInt(&ticksOk);
        if (!delayOk || !ticksOk || delay < 15 || delay > 3600 || ticks < 1 || ticks > 65535) {
            respond(socket, 400, {{"error", "排程時間超出範圍"}});
            return;
        }
        const auto report = reports.constFind(socket->peerAddress());
        if (report != reports.cend() && report->command == "stop" && report->commandUntil > clock.elapsed()) {
            respond(socket, 409, {{"error", "控制端 正在停止此通道的測試"}, {"code", "stop-requested"}});
            return;
        }
        if (previous != requests.end()) {
            if (previous->state == "scheduled" && previous->delay == delay && previous->ticks == ticks)
                respond(socket, 200, {{"id", id}, {"state", "scheduled"},
                    {"remainingMs", qMax<qint64>(0, previous->deadline - clock.elapsed())}});
            else if (previous->state == "cancelled")
                respond(socket, 409, {{"error", "控制端已取消此排程"}, {"code", "stop-requested"}});
            else
                respond(socket, 409, {{"error", "此排程 ID 已使用"}});
            return;
        }
        if (pending.contains(channel) || clock.elapsed() < readyAfter.value(channel)) {
            respond(socket, 409, {{"error", "本通道仍有排程或正在等待復電"}});
            return;
        }
        QString serialError;
        auto resolved = serialSettings;
        if (!resolveRelayPort(resolved, serialError, worker == nullptr)) {
            respond(socket, 503, {{"error", "控制端 尚未完成 RS485 設定：" + serialError}});
            return;
        }

        requests.insert(id, {channel, delay, ticks, clock.elapsed() + qint64(delay) * 1000, "scheduled"});
        pending.insert(channel, id);
        contacts.insert(channel, route->normallyClosed);
        respond(socket, 200, {{"id", id}, {"state", "scheduled"}, {"remainingMs", delay * 1000}});
        log.appendPlainText(QString("CH%1 %2 已接受，%3 秒後送出命令，斷電維持 %4 秒")
            .arg(channel).arg(id).arg(delay).arg(ticks / 10.0));
        updateUi();
        dispatchNext();
    }

    void handleReport(QTcpSocket *socket, const QUrlQuery &query)
    {
        const auto address = socket->peerAddress();
        const auto route = routes.constFind(address);
        if (route == routes.cend() && (!onTestNetwork(address) || (!reports.contains(address) && reports.size() >= 32))) {
            respond(socket, 403, {{"error", "不在測試網段或報到清單已滿"}});
            return;
        }
        auto &report = reports[address];
        report.name = query.queryItemValue("name", QUrl::FullyDecoded).left(64).simplified();
        if (report.name.isEmpty()) report.name = address.toString();
        report.state = query.queryItemValue("state").left(16);
        report.seen = clock.elapsed();
        bool testedOk = false, targetOk = false;
        const auto tested = query.queryItemValue("tested").toULongLong(&testedOk);
        const auto target = query.queryItemValue("target").toULongLong(&targetOk);
        if (testedOk && targetOk && target <= 999999)
            report.cycles = QString("%1 / %2").arg(tested).arg(target == 0 ? QString("不限") : QString::number(target));
        else
            report.cycles.clear();
        if (!report.command.isEmpty() && query.queryItemValue("ack") == report.id) {
            const auto result = query.queryItemValue("result");
            log.appendPlainText(QString("%1：%2").arg(address.toString(), result == "started" ? QString("已開始測試")
                : result == "stopping" ? QString("已收到停止確認") : QString("指令未執行，請查看待測 PC")));
            report.command.clear();
        }
        if (!report.command.isEmpty() && report.commandUntil <= report.seen) {
            log.appendPlainText(address.toString() + "：指令確認逾時");
            report.command.clear();
        }
        QJsonObject reply{{"channel", route == routes.cend() ? 0 : route->channel}};
        if (route != routes.cend() && report.command == "start") {
            refreshSerialStatus();
            if (!serialReady) {
                log.appendPlainText(address.toString() + "：已取消開始測試指令，" + serialStatusError);
                report.command.clear();
            }
        }
        if (route != routes.cend() && !report.command.isEmpty()) {
            reply.insert("id", report.id);
            reply.insert("command", report.command);
        }
        respond(socket, 200, reply);
        updateUi();
    }

    QSettings config{QCoreApplication::applicationDirPath() + "/relay_agent.ini", QSettings::IniFormat};
    QComboBox bind;
    QTableWidget devices{4, 6};
    QSpinBox port;
    QPushButton refreshAddresses{"重新整理本機 IP"}, serial{"RS485 設定及測試"}, listen, cancel{"取消待執行排程"};
    QPushButton startTest{"開始所選 PC 測試"}, stopTest{"停止所選 PC 測試"};
    QLabel state, unbound, powerBanner, serialStatus;
    QPlainTextEdit log;
    QTcpServer server;
    QUdpSocket discovery;
    QTimer countdown, presence, serialPresence;
    QElapsedTimer clock;
    QHash<QHostAddress, Route> routes;
    QHash<QHostAddress, DeviceReport> reports;
    QHash<QString, Request> requests;
    QHash<int, QString> pending, lastResult;
    QHash<int, bool> contacts;
    QHash<int, qint64> readyAfter;
    QThread *worker = nullptr;
    RelaySettings serialSettings;
    bool serialReady = false;
    QString serialStatusError;
};
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    RelayAgent window(app.arguments().contains("--listen"));
    window.show();
    return app.exec();
}
