# GVFG 冷／暖開機 audio 循環測試（2026-10-14）

推薦入口：`boot_cycle_test/bin/GVFG_BootCycleTest.exe`（2026-10-14）。
日常只需雙擊 **bin/GVFG_BootCycleTest.exe**。UI 與背景檢查已合併到同一個 EXE。
開啟視窗不會開始測試，請先選擇待測 EXE 與模式，再按試跑或正式測試。

## Qt 專案與畫面編輯

本工具已改為 C++／Qt Widgets，使用 CMake、AUTOMOC 與 AUTOUIC。
已驗證的環境為 Qt 6.10.2 msvc2022_64、Visual Studio 2022、Windows SDK。
CMakeLists.txt 使用標準 Qt 建置流程，配置階段沒有手動編譯偵測程式。

1. Qt Creator 開啟根目錄 `CMakeLists.txt`，選擇 Qt 6 MSVC 2022 x64 Kit。
2. 開啟 `source/mainwindow.ui`，使用 Qt Designer 編輯畫面。
3. 建置／執行目標 `GVFG_BootCycleTest`。

所有主要控制項與布局都定義在 .ui，mainwindow.cpp 處理 signals／slots 與狀態更新。
不要修改建置時自動產生的 ui_mainwindow.h。

| 檔案 | 職責 |
| --- | --- |
| `source/mainwindow.ui` | 視窗、模式選單、按鈕、狀態及紀錄區布局 |
| `source/mainwindow.cpp/.h` | Qt UI 操作、QProcess 背景程序、QTimer 更新狀態 |
| `source/main.cpp` | Qt 入口、同桌面單一 UI、登入自動執行入口 |
| `source/startup_manager.cpp/.h` | Windows Startup 安裝、解除、查詢 |
| `source/BootCycleTest.cpp` | Windows UIA audio 檢查、停止標記、電源控制 |

測試核心仍在背景程序執行，Qt UI 不會在主執行緒等待 UIA 檢查。
目前 UI 仍從本次 log 解析進度，新增多個測試項目時應再引入結構化的項目結果與執行器。

Qt runtime DLL 與 plugins 必須和 EXE 一起保留／部署，不能只複製 EXE。
原有登入啟動項目已同步更新為 bin 內的 EXE，不需要重新安裝。

專案根目錄只保留專案與說明，執行檔不與原始碼混放：

```text
boot_cycle_test/
  CMakeLists.txt
  README.md
  source/                  C++ 原始碼與 mainwindow.ui
  build/                   Qt Creator 建置目錄
  bin/                     日常執行的 EXE、Qt DLL、plugins、設定與紀錄
    history/               舊暖開機歷史紀錄
```

## 操作

- 左上選擇冷開機或暖重開機，設定自動保存到 test_settings.ini。
- 待測 APP：按「瀏覽…」選擇待測程式的 EXE；路徑保存於 Test/TargetExe，試跑、正式測試、下次登入自動測試都沿用。啟動 APP 時以其所在資料夾作為工作目錄。路徑不存在時顯示 FAIL，不執行電源動作。
- 試跑：檢查 audio，顯示 PASS／FAIL、接收 frame 數與本次紀錄；不關機、不重開機、不呼叫 UART。
- 正式測試：登入後等待 20 秒再啟動 APP，PASS 後等待 15 秒，執行所選模式的電源動作。
- 停止：建立 STOP，阻止尚未執行的控制動作。手動重新開始會清除 STOP／FAILED。
- 關閉測試中的視窗也會要求停止，等測試結束後再關閉即可。
- 開啟紀錄資料夾：查看歷史 CSV 與 log；視窗會載入近期歷史紀錄並追加本次進度。
- 測試執行中不能切換模式。再次開啟程式只會顯示已開啟的視窗。

## 在 UI 管理自動啟動

