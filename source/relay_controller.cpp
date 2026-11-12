#include "relay_controller.h"
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devguid.h>
#include <QHash>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStringList>
#include <vector>
#include <algorithm>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QThread>
#include <QTimer>
#include <QTemporaryDir>
#include <QApplication>
#include <memory>

namespace {
struct UsbSerialDevice {
    QString name, usbId, instance;
};

QHash<QString, UsbSerialDevice> usbSerialDevices()
{
    QHash<QString, UsbSerialDevice> result;
    const HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return result;
    const QRegularExpression idPattern("VID_([0-9A-F]{4})[&+]PID_([0-9A-F]{4})",
        QRegularExpression::CaseInsensitiveOption);
    for (DWORD i = 0; ; ++i) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, i, &device)) break;
        const HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t port[64]{};
        DWORD bytes = sizeof(port);
        const auto read = RegGetValueW(key, nullptr, L"PortName", RRF_RT_REG_SZ, nullptr, port, &bytes);
        RegCloseKey(key);
        if (read != ERROR_SUCCESS) continue;
        wchar_t instance[512]{}, name[256]{};
        SetupDiGetDeviceInstanceIdW(devices, &device, instance, DWORD(std::size(instance)), nullptr);
        SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_FRIENDLYNAME, nullptr,
            reinterpret_cast<PBYTE>(name), sizeof(name), nullptr);
        const QString instanceId = QString::fromWCharArray(instance);
        auto id = idPattern.match(instanceId);
        if (!id.hasMatch()) {
            wchar_t hardwareIds[1024]{};
            if (SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<PBYTE>(hardwareIds), sizeof(hardwareIds), nullptr))
                id = idPattern.match(QString::fromWCharArray(hardwareIds));
        }
        if (id.hasMatch())
            result.insert(QString::fromWCharArray(port).toUpper(), {QString::fromWCharArray(name),
                (id.captured(1) + ":" + id.captured(2)).toUpper(), instanceId});
    }
    SetupDiDestroyDeviceInfoList(devices);
    return result;
}

QStringList availableSerialPorts(QString &error)
{
    // Enumerate current DOS device names without opening or probing serial ports.
    std::vector<wchar_t> buffer(32768);
    for (;;) {
        if (QueryDosDeviceW(nullptr, buffer.data(), DWORD(buffer.size()))) break;
        const DWORD code = GetLastError();
        if (code != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 1048576) {
            error = QString("無法列出串口，Windows error=%1").arg(code); return {};
        }
        buffer.resize(buffer.size() * 2);
    }
    QStringList ports;
    const QRegularExpression pattern("^COM[1-9][0-9]*$", QRegularExpression::CaseInsensitiveOption);
    for (const wchar_t *name = buffer.data(); *name; name += wcslen(name) + 1) {
        const auto port = QString::fromWCharArray(name).toUpper();
        if (pattern.match(port).hasMatch()) ports.append(port);
    }
    ports.removeDuplicates();
    std::sort(ports.begin(), ports.end(), [](const QString &a, const QString &b) {
        return a.mid(3).toULongLong() < b.mid(3).toULongLong();
    });
    return ports;
}

