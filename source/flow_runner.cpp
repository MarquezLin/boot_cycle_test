#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <dwmapi.h>
#include "flow_runner.h"
#include "image_checks.h"
#include "ocr_reader.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QSet>
#include <memory>

namespace {
struct Release { template<class T> void operator()(T *value) const {if (value) value->Release();} };
template<class T> using Com = std::unique_ptr<T, Release>;
QString bstrText(BSTR text) {const auto value = QString::fromWCharArray(text ? text : L""); SysFreeString(text); return value;}
QString processPath(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{}; DWORD count = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &count);
    CloseHandle(process); return ok ? QString::fromWCharArray(path) : QString();
}
struct WindowSearch {QString path; DWORD pid = 0; QVector<FlowWindowInfo> windows;};
BOOL CALLBACK searchWindow(HWND window, LPARAM parameter)
{
    auto &search = *reinterpret_cast<WindowSearch *>(parameter);
    if (!IsWindowVisible(window) || GetWindowTextLengthW(window) == 0
        || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_NOACTIVATE)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if ((search.pid && pid == search.pid) || (!search.pid && QDir::fromNativeSeparators(processPath(pid)).compare(search.path, Qt::CaseInsensitive) == 0)) {
        wchar_t title[2048]{}; GetWindowTextW(window, title, 2048);
        search.windows.append({reinterpret_cast<quintptr>(window), QString::fromWCharArray(title), GetWindow(window, GW_OWNER) != nullptr});
    }
    return TRUE;
}
bool foreground(HWND window, QString &error)
{
    if (!window || !IsWindow(window)) {error = "找不到待測視窗，請先啟動 APP 並等待視窗"; return false;}
    requestFlowForeground(reinterpret_cast<quintptr>(window));
    // Activation crosses input queues and may finish after SetForegroundWindow returns.
    for (int attempt = 0; attempt < 20 && GetForegroundWindow() != window; ++attempt)
        QThread::msleep(25);
    if (GetForegroundWindow() != window) {error = "無法將待測視窗帶到前景，請先點選待測 APP 後重試"; return false;}
    return true;
}
Com<IUIAutomationElement> locate(IUIAutomation *automation, HWND window, const FlowStep &step, QString &error, int *matchingCount = nullptr)
{
    if (matchingCount) *matchingCount = 0;
    IUIAutomationElement *rawRoot = nullptr;
    if (!window || FAILED(automation->ElementFromHandle(window, &rawRoot))) {error = "找不到待測視窗的 UIA 根節點"; return {};}
    Com<IUIAutomationElement> root(rawRoot);
    if (step.selector == "@point") {
        if (!foreground(window, error)) return {};
        RECT bounds{}; GetClientRect(window, &bounds);
        POINT point{step.region.x(), step.region.y()};
        if (point.x < 0 || point.y < 0 || point.x >= bounds.right || point.y >= bounds.bottom) {error = "設定的位置超出待測視窗，請重新指一下"; return {};}
        ClientToScreen(window, &point); IUIAutomationElement *element = nullptr;
        if (FAILED(automation->ElementFromPoint(point, &element)) || !element) {error = "讀取所指位置失敗"; return {};}
        Com<IUIAutomationElement> picked(element); int pid = 0; picked->get_CurrentProcessId(&pid);
        DWORD expected = 0; GetWindowThreadProcessId(window, &expected);
        if (static_cast<DWORD>(pid) != expected) {error = "所指位置被其他程式遮住，請重新確認"; return {};}
        return picked;
    }
    IUIAutomationCondition *rawCondition = nullptr;
    auto conditionFor = [&](PROPERTYID property, const QString &text) {
        VARIANT value; VariantInit(&value); value.vt = VT_BSTR; value.bstrVal = SysAllocString(reinterpret_cast<const wchar_t *>(text.utf16()));
        IUIAutomationCondition *condition = nullptr;
        automation->CreatePropertyCondition(property, value, &condition); VariantClear(&value);
        return Com<IUIAutomationCondition>(condition);
    };
    const bool regexName = step.selector.startsWith("regex:");
    const QRegularExpression namePattern(regexName ? step.selector.mid(6) : QString());
    if (regexName && !namePattern.isValid()) {error = "UIA 名稱正規式無效"; return {};}
    auto name = step.selector.isEmpty() || regexName ? Com<IUIAutomationCondition>() : conditionFor(UIA_NamePropertyId, step.selector);
    auto id = step.automationId.isEmpty() ? Com<IUIAutomationCondition>() : conditionFor(UIA_AutomationIdPropertyId, step.automationId);
    Com<IUIAutomationCondition> combined;
    if (name && id && SUCCEEDED(automation->CreateAndCondition(name.get(), id.get(), &rawCondition))) combined.reset(rawCondition);
    auto *condition = combined ? combined.get() : id ? id.get() : name.get();
    if (!condition && regexName) {automation->CreateTrueCondition(&rawCondition); combined.reset(rawCondition); condition = combined.get();}
    if (!condition) {error = "控制項條件無效"; return {};}
    IUIAutomationElementArray *rawElements = nullptr;
    if (FAILED(root->FindAll(TreeScope_Subtree, condition, &rawElements))) {error = "UIA 查詢失敗"; return {};}
    Com<IUIAutomationElementArray> elements(rawElements); int count = 0;
    if (elements) elements->get_Length(&count);
    Com<IUIAutomationElement> found; int matches = 0;
    for (int i = 0; i < count; ++i) {
        IUIAutomationElement *rawElement = nullptr; elements->GetElement(i, &rawElement); Com<IUIAutomationElement> element(rawElement);
        if (!element) continue;
        if (regexName) {BSTR text = nullptr; element->get_CurrentName(&text); if (!namePattern.match(bstrText(text)).hasMatch()) continue;}
        ++matches; found = std::move(element);
    }
    if (matchingCount) *matchingCount = matches;
    if (matches != 1) {error = matches == 0 ? "找不到符合的 UIA 控制項" : "符合多個控制項，請加入唯一 AutomationId 或縮小名稱條件"; return {};}
    return found;
}
bool keyboard(HWND window, const QString &text, bool shortcut, QString &error)
{
    if (!foreground(window, error)) return false;
    QVector<INPUT> inputs;
    auto key = [&](WORD code, DWORD flags = 0) { INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wVk = code; input.ki.dwFlags = flags; inputs.append(input); };
    if (shortcut) {
        const auto value = text.toUpper(); WORD modifier = 0, code = 0;
        if (value == "ENTER") code = VK_RETURN; else if (value == "TAB") code = VK_TAB; else if (value == "ESC") code = VK_ESCAPE;
        else if (value.startsWith("CTRL+") && QStringList{"CTRL+A", "CTRL+C", "CTRL+V"}.contains(value)) {modifier = VK_CONTROL; code = value.back().unicode();}
        else if (value == "ALT+F4") {modifier = VK_MENU; code = VK_F4;}
        else {error = "不支援的快捷鍵"; return false;}
        if (modifier) key(modifier); key(code); key(code, KEYEVENTF_KEYUP); if (modifier) key(modifier, KEYEVENTF_KEYUP);
    } else {
        for (const auto character : text) {
            INPUT input{}; input.type = INPUT_KEYBOARD; input.ki.wScan = character.unicode(); input.ki.dwFlags = KEYEVENTF_UNICODE;
            inputs.append(input); input.ki.dwFlags |= KEYEVENTF_KEYUP; inputs.append(input);
        }
    }
    if (inputs.isEmpty()) {error = "輸入文字不可留空"; return false;}
    if (SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT)) != static_cast<UINT>(inputs.size())) {error = "鍵盤輸入失敗，請檢查 APP 與工具的權限是否一致"; return false;}
    return true;
}
}
QVector<FlowWindowInfo> flowWindows(const QString &executable, quint32 pid)
{
    WindowSearch search{QDir::fromNativeSeparators(QFileInfo(executable).absoluteFilePath()), pid};
    EnumWindows(searchWindow, reinterpret_cast<LPARAM>(&search)); return search.windows;
}
quintptr findFlowWindow(const QString &executable, quint32 pid, const QString &title)
{
    const auto windows = flowWindows(executable, pid);
    QVector<FlowWindowInfo> matches;
    for (const auto &window : windows)
        if (title.isEmpty() ? !window.owned : window.title == title) matches.append(window);
    return matches.size() == 1 ? matches.first().handle : 0;
}
QString flowWindowError(const QString &executable, quint32 pid, const QString &title)
{
    const auto windows = flowWindows(executable, pid);
    int count = 0;
    for (const auto &window : windows) if (title.isEmpty() ? !window.owned : window.title == title) ++count;
    if (count > 1) return title.isEmpty()
        ? "待測 APP 有多個視窗，請在所選步驟的「待測視窗」選單指定主視窗或預覽視窗。"
        : "相同標題符合多個視窗，請關閉同名視窗或額外 APP 實例後重試。";
    return title.isEmpty() ? "找不到待測 APP 視窗，請先開啟 APP 並重新整理視窗清單。"
        : "找不到指定視窗「" + title + "」，請確認視窗已開啟，或重新選擇待測視窗。";
}
void requestFlowForeground(quintptr handle)
{
    const HWND window = reinterpret_cast<HWND>(handle);
    if (!window || !IsWindow(window)) return;
    if (IsIconic(window)) ShowWindowAsync(window, SW_RESTORE);
    if (GetForegroundWindow() != window) SetForegroundWindow(window);
}
quintptr flowWindowAtPoint(const QString &executable, const QPoint &point)
{
    const HWND underPoint = WindowFromPoint(POINT{point.x(), point.y()});
    const HWND root = underPoint ? GetAncestor(underPoint, GA_ROOT) : nullptr;
    for (const auto &window : flowWindows(executable))
        if (window.handle == reinterpret_cast<quintptr>(root)) return window.handle;
    return 0;
}
QPoint flowClientPoint(quintptr handle, const QPoint &screenPoint)
{
    POINT point{screenPoint.x(), screenPoint.y()};
    if (!ScreenToClient(reinterpret_cast<HWND>(handle), &point)) return QPoint(-1, -1);
    return QPoint(point.x, point.y);
}
QImage captureFlowWindow(quintptr handle, QString &error)
{
    const HWND window = reinterpret_cast<HWND>(handle);
    if (!foreground(window, error)) return {};
    RECT client{}; GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {error = "視窗尺寸無效"; return {};}
    POINT origin{};
    if (!ClientToScreen(window, &origin)) {error = "無法取得待測視窗位置"; return {};}
    MONITORINFO monitor{sizeof(MONITORINFO)}; RECT frame{};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) || !GetWindowRect(window, &frame)) {
        error = "無法取得待測視窗的螢幕範圍"; return {};
    }
    const QRect work(monitor.rcWork.left, monitor.rcWork.top,
        monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top);
    if (!work.contains(QRect(origin.x, origin.y, width, height)) && !IsZoomed(window)) {
        const int frameWidth = frame.right - frame.left, frameHeight = frame.bottom - frame.top;
        if (frameWidth > work.width() || frameHeight > work.height()) {
            error = "待測視窗大於螢幕可用範圍，無法只移動位置完整顯示；請縮小視窗後重新框選。"; return {};
        }
        const int x = qBound(work.x(), int(frame.left), work.x() + work.width() - frameWidth);
        const int y = qBound(work.y(), int(frame.top), work.y() + work.height() - frameHeight);
        // Keep client dimensions unchanged so saved OCR/image regions remain valid.
        origin = POINT{};
        if (!SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)
            || !ClientToScreen(window, &origin)) {
            error = "無法將待測視窗移回螢幕內，請確認視窗權限或手動移動後重試。"; return {};
        }
    }
    const QRect screenBounds(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (!screenBounds.contains(QRect(origin.x, origin.y, width, height))) {
        error = "待測視窗移動後仍超出桌面，請確認視窗位置後重試。"; return {};
    }
    DwmFlush();
    for (const int y : {0, height / 2, height - 1}) for (const int x : {0, width / 2, width - 1}) {
        const HWND visible = WindowFromPoint(POINT{origin.x + x, origin.y + y});
        if (visible && GetAncestor(visible, GA_ROOT) != window) {
            error = "待測視窗被其他視窗遮住，請移開遮擋視窗後重試。"; return {};
        }
    }
    // Read the composed desktop, not the window's GDI backing store (GPU surfaces may be black there).
    HDC source = GetDC(nullptr);
    if (!source) {error = "無法取得桌面截圖來源"; return {};}
    HDC memory = CreateCompatibleDC(source);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr; HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    QImage image;
    if (bitmap && memory && bits) {
        const auto old = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, source, origin.x, origin.y, SRCCOPY | CAPTUREBLT)) image = QImage(static_cast<uchar *>(bits), width, height, QImage::Format_RGB32).copy();
        SelectObject(memory, old);
    }
    if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); ReleaseDC(nullptr, source);
    if (image.isNull()) error = "擷取視窗失敗";
    return image;
}
bool pickFlowControl(quintptr handle, int x, int y, QString &name, QString &id, QString &error)
{
    const auto point = flowClientPoint(handle, QPoint(x, y)); RECT bounds{};
    if (!handle || !GetClientRect(reinterpret_cast<HWND>(handle), &bounds)
        || point.x() < 0 || point.y() < 0 || point.x() >= bounds.right || point.y() >= bounds.bottom) {
        error = "請把滑鼠放在待測視窗內容的文字或按鈕上"; return false;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *raw = nullptr; bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw)))) {
        Com<IUIAutomation> automation(raw); IUIAutomationElement *elementRaw = nullptr;
        if (SUCCEEDED(automation->ElementFromPoint(POINT{x, y}, &elementRaw))) {
            Com<IUIAutomationElement> element(elementRaw); int pid = 0; element->get_CurrentProcessId(&pid);
            DWORD expected = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(handle), &expected);
            if (static_cast<DWORD>(pid) == expected) {
                BSTR value = nullptr; element->get_CurrentName(&value); name = bstrText(value);
                value = nullptr; element->get_CurrentAutomationId(&value); id = bstrText(value); ok = !name.isEmpty() || !id.isEmpty();
            }
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!ok) error = "該位置沒有可使用的待測 APP UIA 名稱或 AutomationId";
    return ok;
}
void FlowRunner::run()
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation *raw = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&raw));
    {
        Com<IUIAutomation> automation(raw);
        HWND window = reinterpret_cast<HWND>(findFlowWindow(flow.executable));
        quint32 pid = 0; QString value = initialValue; bool hasValue = !initialValue.isNull(), success = true;
        QDir().mkpath(evidenceDirectory);
        const int first = singleStep < 0 ? 0 : singleStep, last = singleStep < 0 ? static_cast<int>(flow.steps.size()) : singleStep + 1;
        for (int index = first; index < last && !m_stop; ++index) {
            const auto &s = flow.steps[index]; QString error, detail;
            emit stepStarted(index);
            emit progress(QString("步驟 %1：%2 — %3").arg(index + 1).arg(flowTypeName(s.type), s.name));
            QElapsedTimer timer; timer.start(); bool ok = false;
            QImage baseline; qint64 baselineTime = 0; int imageSamples = 0;
            do {
                error.clear();
                if (s.type == "launch") {
                    QSet<DWORD> existing;
                    if (flow.reuseExisting) for (const auto &candidate : flowWindows(flow.executable)) {
                        DWORD candidatePid = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(candidate.handle), &candidatePid);
                        if (candidatePid) existing.insert(candidatePid);
                    }
                    if (existing.size() > 1) {error = "已開啟多份待測 APP，請先關閉多餘實例後重試；工具不會自動關閉 APP。"; break;}
                    if (existing.size() == 1) {
                        pid = *existing.begin(); ok = true; detail = QString("重用已開啟的待測 APP（PID %1）").arg(pid); window = nullptr;
                    } else {
                    qint64 launchedPid = 0;
                    ok = QProcess::startDetached(flow.executable, QProcess::splitCommand(flow.arguments),
                        flow.workingDirectory.isEmpty() ? QFileInfo(flow.executable).absolutePath() : flow.workingDirectory, &launchedPid);
                    pid = static_cast<quint32>(launchedPid); window = nullptr;
                    if (!ok) error = "無法啟動待測 APP";
                    else detail = QString("已啟動待測 APP（PID %1）").arg(pid);
                    }
                } else if (s.type == "waitWindow") {
                    window = reinterpret_cast<HWND>(findFlowWindow(flow.executable, pid, s.windowTitle));
                    ok = window != nullptr; if (!ok) error = flowWindowError(flow.executable, pid, s.windowTitle);
                } else if (s.type == "delay") {
                    while (!m_stop && timer.elapsed() < s.timeoutMs) msleep(50);
                    ok = !m_stop;
                } else {
                    window = reinterpret_cast<HWND>(findFlowWindow(flow.executable, pid, s.windowTitle));
                    if (!window && s.type != "clickUIA" && s.type != "readUIA") {
                        error = flowWindowError(flow.executable, pid, s.windowTitle);
                        if ((s.type == "readOCR" || s.type == "checkImage") && timer.elapsed() < s.timeoutMs) {
                            emit stepObserved(index, "等待待測視窗出現：" + s.windowTitle);
                            msleep(100); continue;
                        }
                        break;
                    }
                    if (s.type == "clickUIA" || s.type == "readUIA") {
                        if (FAILED(created) || !automation) {error = "無法初始化 UI Automation"; break;}
                        auto element = window ? locate(automation.get(), window, s, error) : Com<IUIAutomationElement>();
                        if (!window) error = flowWindowError(flow.executable, pid, s.windowTitle);
                        if (!element && !s.automationId.isEmpty() && s.selector != "@point") {
                            int total = 0; HWND resolved = nullptr; QString resolvedTitle;
                            Com<IUIAutomationElement> resolvedElement;
                            for (const auto &candidate : flowWindows(flow.executable, pid)) {
                                if (m_stop) break;
                                int count = 0; QString candidateError;
                                auto match = locate(automation.get(), reinterpret_cast<HWND>(candidate.handle), s, candidateError, &count);
                                total += count;
                                if (match) {resolvedElement = std::move(match); resolved = reinterpret_cast<HWND>(candidate.handle); resolvedTitle = candidate.title;}
                            }
                            if (total == 1 && resolvedElement) {
                                window = resolved; element = std::move(resolvedElement); error.clear();
                                flow.steps[index].windowTitle = resolvedTitle;
                                emit windowResolved(index, resolvedTitle);
                                emit progress("依 AutomationId 重新定位控制項；實際視窗：" + resolvedTitle);
                            } else error = total > 1 ? "AutomationId 符合多個控制項，無法唯一重新定位；請關閉多餘視窗或重新指定。"
                                : "在待測 APP 的已開啟視窗中找不到 AutomationId：" + s.automationId + "；請重新指一下控制項。";
                        }
                        if (element) {
                            if (s.type == "clickUIA") {
                                IUIAutomationInvokePattern *invokeRaw = nullptr;
                                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invokeRaw)))) {
                                    Com<IUIAutomationInvokePattern> invoke(invokeRaw); ok = SUCCEEDED(invoke->Invoke());
                                }
                                if (!ok) error = "控制項不支援 Invoke 或點擊失敗，請改用位置點擊";
                            } else {
                                IUIAutomationValuePattern *valueRaw = nullptr; BSTR text = nullptr;
                                if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&valueRaw)))) {
                                    Com<IUIAutomationValuePattern> pattern(valueRaw); ok = SUCCEEDED(pattern->get_CurrentValue(&text));
                                } else ok = SUCCEEDED(element->get_CurrentName(&text));
                                value = bstrText(text); hasValue = ok; detail = value;
                                if (!ok) error = "無法讀取控制項文字";
                            }
                        }
                    } else if (s.type == "clickPoint") {
                        RECT client{}; GetClientRect(window, &client);
                        if (s.region.x() < 0 || s.region.y() < 0 || s.region.x() >= client.right || s.region.y() >= client.bottom) error = "點擊位置超出視窗";
                        else if (foreground(window, error)) {
                            POINT point{s.region.x(), s.region.y()}; ClientToScreen(window, &point);
                            SetCursorPos(point.x, point.y);
                            INPUT inputs[2]{}; inputs[0].type = inputs[1].type = INPUT_MOUSE;
                            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                            ok = SendInput(2, inputs, sizeof(INPUT)) == 2;
                            if (!ok) error = "滑鼠輸入失敗，請檢查權限";
                        }
                    } else if (s.type == "typeText" || s.type == "hotkey") ok = keyboard(window, s.text, s.type == "hotkey", error);
                    else if (s.type == "readOCR") {
                        const auto image = captureFlowWindow(reinterpret_cast<quintptr>(window), error);
                        if (image.isNull()) {
                            emit stepObserved(index, error);
                            if (timer.elapsed() >= s.timeoutMs) break;
                            msleep(100); continue;
                        }
                        if (!image.rect().contains(s.region)) {error = "OCR 區域超出目前視窗，請重新框選"; break;}
                        const auto path = evidenceDirectory + QString("/step_%1.png").arg(index + 1);
                        if (!image.copy(s.region).save(path)) {error = "無法保存 OCR 截圖"; break;}
                        const auto prepared = evidenceDirectory + QString("/step_%1_ocr.png").arg(index + 1);
                        if (!prepareOcrImage(image.copy(s.region)).save(prepared)) {error = "無法準備文字辨識圖片"; break;}
                        QProcess ocr;
                        const auto engine = detectOcrExecutable(flow.ocrExecutable);
                        ocr.start(engine, ocrArguments(engine, prepared, flow.ocrLanguage));
                        while (!m_stop && timer.elapsed() < s.timeoutMs && !ocr.waitForFinished(50)) {
                            if (ocr.state() == QProcess::NotRunning) break;
                        }
                        if (ocr.state() != QProcess::NotRunning) {ocr.kill(); ocr.waitForFinished(1000); error = "OCR 逾時或已停止";}
                        else if (ocr.exitStatus() != QProcess::NormalExit || ocr.exitCode() != 0 || ocr.error() == QProcess::FailedToStart)
                            error = "OCR 引擎失敗：" + QString::fromUtf8(ocr.readAllStandardError());
                        else {
                            value = QString::fromUtf8(ocr.readAllStandardOutput()).trimmed(); hasValue = !value.isEmpty(); ok = hasValue;
                            detail = value; if (!ok) error = "OCR 未辨識出文字";
                        }
                    } else if (s.type == "checkImage") {
                        const int sampleInterval = 500 + 137 * (qMax(0, imageSamples - 1) % 4);
                        if (!baseline.isNull() && timer.elapsed() - baselineTime < sampleInterval) {
                            error = "等待下一張預覽畫面；" + detail;
                        } else {
                            const auto full = captureFlowWindow(reinterpret_cast<quintptr>(window), error);
                            if (full.isNull()) {
                                emit stepObserved(index, error);
                                if (timer.elapsed() >= s.timeoutMs) break;
                                msleep(100); continue;
                            }
                            if (!full.rect().contains(s.region)) {error = "影像區域超出目前視窗，請重新框選"; break;}
                            const auto image = full.copy(s.region);
                            if (!image.save(evidenceDirectory + QString("/step_%1.png").arg(index + 1))) {error = "無法保存影像檢查截圖"; break;}
                            const double contrast = imageContrast(image), threshold = s.expected.toDouble();
                            const double difference = baseline.isNull() ? -1 : imageDifference(baseline, image);
                            if (!baseline.isNull() && !baseline.save(evidenceDirectory + QString("/step_%1_before.png").arg(index + 1))) {
                                error = "無法保存比較前的截圖"; break;
                            }
                            if (s.condition == "contrast") {
                                // Uniform test patterns can have valid content across time.
                                ok = contrast >= threshold || difference >= threshold;
                                detail = QString("亮度標準差=%1；前後平均 RGB 差異=%2；任一 ≥ %3 算有畫面證據")
                                    .arg(contrast, 0, 'f', 2).arg(difference < 0 ? "尚待第二張" : QString::number(difference, 'f', 2)).arg(threshold);
                            } else if (s.condition == "change") {
                                ok = difference >= threshold;
                                detail = difference < 0 ? "已取得第一張畫面，等待至少 500 ms"
                                    : QString("畫面平均 RGB 差異=%1；門檻=%2（允許純黑／純白互換）").arg(difference, 0, 'f', 2).arg(threshold);
                            } else {error = "不支援的影像檢查條件"; break;}
                            baseline = image; baselineTime = timer.elapsed(); ++imageSamples;
                            if (!ok) error = "影像條件未符合：" + detail;
                        }
                    } else if (s.type == "assert") {
                        if (!hasValue) error = "尚未取得讀取值，請先執行讀取步驟";
                        else {
                            QString parsed; ok = evaluateFlowValue(value, s.condition, s.expected, parsed, error);
                            detail = "原始值：" + value + "；解析值：" + parsed + "；預期：" + s.expected;
                            if (!ok && error.isEmpty()) error = "條件未符合；" + detail;
                        }
                    } else error = "未知的步驟類型";
                }
                if (ok && (s.type == "readUIA" || s.type == "readOCR")) {
                    const QString rawValue = value;
                    emit stepObserved(index, "目前讀到：" + (rawValue.isEmpty() ? "（空字串）" : rawValue) + "；預期：" + s.expected);
                    if (!s.text.isEmpty()) {
                        const auto match = QRegularExpression(s.text).match(value);
                        if (!match.hasMatch() || match.lastCapturedIndex() < 1) {error = "擷取正規式未取得第一個括號群組"; ok = false;}
                        else value = match.captured(1);
                    }
                    if (ok && !s.expected.isEmpty()) {
                        QString parsed; ok = evaluateFlowValue(value, s.condition, s.expected, parsed, error);
                        if (!ok && error.isEmpty()) error = "讀取值尚未符合條件：" + (value.isEmpty() ? "（空字串）" : value) + "；預期：" + s.expected;
                    }
                    hasValue = ok;
                    detail = "原始值：" + rawValue + "；讀取值：" + value;
                }
                // Only discovery/read steps are retried. Actions execute once.
                if (ok || (s.type != "waitWindow" && s.type != "readUIA" && s.type != "readOCR" && s.type != "checkImage") || timer.elapsed() >= s.timeoutMs) break;
                msleep(100);
            } while (!m_stop);
            if (m_stop) {emit stepFinished(index, "STOP", detail); break;}
            if (ok && (s.type == "readUIA" || s.type == "readOCR")) emit valueRead(value);
            emit stepFinished(index, ok ? "PASS" : "FAIL", ok ? detail : error + (detail.isEmpty() ? QString() : "；" + detail));
            emit progress((ok ? "PASS: " : "FAIL: ") + (ok ? detail : error));
            if (!ok) {
                QString screenshotError;
                const auto image = captureFlowWindow(reinterpret_cast<quintptr>(window), screenshotError);
                if (!image.isNull()) image.save(evidenceDirectory + "/failure.png");
                success = false; break;
            }
        }
        emit completed(success && !m_stop, m_stop);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}