視窗直接顯示自動啟動的安裝狀態，每秒更新。
- 安裝自動啟動：保存目前冷／暖模式，在目前使用者的 Windows Startup 資料夾建立捷徑，參數為 --auto-run。安裝不會立即測試。
- 解除自動啟動：移除本工具的捷徑與先前 CMD 啟動檔，不刪除程式、模式設定或紀錄，也不建立 STOP。
- 已安裝／未安裝：顯示啟動項目是否存在及是否指向目前 EXE、帶有 --auto-run。
- 路徑已變更／設定不符／項目重複：按「更新自動啟動」，以目前開啟工具的位置重新建立捷徑；成功後移除舊 CMD，避免重複。
- STOP／FAILED 存在：即使已安裝，下次登入仍會跳過測試。手動開始新測試會清除標記。

解除只影響下次登入，若要停止本次測試請按停止。測試執行中安裝／解除按鈕暫時停用。
顯示的是啟動項目的安裝狀態；若在 Windows 工作管理員另行停用啟動項目，需在 Windows 中重新啟用。

| 模式 | 正式測試 PASS 後動作 |
| --- | --- |
| 冷開機 | 呼叫 UART 預留介面，再以 shutdown.exe /s /t 0 正常關機 |
| 暖重開機 | 以 shutdown.exe /r /t 0 重新啟動 Windows，不呼叫 UART |

## 判定與紀錄

Windows UI Automation 啟動 Customer Sample 的 Start，讀取 CH0 Audio 的 received=N frames。
30 秒內 N > 0 即 PASS；不檢查聲音振幅、播放或持續接收。
失敗會建立 FAILED，保留 APP 與電腦現場，不關機或重開機。

兩種模式共用 boot_cycle_results.csv 與 boot_cycle_detail.log，PASS detail 及每輪起始 log 記錄 Mode=cold／warm。
PASS 只代表本次 audio 檢查通過，不代表已完成電源循環或下一次開機。

目前目標固定為 C:\Users\mark\Desktop\GVFG_Customer_Sample_Release_1.0.0_20260923_1106\gvfg_qt_preview.exe。
更換 APP 版本需修改 source/BootCycleTest.cpp 的 kTargetExe 與 kTargetDir 後重新編譯。

## 冷開機外部控制器

requestExternalPowerCycle() 尚未傳送 UART 指令，會記錄 CONTROLLER_NOT_CONFIGURED。
因此目前冷開機 PASS 後只會關機，需要外部恢復供電，完整自動循環尚未接通。

未來需確認 COM port、baud rate、封包、ACK、取消機制及電源切換時序。
控制器必須獨立供電，自行完成延遲斷電 → 保持斷電 → 恢復供電。
斷電保持 20 秒從實際斷電起算，是否足夠放電需硬體驗證。
只有整個序列獲確認才回傳 Armed；傳送／ACK 失敗回傳 Failed，阻止關機。
關機延遲需容納實際 Windows 關機時間；APP 阻擋關機時，固定時間切電仍可能變成運作中斷電。
STOP 目前不能撤回控制器已接受的序列，UART 整合時需另外實作取消協定。
BIOS 需設定恢復供電後自動開機。

## 維護檔案

- source/：C++ 原始碼，保留以便修改與重新編譯。
- .qtcreator/：Qt Creator 的使用者設定，保留目前 Kit 與建置設定。
- build/：Qt Creator 建置產物；目前保留使用者建立的 Debug 建置目錄。

已移除舊 build_test.cmd 與它產生的 Release 建置目錄，之後用 Qt Creator 編譯即可。
Qt Creator 的建置目錄可另選；建置出的 EXE 位於該建置目錄下的 bin，設定與紀錄會存到它旁邊。
Qt Creator 建置不會自動更新專案根目錄下的日常用 bin；需要更新日常版本時，選擇 Release Kit／組態建置，
關閉工具後，以對應 Qt Kit 的 windeployqt --release 部署建置出的 EXE，
將 EXE、Qt DLL、plugins 複製到專案 bin，保留既有 test_settings.ini、CSV／log 與 history。
Debug 版不應直接覆蓋日常使用的 Release EXE 而沿用 Release Qt DLL。
執行 Qt 版需要 Microsoft Visual C++ runtime；本機已可執行，額外的 VC runtime 安裝程式已清除。