quint16 crc(const QByteArray &bytes)
{
    quint16 value = 0xffff;
    for (const char byte : bytes) {
        value ^= static_cast<unsigned char>(byte);
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (value >> 1) ^ 0xa001 : value >> 1;
    }
    return value;
}
QByteArray withCrc(QByteArray bytes)
{
    const auto value = crc(bytes);
    bytes.append(char(value & 0xff)); bytes.append(char(value >> 8));
    return bytes;
}
void word(QByteArray &bytes, int value)
{
    bytes.append(char(value >> 8)); bytes.append(char(value & 0xff));
}
bool validate(const RelaySettings &s, QString &error, bool requirePort)
{
    error.clear();
    if (requirePort && !QRegularExpression("^COM[1-9][0-9]*$", QRegularExpression::CaseInsensitiveOption).match(s.port).hasMatch())
        error = "請輸入 COM port，例如 COM3";
    else if (s.baud != 4800 && s.baud != 9600 && s.baud != 19200) error = "鮑率須為 4800、9600 或 19200";
    else if (s.address < 1 || s.address > 255) error = "設備位址須為 1～255";
    else if (s.channel < 1 || s.channel > 4) error = "此四路模組僅支援通道 1～4";
    else if (s.ticks < 1 || s.ticks > 65535) error = "維持時間須為 0.1～6553.5 秒";
    return error.isEmpty();
}
QByteArray request(const RelaySettings &s)
{
    QByteArray bytes; bytes.append(char(s.address)); bytes.append(char(0x10));
    word(bytes, 3 + (s.channel - 1) * 5); word(bytes, 2);
    // Pulse direction follows the load contact: interrupt power, then restore it.
    bytes.append(char(4)); word(bytes, s.normallyClosed ? 4 : 2); word(bytes, s.ticks);
    return withCrc(bytes);
}
bool acceptReply(const RelaySettings &s, const QByteArray &reply, QString &error)
{
    if (reply.size() < 5 || crc(reply) != 0) {error = "回覆長度不足或 CRC 錯誤"; return false;}
    if (static_cast<unsigned char>(reply[0]) != s.address) {error = "回覆設備位址不符"; return false;}
    if (static_cast<unsigned char>(reply[1]) == 0x90 && reply.size() == 5) {
        error = QString("Modbus exception 0x%1").arg(static_cast<unsigned char>(reply[2]), 2, 16, QChar('0'));
        return false;
    }
    if (reply != withCrc(request(s).left(6))) {error = "回覆功能碼、寄存器或數量不符"; return false;}
    return true;
}

QByteArray statusRequest(const RelaySettings &s)
{
    QByteArray bytes; bytes.append(char(s.address)); bytes.append(char(1));
    word(bytes, 0); word(bytes, 8); // Match the supplied module's read-status command.
    return withCrc(bytes);
}
QByteArray coilRequest(const RelaySettings &s, bool on)
{
    QByteArray bytes;
    bytes.append(char(s.address));
    bytes.append(char(5));
    word(bytes, s.channel - 1);
    word(bytes, on ? 0xff00 : 0);
    return withCrc(bytes);
}
bool acceptStatusReply(const RelaySettings &s, const QByteArray &reply, unsigned char &states, QString &error)
{
    if (reply.size() < 5) {error = "回覆不完整"; return false;}
    if (crc(reply) != 0) {error = "回覆 CRC 錯誤"; return false;}
    if (static_cast<unsigned char>(reply[0]) != s.address) {error = "回覆設備位址不符"; return false;}
    if (static_cast<unsigned char>(reply[1]) == 0x81 && reply.size() == 5) {
        error = QString("Modbus exception 0x%1").arg(static_cast<unsigned char>(reply[2]), 2, 16, QChar('0'));
        return false;
    }
    if (reply.size() != 6 || reply[1] != char(1) || reply[2] != char(1)) {
        error = "讀取狀態回覆的功能碼、長度或資料數量不符"; return false;
    }
    states = static_cast<unsigned char>(reply[3]); return true;
}
}

RelaySettings loadRelaySettings(const QString &directory)
{
    QSettings ini(directory + "/test_settings.ini", QSettings::IniFormat);
    ini.beginGroup("Relay"); RelaySettings s;
    s.port = ini.value("Port").toString().trimmed().toUpper();
    s.usbId = ini.value("UsbId").toString();
    s.deviceInstance = ini.value("DeviceInstance").toString();
    s.baud = ini.value("Baud", 9600).toInt(); s.address = ini.value("Address", 1).toInt();
    s.channel = ini.value("Channel", 1).toInt(); // Legacy Mode is ignored.
    s.normallyClosed = ini.value("NormallyClosed", true).toBool();
    s.ticks = ini.value("DurationTicks", 200).toInt();
    s.agentUrl = ini.value("AgentUrl", "http://192.168.50.20:8080").toString();
    s.delaySeconds = ini.value("DelaySeconds", 30).toInt(); return s;
}

bool relaySettingsValid(const RelaySettings &settings, QString &error)
{
    return validate(settings, error, true);
}

