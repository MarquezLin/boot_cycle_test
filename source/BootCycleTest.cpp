#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace fs = std::filesystem;

namespace {

constexpr DWORD kStartupDelaySeconds = 20;
constexpr DWORD kWindowTimeoutSeconds = 60;
constexpr DWORD kAudioTimeoutSeconds = 30;

fs::path executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

std::wstring nowText()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::string narrow(const std::wstring &value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

void appendDetail(const fs::path &dir, const std::wstring &message)
{
    std::ofstream out(dir / "boot_cycle_detail.log", std::ios::app | std::ios::binary);
    out << narrow(nowText()) << " | " << narrow(message) << "\r\n";
}

void appendResult(const fs::path &dir, const std::wstring &result,
                  unsigned long long received, const std::wstring &detail)
{
    const fs::path csv = dir / "boot_cycle_results.csv";
    const bool newFile = !fs::exists(csv);
    std::ofstream out(csv, std::ios::app | std::ios::binary);
    if (newFile) out << "time,result,received_frames,detail\r\n";
    std::string clean = narrow(detail);
    for (char &ch : clean) {
        if (ch == '\r' || ch == '\n' || ch == ',') ch = ' ';
    }
    out << narrow(nowText()) << ',' << narrow(result) << ',' << received << ',' << clean << "\r\n";
}

bool stopRequested(const fs::path &dir)
{
    return fs::exists(dir / "STOP") || fs::exists(dir / "FAILED");
}

bool waitInterruptible(const fs::path &dir, DWORD seconds)
{
    for (DWORD i = 0; i < seconds; ++i) {
        if (stopRequested(dir)) return false;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return !stopRequested(dir);
}

bool launchTarget(const fs::path &targetExe, PROCESS_INFORMATION &pi, std::wstring &error)
{
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    std::wstring command = L"\"" + targetExe.wstring() + L"\"";
    const auto workingDirectory = targetExe.parent_path().wstring();
    if (!CreateProcessW(targetExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, workingDirectory.c_str(), &si, &pi)) {
        error = L"CreateProcess failed: " + std::to_wstring(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    return true;
}

IUIAutomationElement *findProcessWindow(IUIAutomation *automation, DWORD pid)
{
    IUIAutomationElement *root = nullptr;
    if (FAILED(automation->GetRootElement(&root))) return nullptr;

    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = static_cast<LONG>(pid);
    IUIAutomationCondition *condition = nullptr;
    automation->CreatePropertyCondition(UIA_ProcessIdPropertyId, value, &condition);

    IUIAutomationElement *window = nullptr;
    if (condition) {
        root->FindFirst(TreeScope_Children, condition, &window);
        condition->Release();
    }
    root->Release();
    return window;
}

bool invokeStart(IUIAutomation *automation, IUIAutomationElement *window, std::wstring &error)
{
    VARIANT value{};
    value.vt = VT_I4;
    value.lVal = UIA_ButtonControlTypeId;
    IUIAutomationCondition *condition = nullptr;
    automation->CreatePropertyCondition(UIA_ControlTypePropertyId, value, &condition);
    if (!condition) {
        error = L"Cannot create button condition";
        return false;
    }

    IUIAutomationElementArray *buttons = nullptr;
    window->FindAll(TreeScope_Subtree, condition, &buttons);
    condition->Release();
    if (!buttons) {
        error = L"No buttons exposed through UI Automation";
        return false;
    }

    int count = 0;
    buttons->get_Length(&count);
    bool invoked = false;
    for (int i = 0; i < count && !invoked; ++i) {
        IUIAutomationElement *button = nullptr;
        buttons->GetElement(i, &button);
        if (!button) continue;
        BSTR name = nullptr;
        BOOL enabled = FALSE;
        button->get_CurrentName(&name);
        button->get_CurrentIsEnabled(&enabled);
        if (name && enabled && wcscmp(name, L"Start") == 0) {
            IUIAutomationInvokePattern *invoke = nullptr;
            if (SUCCEEDED(button->GetCurrentPatternAs(UIA_InvokePatternId,
                                                       IID_PPV_ARGS(&invoke))) && invoke) {
                invoked = SUCCEEDED(invoke->Invoke());
                invoke->Release();
            }
        }
        if (name) SysFreeString(name);
        button->Release();
    }
    buttons->Release();
    if (!invoked) error = L"Enabled CH0 Start button was not found or could not be invoked";
    return invoked;
}

bool readAudioCount(IUIAutomation *automation, IUIAutomationElement *window,
                    unsigned long long &received, std::wstring &matchedText)
{
    IUIAutomationCondition *condition = nullptr;
    automation->CreateTrueCondition(&condition);
    if (!condition) return false;

    IUIAutomationElementArray *elements = nullptr;
    window->FindAll(TreeScope_Subtree, condition, &elements);
    condition->Release();
    if (!elements) return false;

    static const std::wregex pattern(LR"(CH0 Audio[\s\S]*received=([0-9]+) frames)");
    int count = 0;
    elements->get_Length(&count);
    bool found = false;
    for (int i = 0; i < count && !found; ++i) {
        IUIAutomationElement *element = nullptr;
        elements->GetElement(i, &element);
        if (!element) continue;
        BSTR name = nullptr;
        element->get_CurrentName(&name);
        if (name) {
            std::wstring text(name, SysStringLen(name));
            std::wsmatch match;
            if (std::regex_search(text, match, pattern)) {
                received = std::stoull(match[1].str());
                matchedText = text;
                found = true;
            }
            SysFreeString(name);
        }
        element->Release();
    }
    elements->Release();
    return found;
}

void markFailure(const fs::path &dir, const std::wstring &detail)
{
    appendResult(dir, L"FAIL", 0, detail);
    appendDetail(dir, L"FAIL: " + detail);
    std::ofstream(dir / "FAILED", std::ios::binary) << narrow(nowText()) << " | " << narrow(detail) << "\r\n";
}

enum class PowerCycleRequest { NotConfigured, Armed, Failed };

PowerCycleRequest requestExternalPowerCycle(const fs::path &dir, std::wstring &error)
{
    // TODO: Configure UART port, baud rate and the controller's protocol.
    // Request one autonomous sequence: delayed power OFF, OFF hold, power ON.
    // Return Armed only after the controller acknowledges the whole sequence.
    // Return Failed with error details on transmission/acknowledgment failure.
    // The controller must keep its own power while the tested PC is powered off.
    error.clear();
    appendDetail(dir, L"CONTROLLER_NOT_CONFIGURED: UART placeholder; no command sent. "
                      L"Shutdown only; external power restoration is required.");
    return PowerCycleRequest::NotConfigured;
}

} // namespace

int runBootTest(int argc, wchar_t **argv)
{
    const fs::path dir = executableDirectory();
    bool dryRun = false, warmReboot = false, describeAction = false;
    fs::path targetExe;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--dry-run") dryRun = true;
        else if (arg == L"--warm-reboot") warmReboot = true;
        else if (arg == L"--describe-action") describeAction = true;
        else if (arg == L"--target-exe" && i + 1 < argc) targetExe = argv[++i];
        else { std::wcerr << L"Unknown argument: " << arg << L'\n'; return 1; }
    }
    const wchar_t *powerCommand = warmReboot
        ? L"shutdown.exe /r /t 0 /d p:4:1 /c \"GVFG warm reboot audio-frame test\""
        : L"shutdown.exe /s /t 0 /d p:4:1 /c \"GVFG cold boot audio-frame test\"";
    if (describeAction) {
        std::wcout << L"Mode=" << (warmReboot ? L"warm" : L"cold") << L"\nAction="
                   << (dryRun ? L"none (dry-run)" : powerCommand)
                   << L"\nController=" << (!dryRun && !warmReboot ? L"UART placeholder" : L"not called") << L'\n';
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\GVFG_BootCycleTest_Runner");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    if (stopRequested(dir)) {
        appendDetail(dir, L"Runner skipped because STOP or FAILED exists");
        CloseHandle(mutex);
        return 0;
    }

    std::error_code targetError;
    if (targetExe.empty() || !targetExe.is_absolute() || !fs::is_regular_file(targetExe, targetError)) {
        markFailure(dir, L"Target EXE is missing or invalid: " + targetExe.wstring());
        CloseHandle(mutex);
        return 1;
    }
    appendDetail(dir, L"Target EXE: " + targetExe.wstring());

    appendDetail(dir, warmReboot ? L"Mode=warm (Windows restart)" : L"Mode=cold (external power cycle)");
    appendDetail(dir, dryRun ? L"Dry-run started" : L"Boot check started");
    if (!waitInterruptible(dir, dryRun ? 1 : kStartupDelaySeconds)) {
        appendDetail(dir, L"Stopped before application launch");
        CloseHandle(mutex);
        return 0;
    }

    PROCESS_INFORMATION pi{};
    std::wstring error;
    if (!launchTarget(targetExe, pi, error)) {
        markFailure(dir, error);
        CloseHandle(mutex);
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IUIAutomation *automation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&automation))) || !automation) {
        markFailure(dir, L"Cannot initialize Windows UI Automation");
        CloseHandle(pi.hProcess);
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    IUIAutomationElement *window = nullptr;
    appendDetail(dir, L"Waiting for application window");
    for (DWORD i = 0; i < kWindowTimeoutSeconds && !stopRequested(dir); ++i) {
        window = findProcessWindow(automation, pi.dwProcessId);
        if (window) break;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    if (stopRequested(dir)) {
        appendDetail(dir, L"STOP detected while waiting for application window");
        if (window) window->Release();
        automation->Release();
        CloseHandle(pi.hProcess);
        CoUninitialize();
        CloseHandle(mutex);
        return 0;
    }
    if (!window) {
        markFailure(dir, L"Application window did not appear within 60 seconds");
        automation->Release();
        CloseHandle(pi.hProcess);
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    appendDetail(dir, L"Invoking Start");
    if (!invokeStart(automation, window, error)) {
        markFailure(dir, error);
        window->Release();
        automation->Release();
        CloseHandle(pi.hProcess);
        CoUninitialize();
        CloseHandle(mutex);
        return 1;
    }

    unsigned long long received = 0;
    appendDetail(dir, L"Waiting for CH0 audio (30 second timeout)");
    std::wstring statusText;
    for (DWORD i = 0; i < kAudioTimeoutSeconds && !stopRequested(dir); ++i) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (readAudioCount(automation, window, received, statusText) && received > 0) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    window->Release();
    automation->Release();
    CloseHandle(pi.hProcess);
    CoUninitialize();

    if (stopRequested(dir)) {
        appendDetail(dir, L"Manual STOP detected; no shutdown or controller command");
        CloseHandle(mutex);
        return 0;
    }
    if (received == 0) {
        markFailure(dir, statusText.empty()
                             ? L"CH0 Audio received count was not exposed or stayed at zero for 30 seconds"
                             : statusText);
        CloseHandle(mutex);
        return 1;
    }

    appendResult(dir, L"PASS", received, (warmReboot ? L"Mode=warm; " : L"Mode=cold; ") + statusText);
    appendDetail(dir, L"PASS: received=" + std::to_wstring(received));

    if (dryRun) {
        appendDetail(dir, L"Dry-run complete; controller command and shutdown skipped");
        CloseHandle(mutex);
        return 0;
    }

    // Give the operator a cancellable grace period before arming external power.
    appendDetail(dir, warmReboot ? L"Waiting 15 seconds before warm restart"
                                : L"Waiting 15 seconds before controller request and normal shutdown");
    if (!waitInterruptible(dir, 15)) {
        appendDetail(dir, L"STOP detected; no controller command or shutdown");
        CloseHandle(mutex);
        return 0;
    }
    const auto powerRequest = warmReboot ? PowerCycleRequest::NotConfigured
                                        : requestExternalPowerCycle(dir, error);
    if (powerRequest == PowerCycleRequest::Failed) {
        markFailure(dir, L"External controller request failed: " + error);
        CloseHandle(mutex);
        return 1;
    }
    if (powerRequest == PowerCycleRequest::Armed)
        appendDetail(dir, L"External controller acknowledged power-cycle sequence");
    appendDetail(dir, warmReboot ? L"Requesting Windows warm restart" : L"Requesting normal Windows shutdown");
    const int rc = _wsystem(powerCommand);
    if (rc != 0) markFailure(dir, L"shutdown.exe failed with code " + std::to_wstring(rc));
    CloseHandle(mutex);
    return rc == 0 ? 0 : 1;
}