登入自動啟動使用 GVFG_BootCycleTest.exe --auto-run，顯示 UI 並依保存的模式正式測試。
STOP／FAILED 存在時會跳過，不會自動清除。沒有保存設定時預設冷開機。
Startup 是登入後執行，無人值守需自行設定 Windows 自動登入。
已安裝的 GVFG_Boot_Cycle_Test.cmd 仍相容；按 UI 安裝會轉成捷徑，並移除該舊 CMD。

舊 warm_reboot_test 與其登入啟動設定已移除。歷史 CSV／log 保存於 bin/history/warm_reboot_test/，日常使用新版即可。



## 發布給其他使用者

使用 Release Kit 建置。CMake 提供 install 與 package 目標，透過 Qt 部署流程收集 EXE、Qt DLL、plugins 及 VC runtime 部署檔案；ZIP 僅包含安裝規則列出的檔案，排除個人設定、STOP、FAILED、CSV、log、history 與原始碼。

在 Qt Creator 可新增 CMake 建置步驟，目標選擇 package；或在已配置 MSVC 環境的終端執行（將 <Release建置目錄> 換成實際位置）：

```text
cmake --build <Release建置目錄> --target package
```

ZIP 產生於該建置目錄，名稱為 GVFG_BootCycleTest-1.0.0-Windows-x64.zip。
如果只要部署資料夾，可使用：

```text
cmake --install <Release建置目錄> --config Release --prefix <新的部署資料夾>
```

部署目錄應使用新的空資料夾，避免保留舊檔。不要使用 Debug 版當作對外發布版。

收件者操作：

1. 解壓縮整包到可寫入的位置，保留 bin 與 plugins 等完整結構。
2. 若包內含 VC runtime 安裝程式，依目標電腦環境安裝。待測 APP、對應驅動與硬體另行準備。
3. 開啟 bin/GVFG_BootCycleTest.exe，按「瀏覽…」選擇待測 APP，選擇模式，先試跑。
4. 確認結果後按「安裝自動啟動」，下次登入會使用目前工具位置及該資料夾保存的設定。
5. 若搬移工具，從新位置開啟後按「更新自動啟動」。搬移時一併保留設定與 Qt runtime 檔案。

試跑、正式測試與安裝自動啟動前，都會確認待測 EXE 與設定／紀錄寫入能力。檢查失敗會在狀態及本次紀錄區顯示原因，不啟動測試或執行電源動作。這是檔案與寫入檢查，不能取代待測 APP 的驅動、UIA 或硬體試跑。
## 紀錄與累計次數

畫面顯示累計次數，區分正式測試與試跑，並列出 PASS、FAIL、停止與未完成數量。每次啟動工具會從 boot_cycle_detail.log 重建統計，因此重開機後保留；開始後尚未出現結果的輪次列為未完成。STOP／FAILED 導致跳過或環境檢查未通過時，不算一次測試。

同一輪先 audio PASS、再發生電源控制 FAIL 時，統計改列 FAIL，不重複計次；audio PASS 後手動停止電源動作仍保留 PASS。PASS 不代表已完成實際電源循環。累計包含所有已保存的模式與待測 APP 紀錄，更換 APP 不會自動歸零。

紀錄區顯示最近最多 3000 行（載入時最多讀取最後 512 KiB 文字），完整 log／CSV 可從「開啟紀錄資料夾」查看。累計統計使用完整 log，不受畫面行數限制。

「清除紀錄」確認後刪除目前的 boot_cycle_detail.log 與 boot_cycle_results.csv，累計次數歸零；不刪除設定、STOP／FAILED 標記及 history 資料夾。測試執行中按鈕停用。清除不可復原，需保留紀錄時請先另行備份。