bool resolveRelayPort(RelaySettings &settings, QString &error, bool checkAccess)
{
    error.clear();
    const auto ports = availableSerialPorts(error);
    if (!error.isEmpty()) return false;
    if (!settings.usbId.isEmpty()) {
        const auto devices = usbSerialDevices();
        QStringList matching;
        QString exact;
        for (auto it = devices.cbegin(); it != devices.cend(); ++it) {
            if (it->usbId != settings.usbId || !ports.contains(it.key())) continue;
            matching.append(it.key());
            if (!settings.deviceInstance.isEmpty() && it->instance == settings.deviceInstance) exact = it.key();
        }
        if (!exact.isEmpty()) settings.port = exact;
        else if (matching.size() == 1) settings.port = matching.first();
        else {
            error = matching.isEmpty() ? "未偵測到已設定的 USB 轉接器 " + settings.usbId
                : "找到多個相同 VID/PID 的 USB 轉接器，請重新選擇 COM 埠";
            return false;
        }
    }
    if (!relaySettingsValid(settings, error)) return false;
    if (!ports.contains(settings.port)) {
        error = settings.port + " 未偵測到，請確認轉接器已插入";
        return false;
    }
    if (!checkAccess) return true;
    const auto path = (QStringLiteral("\\\\.\\") + settings.port).toStdWString();
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        error = QString("無法開啟 %1（Windows error=%2）").arg(settings.port).arg(code);
        if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION)
            error += "，請關閉其他占用串口的工具";
        return false;
    }
    CloseHandle(handle);
    return true;
}

QString describeRelayCommand(const RelaySettings &settings, QString &error)
{
    if (!validate(settings, error, false)) return {};
    return QString::fromLatin1(request(settings).toHex(' ').toUpper());
}

namespace {
bool configureSerial(HANDLE handle, const RelaySettings &s, QString &error)
{
    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle, &dcb)) {error = "無法讀取序列埠設定"; return false;}
    dcb.BaudRate = s.baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fDsrSensitivity = FALSE; dcb.fOutX = FALSE; dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE; dcb.fNull = FALSE; dcb.fRtsControl = RTS_CONTROL_DISABLE; dcb.fAbortOnError = FALSE;
    COMMTIMEOUTS timeouts{}; timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 50; timeouts.WriteTotalTimeoutConstant = 2000;
    if (!SetCommState(handle, &dcb) || !SetCommTimeouts(handle, &timeouts)) {
        error = "無法設定序列埠 8N1"; return false;
    }
    return true;
}
bool exchange(const RelaySettings &s, const QByteArray &bytes, int normalReplySize,
    const std::function<void(const QString &)> &log, QByteArray &reply, QString &error, HANDLE existing = INVALID_HANDLE_VALUE)
{
    if (!validate(s, error, true)) return false;
    const std::wstring port = (QString("\\\\.\\") + s.port).toStdWString();
    HANDLE handle = existing != INVALID_HANDLE_VALUE ? existing : CreateFileW(port.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {error = QString("無法開啟 %1，Windows error=%2").arg(s.port).arg(GetLastError()); return false;}
    struct ClosePort {HANDLE value; ~ClosePort() {if (value != INVALID_HANDLE_VALUE) CloseHandle(value);}} close{existing == INVALID_HANDLE_VALUE ? handle : INVALID_HANDLE_VALUE};
    // A persistent port was configured when opened. Keep its line settings
    // stable between transactions, just as the vendor tool does.
    if (existing == INVALID_HANDLE_VALUE && !configureSerial(handle, s, error)) return false;
    if (!PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR)) {
        error = "無法清除序列埠緩衝區"; return false;
    }
    log(QString("RELAY TX %1 %2 8N1: %3")
        .arg(s.port).arg(s.baud).arg(QString::fromLatin1(bytes.toHex(' ').toUpper())));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.constData(), DWORD(bytes.size()), &written, nullptr) || written != DWORD(bytes.size())) {
        error = "傳送失敗或僅傳送部分資料，不重送"; return false;
    }
    QElapsedTimer timer; timer.start(); reply.clear();
    while (timer.elapsed() < 2000) {
        char data[32]; DWORD count = 0;
        if (!ReadFile(handle, data, sizeof(data), &count, nullptr)) {error = "讀取回覆失敗，不重送"; return false;}
        reply.append(data, int(count));
        if (reply.size() >= 2) {
            const int expected = static_cast<unsigned char>(reply[1]) == (static_cast<unsigned char>(bytes[1]) | 0x80) ? 5 : normalReplySize;
            if (reply.size() >= expected) break;
        }
    }
    log("RELAY RX: " + QString::fromLatin1(reply.toHex(' ').toUpper()));
    if (reply.isEmpty()) {error = "2 秒內未收到模組回覆；請確認供電、UART TX/RX 交叉與共地（或 RS485 A/B）、鮑率及設備位址"; return false;}
    return true;
}
}

bool sendRelayPulse(const RelaySettings &s, const std::function<void(const QString &)> &log, QString &error)
{
    QByteArray reply;
    if (!exchange(s, request(s), 8, log, reply, error) || !acceptReply(s, reply, error)) {
        error += "；模組可能已動作，不重送"; return false;
    }
    log("RELAY ACK: timed pulse accepted; this does not prove AC loss or restored power");
    return true;
}

namespace {
struct StatusResult {bool success = false; QString error, details; unsigned char states = 0;};
StatusResult readStatus(const RelaySettings &s, HANDLE existing = INVALID_HANDLE_VALUE)
{
    StatusResult result; QByteArray reply;
    // This path can only send function 0x01; it never sends a relay write command.
    result.success = exchange(s, statusRequest(s), 6,
        [&](const QString &line) {result.details += line + '\n';}, reply, result.error, existing)
        && acceptStatusReply(s, reply, result.states, result.error);
    return result;
}
}

bool relaySelfTest()
{
    RelaySettings s; QString error;
    if (coilRequest(s, false) != QByteArray::fromHex("010500000000CDCA")
        || coilRequest(s, true) != QByteArray::fromHex("01050000FF008C3A")) return false;
    s.address = 255;
    unsigned char states = 0;
    if (statusRequest(s) != QByteArray::fromHex("FF01000000082812")) return false;
    if (!acceptStatusReply(s, QByteArray::fromHex("FF010101A1A0"), states, error) || states != 1) return false;
    for (int value : {0, 1, 5, 15, 255}) {
        QByteArray bytes = QByteArray::fromHex("FF0101"); bytes.append(char(value));
        if (!acceptStatusReply(s, withCrc(bytes), states, error) || states != value) return false;
    }
    if (acceptStatusReply(s, QByteArray::fromHex("FF010101A100"), states, error)
        || acceptStatusReply(s, withCrc(QByteArray::fromHex("FE010100")), states, error)
        || acceptStatusReply(s, withCrc(QByteArray::fromHex("FF8102")), states, error)
        || acceptStatusReply(s, statusRequest(s), states, error)
        || acceptStatusReply(s, QByteArray::fromHex("FF0101"), states, error)
        || acceptStatusReply(s, withCrc(QByteArray::fromHex("FF1000030002")), states, error)) return false;
    s.ticks = 20;
    if (request(s) != QByteArray::fromHex("FF10000300020400040014C59F")) return false;
    s.normallyClosed = false; s.ticks = 30;
    if (request(s) != QByteArray::fromHex("FF1000030002040002001EA599")) return false;
    s.ticks = 200;
    if (request(s) != QByteArray::fromHex("FF100003000204000200C82407")) return false;
    s.normallyClosed = true; s.ticks = 200;
    if (request(s) != QByteArray::fromHex("FF100003000204000400C8C406")) return false;
    const auto ack = QByteArray::fromHex("FF1000030002A416");
    if (!acceptReply(s, ack, error)) return false;
    auto bad = ack; bad[7] = char(0);
    if (acceptReply(s, bad, error)) return false;
    if (acceptReply(s, withCrc(QByteArray::fromHex("FF9002")), error)) return false;
    s.channel = 2;
    if (acceptReply(s, ack, error) || request(s).mid(2, 2) != QByteArray::fromHex("0008")) return false;
    for (int channel = 1; channel <= 4; ++channel) {
        s.channel = channel;
        s.ticks = 65535;
        if (request(s).mid(7, 2) != QByteArray::fromHex("0004")
            || crc(request(s)) != 0 || !acceptReply(s, withCrc(request(s).left(6)), error)) return false;
    }
    s.port = "COM3"; s.channel = 5;
    if (validate(s, error, true)) return false;
    s.channel = 1; s.ticks = 0;
    if (validate(s, error, true)) return false;
    s.ticks = 200; s.address = 0;
    if (validate(s, error, true)) return false;
    s.address = 255;
    return validate(s, error, true);
}

void showRelaySettings(const QString &directory, QWidget *parent)
{
    const auto initial = loadRelaySettings(directory);
    QDialog dialog(parent); dialog.setWindowTitle("UART／RS485 冷開機電源設定");
    auto *layout = new QFormLayout(&dialog);
    auto *note = new QLabel("此處設定 RS485 與手動測試。正式排程的關機緩衝及斷電維持時間由待測端指定。", &dialog);
    note->setWordWrap(true); layout->addRow(note);
    QComboBox port(&dialog); port.setObjectName("relayPort");
    QPushButton refreshPorts("重新整理", &dialog); refreshPorts.setObjectName("relayRefreshPorts");
    QHBoxLayout portRow; portRow.setContentsMargins(0, 0, 0, 0);
    portRow.addWidget(&port, 1); portRow.addWidget(&refreshPorts); layout->addRow("COM port", &portRow);
    auto populatePorts = [&] {
        auto resolved = initial;
        QString resolveError;
        const bool detected = resolveRelayPort(resolved, resolveError);
        QString selected = port.currentData().toString();
        if (selected.isEmpty() && (detected || initial.usbId.isEmpty())) selected = resolved.port;
        QString error; const auto ports = availableSerialPorts(error);
        const auto usb = usbSerialDevices();
        if (initial.port.isEmpty() && initial.usbId.isEmpty() && selected.isEmpty() && usb.size() == 1)
            selected = usb.cbegin().key();
        QSignalBlocker block(port); port.clear();
        QString hint = ports.isEmpty() ? "未偵測到串口，請插入後重新整理" : "請選擇串口";
        if (!error.isEmpty()) hint = error;
        else if (selected.isEmpty() && !initial.usbId.isEmpty()) hint = resolveError;
        else if (!selected.isEmpty() && !ports.contains(selected)) hint = selected + " 未連接，請重新選擇";
        port.addItem(hint, QString());
        for (const auto &name : ports) {
            const auto adapter = usb.value(name);
            port.addItem(adapter.usbId.isEmpty() ? name
                : QString("%1  %2  [VID:PID %3]").arg(name, adapter.name, adapter.usbId), name);
        }
        port.setCurrentIndex(qMax(0, port.findData(selected)));
        port.setToolTip("列出目前 Windows 串口；是否可開啟及模組是否回應，請按測試通訊。");
    };
    populatePorts();
    QComboBox baud(&dialog); for (int value : {4800, 9600, 19200}) baud.addItem(QString::number(value), value);
    baud.setCurrentIndex(baud.findData(initial.baud)); layout->addRow("鮑率（8N1）", &baud);
    QSpinBox address(&dialog); address.setRange(1, 255); address.setValue(initial.address); layout->addRow("設備位址", &address);
    QSpinBox channel(&dialog); channel.setRange(1, 4); channel.setValue(initial.channel); layout->addRow("繼電器通道", &channel);
    QComboBox contact(&dialog); contact.setObjectName("relayContact");
    contact.addItem("COM–NC（平常釋放；吸合斷電後釋放復電）", true);
    contact.addItem("COM–NO（平常吸合；釋放斷電後吸合復電）", false);
    contact.setCurrentIndex(initial.normallyClosed ? 0 : 1); layout->addRow("負載接點", &contact);
    QDoubleSpinBox duration(&dialog); duration.setRange(0.1, 6553.5); duration.setDecimals(1); duration.setSingleStep(0.1);
    const QSettings saved(directory + "/test_settings.ini", QSettings::IniFormat);
    duration.setValue(saved.value("Relay/ManualDurationTicks", initial.ticks).toInt() / 10.0);
    duration.setSuffix(" 秒");
    layout->addRow("手動定時測試維持時間", &duration);
    QLabel preview(&dialog); preview.setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addRow("傳送預覽（不送出）", &preview);
    auto current = [&] {RelaySettings s; s.port = port.currentData().toString();
        s.baud = baud.currentData().toInt(); s.address = address.value(); s.channel = channel.value();
        s.normallyClosed = contact.currentData().toBool();
        s.ticks = qRound(duration.value() * 10); return s;};
    auto update = [&] {QString error; const auto command = describeRelayCommand(current(), error); preview.setText(error.isEmpty() ? command : error);};
    QObject::connect(&baud, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(&address, &QSpinBox::valueChanged, &dialog, update);
    QObject::connect(&channel, &QSpinBox::valueChanged, &dialog, update);
    QObject::connect(&contact, &QComboBox::currentIndexChanged, &dialog, update);
    QObject::connect(&duration, &QDoubleSpinBox::valueChanged, &dialog, update); update();
    QPushButton testButton("測試通訊（不斷電）", &dialog); layout->addRow(&testButton);
    testButton.setObjectName("relayCommunicationTest");
    QLabel testResult("尚未測試；請設定 COM port、鮑率及設備位址後按測試。", &dialog);
    testResult.setObjectName("relayCommunicationResult"); testResult.setWordWrap(true);
    testResult.setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addRow(&testResult);
    QPlainTextEdit communicationLog(&dialog); communicationLog.setReadOnly(true); communicationLog.setMaximumHeight(90);
    communicationLog.setPlaceholderText("通訊測試將顯示傳送 TX 與接收 RX"); layout->addRow(&communicationLog);
    struct Connection {
        HANDLE handle = INVALID_HANDLE_VALUE;
        void close() {if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); handle = INVALID_HANDLE_VALUE;}
        ~Connection() {close();}
    } connection;
    QPushButton connectPort("連線", &dialog), disconnectPort("斷線", &dialog);
    connectPort.setObjectName("relayConnect"); disconnectPort.setObjectName("relayDisconnect");
    QLabel connectionLabel("串口未連線", &dialog);
    QHBoxLayout connectionRow; connectionRow.addWidget(&connectPort); connectionRow.addWidget(&disconnectPort); connectionRow.addWidget(&connectionLabel);
    layout->insertRow(3, &connectionRow);
    QPushButton pulseNc("吸合 → 計時 → 釋放", &dialog), pulseNo("釋放 → 計時 → 吸合", &dialog);
    pulseNc.setObjectName("relayPulseNc"); pulseNo.setObjectName("relayPulseNo");
    QHBoxLayout manualRow; manualRow.addWidget(&pulseNc); manualRow.addWidget(&pulseNo); layout->addRow("選定通道測試", &manualRow);
    QPushButton turnOn("開（吸合）", &dialog), turnOff("關（釋放）", &dialog);
    QHBoxLayout coilRow;
    coilRow.addWidget(&turnOn);
    coilRow.addWidget(&turnOff);
    layout->addRow("選定通道開／關", &coilRow);
    QLabel manualNote("手動測試立即送出，使用上方通道與維持時間，不等待斷電前秒數。\n會切換實體接點：NC 吸合時斷電，NO 釋放時斷電。串口斷線不取消模組計時。", &dialog);
    manualNote.setWordWrap(true); layout->addRow(&manualNote);
    QPushButton clearLog("清除紀錄", &dialog); layout->addRow(&clearLog);
    QObject::connect(&clearLog, &QPushButton::clicked, &communicationLog, &QPlainTextEdit::clear);
    bool busy = false;
    auto connectionUi = [&] {
        const bool connected = connection.handle != INVALID_HANDLE_VALUE;
        connectPort.setEnabled(!busy && !connected && !port.currentData().toString().isEmpty());
        disconnectPort.setEnabled(!busy && connected); pulseNc.setEnabled(!busy && connected); pulseNo.setEnabled(!busy && connected);
        turnOn.setEnabled(!busy && connected);
        turnOff.setEnabled(!busy && connected);
        port.setEnabled(!busy && !connected); baud.setEnabled(!busy && !connected); refreshPorts.setEnabled(!busy && !connected);
        connectionLabel.setText(connected ? "已連線：" + current().port : "串口未連線");
    };
    QObject::connect(&connectPort, &QPushButton::clicked, &dialog, [&] {
        auto settings = current(); QString error;
        if (!validate(settings, error, true)) {testResult.setText(error); return;}
        const auto name = (QString("\\\\.\\") + settings.port).toStdWString();
        connection.handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (connection.handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            testResult.setText(QString("連線失敗：Windows error=%1").arg(code)
                + (code == ERROR_ACCESS_DENIED ? "。請先在原廠工具按「關閉串口」，再重新連線。" : ""));
        }
        else if (!configureSerial(connection.handle, settings, error)) {connection.close(); testResult.setText("連線失敗：" + error);}
        else {testResult.setText("串口已開啟；請讀取狀態確認模組回應。"); communicationLog.appendPlainText("CONNECTED " + settings.port);}
        connectionUi();
    });
    QObject::connect(&disconnectPort, &QPushButton::clicked, &dialog, [&] {connection.close(); communicationLog.appendPlainText("DISCONNECTED"); connectionUi();});
    QObject::connect(&port, &QComboBox::currentIndexChanged, &dialog, connectionUi);
    QObject::connect(&refreshPorts, &QPushButton::clicked, &dialog, connectionUi);
    connectionUi();
    std::unique_ptr<QThread> testWorker;
    auto invalidate = [&] {testResult.setText("設定已變更，請重新測試通訊。"); testResult.setStyleSheet({}); communicationLog.clear();};
    QObject::connect(&port, &QComboBox::currentIndexChanged, &dialog, [&] {
        invalidate(); testButton.setEnabled(!port.currentData().toString().isEmpty());
    });
    QObject::connect(&refreshPorts, &QPushButton::clicked, &dialog, [&] {
        populatePorts(); invalidate(); testButton.setEnabled(!port.currentData().toString().isEmpty());
    });
    testButton.setEnabled(!port.currentData().toString().isEmpty());
    QObject::connect(&baud, &QComboBox::currentIndexChanged, &dialog, invalidate);
    QObject::connect(&address, &QSpinBox::valueChanged, &dialog, invalidate);
    QDialogButtonBox buttons(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); layout->addRow(&buttons);
    auto setTesting = [&](bool testing) {
        testButton.setEnabled(!testing && !port.currentData().toString().isEmpty()); refreshPorts.setEnabled(!testing); port.setEnabled(!testing); baud.setEnabled(!testing); address.setEnabled(!testing);
        buttons.button(QDialogButtonBox::Save)->setEnabled(!testing);
        channel.setEnabled(!testing); duration.setEnabled(!testing); contact.setEnabled(!testing);
        busy = testing; connectionUi();
    };
    QObject::connect(&testButton, &QPushButton::clicked, &dialog, [&] {
        const auto settings = current(); QString error;
        if (!validate(settings, error, true)) {testResult.setText("設定無效：" + error); return;}
        auto result = std::make_shared<StatusResult>();
        testWorker.reset(QThread::create([settings, result, &connection] {*result = readStatus(settings, connection.handle);}));
        QObject::connect(testWorker.get(), &QThread::finished, &dialog, [&, result] {
            if (!result->success) connection.close();
            setTesting(false); communicationLog.appendPlainText(result->details);
            if (!result->success) {
                testResult.setText("通訊失敗：" + result->error);
                testResult.setStyleSheet("color: #b52323;"); return;
            }
            QStringList channels;
            for (int i = 0; i < 4; ++i)
                channels << QString("CH%1：%2").arg(i + 1).arg((result->states & (1 << i)) ? "吸合（NC 斷開）" : "釋放（NC 導通）");
            testResult.setText("通訊成功：已收到有效 Modbus 回覆。\n模組回報：" + channels.join("；"));
            testResult.setStyleSheet("color: #167744;");
        });
        setTesting(true); testResult.setText("測試中：讀取繼電器狀態，不切換接點…");
        testResult.setStyleSheet({}); testWorker->start();
    });
    auto manualCommand = [&](bool timed, bool on) {
        auto settings = current();
        settings.normallyClosed = on;
        const auto command = timed ? request(settings) : coilRequest(settings, on);
        auto result = std::make_shared<StatusResult>();
        testWorker.reset(QThread::create([settings, result, &connection, timed, command] {
            QByteArray reply;
            if (!exchange(settings, command, 8,
                [&](const QString &line) {result->details += line + '\n';}, reply, result->error, connection.handle)
                ) return;
            result->success = timed ? acceptReply(settings, reply, result->error) : reply == command;
            if (!result->success && result->error.isEmpty()) result->error = "回覆與開／關命令不符";
        }));
        QObject::connect(testWorker.get(), &QThread::finished, &dialog, [&, result] {
            if (!result->success) connection.close();
            setTesting(false); communicationLog.appendPlainText(result->details);
            testResult.setText(result->success ? "模組已確認命令。可讀取狀態確認接點。" : "命令未確認：" + result->error + "；模組可能已動作，不自動重送。");
        });
        setTesting(true); testResult.setText("送出手動命令…"); testWorker->start();
    };
    QObject::connect(&pulseNc, &QPushButton::clicked, &dialog, [&] {manualCommand(true, true);});
    QObject::connect(&pulseNo, &QPushButton::clicked, &dialog, [&] {manualCommand(true, false);});
    QObject::connect(&turnOn, &QPushButton::clicked, &dialog, [&] {manualCommand(false, true);});
    QObject::connect(&turnOff, &QPushButton::clicked, &dialog, [&] {manualCommand(false, false);});
    QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(&buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto s = current(); QString error;
        if (!validate(s, error, true)) {QMessageBox::warning(&dialog, "設定無效", error); return;}
        QSettings ini(directory + "/test_settings.ini", QSettings::IniFormat); ini.beginGroup("Relay");
        ini.setValue("Port", s.port); ini.setValue("Baud", s.baud);
        const auto adapter = usbSerialDevices().value(s.port);
        ini.setValue("UsbId", adapter.usbId);
        ini.setValue("DeviceInstance", adapter.instance);
        ini.setValue("Address", s.address); ini.setValue("Channel", s.channel); ini.remove("Mode");
        ini.setValue("NormallyClosed", s.normallyClosed);
        ini.setValue("ManualDurationTicks", s.ticks);
        ini.sync();
        if (ini.status() != QSettings::NoError) {QMessageBox::warning(&dialog, "保存失敗", "無法寫入 test_settings.ini"); return;}
        dialog.accept();
    });
    dialog.exec();
    if (testWorker) testWorker->wait(); // Keep captured result/UI state alive until the read completes.
}


bool relayDialogSelfTest()
{
    QTemporaryDir directory(QCoreApplication::applicationDirPath() + "/relay_ui_check_XXXXXX");
    if (!directory.isValid()) return false;
    QTimer poll; poll.setInterval(50); QElapsedTimer elapsed; elapsed.start();
    bool started = false, success = false;
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *button = dialog->findChild<QPushButton *>("relayCommunicationTest");
        auto *result = dialog->findChild<QLabel *>("relayCommunicationResult");
        auto *port = dialog->findChild<QComboBox *>("relayPort");
        if (!button || !result || !port) {dialog->reject(); return;}
        if (!started) {
            // Nonexistent port: exercise the UI error path without accessing physical hardware.
            if (port->isEditable()) {dialog->reject(); return;}
            // Inject a nonexistent test-only choice; never select a physical serial port.
            port->addItem("COM999999", "COM999999"); port->setCurrentIndex(port->count() - 1);
            auto *connect = dialog->findChild<QPushButton *>("relayConnect");
            auto *disconnect = dialog->findChild<QPushButton *>("relayDisconnect");
            auto *nc = dialog->findChild<QPushButton *>("relayPulseNc");
            auto *no = dialog->findChild<QPushButton *>("relayPulseNo");
            if (!connect || !disconnect || !nc || !no || !connect->isEnabled()
                || disconnect->isEnabled() || nc->isEnabled() || no->isEnabled()) {dialog->reject(); return;}
            connect->click();
            if (!result->text().startsWith("連線失敗：") || disconnect->isEnabled()
                || nc->isEnabled() || no->isEnabled()) {dialog->reject(); return;}
            button->click(); started = true;
        } else if (button->isEnabled() && result->text().startsWith("通訊失敗：")) {
            success = result->text().contains("無法開啟") && dialog->findChildren<QComboBox *>().size() == 3;
            success = dialog->grab().save(QCoreApplication::applicationDirPath() + "/relay_settings_preview.png") && success;
            auto *refresh = dialog->findChild<QPushButton *>("relayRefreshPorts");
            if (!refresh) {dialog->reject(); return;}
            refresh->click();
            success = success && result->text().contains("請重新測試") && port->findData("COM999999") < 0
                && !button->isEnabled();
            dialog->reject();
        } else if (elapsed.elapsed() > 5000) dialog->reject();
    });
    poll.start(); showRelaySettings(directory.path(), nullptr); poll.stop();
    return started && success;
}
