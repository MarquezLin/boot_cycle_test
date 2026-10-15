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

修改程式後，直接雙擊專案根目錄的 `package_release.bat`。它會載入 Visual Studio x64 編譯環境，在獨立的 `build/package-release` 目錄設定並建置最新 Release，再收集 Qt、VC++ 安裝程式、OCR 引擎／DLL／語言資料及授權，輸出 ZIP 到 `release`。不需要先在 Qt Creator 手動建置。

顯示 `SUCCESS` 才表示本次完成；失敗時會顯示錯誤並暫停，完整紀錄位於 `build/package_release.log`。ZIP 的版號取自 `CMakeLists.txt` 的 `project(... VERSION ...)`；同版號重新打包會覆蓋該 ZIP，不同版號會保留舊包。不要在打包時修改原始碼。

此批次檔供開發電腦使用，收件者只需收到 ZIP。預設使用 `C:/Qt/6.10.2/msvc2022_64`，並以 vswhere 尋找 Visual Studio C++ Build Tools。工具位置變更時，可修改批次檔上方的預設路徑，或設定 `QT_ROOT`、`CMAKE_EXE`、`NINJA_EXE`、`VS_ROOT` 環境變數。請保留 `third_party/ocr` 及 `distribution/licenses`；搬移原始碼到另一台開發電腦時也要帶上這些發布資源。命令列自動化可使用 `package_release.bat /nopause`，成功回傳 0，失敗回傳非 0。

使用 Release Kit 建置。CMake 提供 install 與 package 目標，透過 Qt 部署流程收集 EXE、Qt DLL、plugins 及 VC runtime 部署檔案；ZIP 僅包含安裝規則列出的檔案，排除個人設定、STOP、FAILED、CSV、log、history 與原始碼。

在 Qt Creator 可新增 CMake 建置步驟，目標選擇 package；或在已配置 MSVC 環境的終端執行（將 <Release建置目錄> 換成實際位置）：

```text
cmake --build <Release建置目錄> --target package
```

ZIP 產生於該建置目錄，名稱為 GVFG_BootCycleTest-1.1.1-Windows-x64.zip。打包前請保留 third_party/ocr 的完整引擎、相依 DLL、tessdata 及 distribution/licenses；缺少引擎時 package 目標會直接失敗，避免產生缺件套件。
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
## 通用測試流程（1.1 開發版）

主畫面按「通用測試流程…」開啟流程編輯器，可先獨立試跑，並按「匯入開關機測試」套用到主畫面。啟用匯入流程後，試跑、正式測試與登入自動啟動皆執行匯入快照，依整份流程結果決定是否進入關機／重啟倒數，並記入主要統計；未啟用時沿用固定 CH0 audio 檢查。

1. 選擇待測 EXE、啟動參數與工作目錄（工作目錄留空時使用 EXE 所在資料夾）。
2. 新增、刪除、上移／下移步驟，在下方設定所選步驟。
3. 先執行「啟動 APP」及「等待視窗」，再設定操作與讀取。
4. UIA 控制項可按「取控制項」，在倒數 3 秒內把滑鼠停到待測控制項上；名稱採完全符合，也可填 regex: 加正規式。名稱與 AutomationId 皆填時須同時符合；多個符合項會拒絕操作。
5. 位置點擊與 OCR 可按「擷取並框選」，在截圖上拖曳；位置點擊使用框中心，OCR 使用整個框。座標相對於視窗內容區，以實體像素保存。改變 APP 布局、視窗尺寸或 DPI 時需重新框選並試跑。
6. 選擇步驟按「單步試跑」，確認結果與原始值，再執行完整流程。
7. 按「保存流程」保存 JSON；下次開啟編輯器會嘗試載入上次保存的流程。欄位的「自動套用」只更新目前編輯內容，不等於已寫入 JSON。

支援啟動 APP、等待主視窗、UIA Invoke 點擊、視窗位置點擊、Unicode 文字輸入、Enter／Tab／Esc／Ctrl+A／Ctrl+C／Ctrl+V／Alt+F4、等待時間、UIA 文字讀取、OCR、讀取值判定。

讀取步驟的預期值留空時只讀取；填入預期值會在逾時前輪詢等待符合條件。字串可相等／包含／正規式符合；數字可比較或填「最小值,最大值」範圍。數字判定取第一個數字；若文字有多個數字，請在讀取步驟的「擷取正規式」填入含括號群組的正規式，例如 received=([0-9]+)，取出 received 的數值。無法解析不會當作零。

「判定讀取值」使用同一流程最近一次成功讀取的值；單步試跑可沿用最近一次讀取值。完整流程會重新開始讀取狀態。編輯讀取來源後，請重新執行讀取步驟。

發布套件內附完整 Tesseract runtime，預設自動選用 bin/ocr/tesseract.exe。舊流程保存的引擎路徑若不存在，會重新尋找套件內引擎。語言可填 eng、chi_tra 或 eng+chi_tra，須保留對應 traineddata；使用其他語言時需自行補上語言檔。

每次流程試跑／完整執行在 EXE 旁的 flow_results 建立獨立目錄，保存實際流程 JSON、flow.log、result.txt、OCR 區域 PNG；步驟失敗時也會嘗試保存 failure.png。主畫面「開啟紀錄資料夾」可找到這些目錄；「清除紀錄」只清除原本開機測試 CSV／log，不刪除 flow_results。

截圖從桌面顯示內容依待測視窗客戶區擷取，保留原始像素，以支援 GPU 預覽。截圖前會將超出螢幕的待測視窗移回目前螢幕可用範圍，僅移動位置而不主動縮放，以保留框選座標；視窗本身大於可用範圍時會提示縮小後重新框選。視窗必須可帶到前景；偵測到取樣點被其他視窗遮擋時會提示，請移開遮擋，桌面鎖定時無法正常擷取。多個同 APP 視窗可在各步驟的「此步驟待測視窗」選單指定，選擇會隨流程保存；同名視窗無法唯一識別時會拒絕操作。啟動器另外產生子程序的 APP 尚未支援自動追蹤。UIA 原生呼叫的執行時間受待測 APP 的 provider 影響，停止會在該呼叫返回後生效。

來源：flow_dialog.ui 定義編輯器、region_dialog.ui 定義框選視窗、flow_model 處理 JSON／條件、flow_runner 在背景執行 Windows 操作與 OCR 程序。執行器不在 UI 執行緒等待測試。通用版本新增修改保留在工作區，第一版可由 Git commit e1505cd 找回。
## 可選測試範例

流程編輯器有範例下拉清單，可選文字來源 UIA／OCR，以及「追加檢查步驟」或「取代流程（含啟動／等待）」。預設追加，可依序加入多個檢查。取代會詢問是否覆蓋目前未保存的步驟。範例不猜測 APP 的 Start 按鈕；需要開始擷取時，請在檢查前加入該 APP 的點擊步驟。

| 範例 | 預設條件 |
| --- | --- |
| CH0 audio received | received > 0 |
| Running 狀態 | Running 或 Status: Running；排除 Not Running、Running=false |
| 1920 × 1080 | 接受 x、X、×、* 分隔及空白 |
| 約 60 FPS | 59～61，包含 59.94；可自行改範圍 |
| 整合檢查 | 依序檢查 Running、1920 × 1080、約 60 FPS |
| Video received frames | video 接收計數 > 0 |
| 預覽區有內容 | 框選區域亮度標準差 ≥ 8，或前後截圖平均 RGB 差異 ≥ 8；支援純黑／純白切換 |
| 預覽畫面有變化 | 至少相隔 500 ms 的截圖平均 RGB 差異 ≥ 2；取樣間隔輪替，允許純色畫面互換 |

文字範例載入後必須取 UIA 控制項或框選 OCR 區域；整合範例的三個步驟需分別設定。UIA 請選正確的即時狀態／實際訊號值，不要選設定欄位。控制項有 AutomationId 時優先使用該 Id，不同時限制會改變的 Name；沒有 Id 而 Name 會變動時，請自行設定 regex: 名稱條件，或使用 OCR。

影像範例不需要 OCR 引擎，但必須只框選預覽畫面內部，排除工具列、邊框、OSD 文字與其他 APP。門檻使用 0～255 量級，可在「預期值」調整。黑白互換可以透過前後截圖差異通過，固定純色則無法區分有效測試圖與無訊號黑場。畫面變化僅表示截圖改變，不測量實際 FPS；有限取樣仍可能錯過切換，錯誤提示／雜訊也可能產生差異，應配合 APP 的訊號狀態／接收計數交叉檢查。這些檢查不能證明擷取卡或外接螢幕的實際輸出。

影像檢查保存 step_N.png；變化檢查另存 step_N_before.png，log 顯示計算值與門檻。範例條件與影像演算法已用合成資料及獨立測試視窗檢查，目標 APP／硬體與 OCR 辨識準確度仍需自行試跑確認。
## 勾選建立與通用字串比對

按「建立測試項目…」，直接勾選 Running、解析度、FPS、畫面內容／變化、音訊／影像計數，或勾選「比對字串」並填入預期文字。比對方式可選等於（忽略大小寫與空白）、包含（忽略大小寫）、完全相同（區分大小寫與空白）或包含（區分大小寫）。字串按原文比對，不視為正規式；空白預期文字不能建立。

建立後選取該步驟，再「指一下文字／按鈕位置」指定 UIA 文字控制項，或「框選文字或預覽區域」使用 OCR。框選後可沿用已設定的比對條件。按單步試跑確認讀取值，再保存流程。插入或追加新檢查會保留原有目標與畫面上的測試結果。

## 框選文字與狀態判定（2026-10-15）

套件內附 Tesseract 5.4.0.20240606。工具旁 ocr/tessdata 包含英文、繁體中文語言資料及授權，預設自動使用 chi_tra+eng；繁體模型來自官方 tessdata_best，並補齊其直排模型依賴。

1. 選擇並開啟待測 APP，讓要檢查的文字顯示在視窗內。
2. 按「框選文字或預覽區域」，在截圖上拖曳圈出文字，按確認。不必先切到 OCR 類型。
3. 會開啟文字預覽視窗，自動顯示辨識結果。選中 Running／解析度／FPS 等既有文字步驟時，預設沿用原判定；選其他步驟時會新增文字檢查，不會取代啟動等動作。
4. 若要自訂，取消「沿用既定條件」，直接選文字等於、包含文字、數字大於或數字範圍，再填預期值。例如狀態必須為 Running，建議選「文字等於」並填 Running；包含文字也可能符合 Not Running，不適合拿來區分這兩個狀態。
5. 按「檢查這次辨識結果」看 PASS／FAIL，確認後按「保存這個文字檢查」。主畫面再單步試跑，最後保存流程。

文字等於會忽略英文大小寫與空白；Not Running 不會等於 Running。空白或辨識錯誤不會算 PASS。已以合成圖片驗證 Running、深底 Not Running 及繁體「執行中」；這不代表所有字體／大小／背景都能正確辨識。請看預覽文字是否正確，必要時重新框選、保留少量周圍空間或放大待測 APP 的文字。

目前畫面內容類型仍用框選範圍做影像內容／變化檢查，不會開啟文字判定視窗；位置點擊類型則保留框選中心作為點擊位置。

### 通用流程的 APP 重用與等待狀態

「重用已開啟的待測 APP；未開啟時才啟動」預設勾選，舊流程未指定此設定時也預設啟用。開啟步驟會重用同 EXE 已開啟視窗所屬的唯一程序；偵測到多份程序時會提示，請關閉多餘實例。取消勾選才每次啟動新程序。此選項只適用通用流程，不改變冷／暖開機測試。

「指一下文字／按鈕位置」依滑鼠實際指到的待測 APP 視窗保存標題與控制項；Start 按鈕應位於主視窗，影像檢查則選預覽視窗。有 AutomationId 的步驟若指定視窗失效或找不到控制項，會在同一待測程序的已開啟視窗中重新查找；只有唯一符合時才繼續，並更新步驟視窗設定，請保存流程。僅依位置的步驟須重新指選，OCR／影像區域不會套用這項自動重新定位。執行中會顯示步驟、等待秒數與目前讀取值；逾時條件在外部 UIA 呼叫返回後檢查，外部 provider 未返回時無法保證立即停止。

預覽建立等待：OCR／影像檢查會在該步驟的逾時內等待指定視窗出現（例如 Start 後延遲建立的預覽），等待時顯示目前狀態；視窗出現後先自動移回螢幕可用範圍，再擷取與判定。逾時仍找不到才 FAIL。


### 插入測試步驟

需要插入等待時，選取插入位置，按「在前插入…」或「在後插入…」，只勾選「等待時間（Delay）」並輸入秒數（0.1～600 秒），即可插入等待步驟，不需指定視窗、按鈕或框選區域。與其他項目一起勾選時，產生順序是點擊按鈕、等待、其餘檢查；可用上移／下移調整。

選取現有步驟，按「在前插入…」或「在後插入…」，使用勾選／文字介面建立新檢查。原有步驟的控制項名稱、AutomationId、待測視窗、框選區域及畫面上的 PASS／FAIL／讀取值會隨步驟保留，新步驟顯示未執行。插入模式不會取代流程，也不會自動加入開啟 APP／等待視窗。未選取列時插入至尾端；取消不改動流程。刪除只移除所選步驟；上移／下移時目標設定、測試結果與讀取值跟著步驟移動。追加檢查也會保留舊結果；執行完整流程時則仍會重新執行全部步驟。

## 將通用流程匯入開關機測試

建立測試項目時全部不預選，請自行勾選與指定目標。通用流程編輯器新增「匯入開關機測試」按鈕：完成設定後按此鈕，即保存流程快照至工具旁 boot_flows 資料夾，並同步主要畫面的待測 APP 與「使用匯入的通用流程」設定。也可在主畫面按「匯入流程…」選擇已保存的 JSON。匯入不會立即執行測試；編輯器仍保留原本的目標、步驟與結果。

先回主畫面按「試跑（不關機）」確認。啟用匯入流程時，開機後執行整份流程，全部通過才在正式模式進入可停止的 15 秒倒數，然後依冷／暖模式關機或重新啟動。失敗會留下 FAILED 標記並停止循環；STOP 會中止流程且不執行電源動作。未啟用時沿用原本 CH0 audio received > 0 檢查。已安裝的自動啟動仍會讀取目前工具目錄的設定，沿用所匯入的流程。

主畫面顯示整份流程的最終結果，內部每一步的通過不會先算整份 PASS。每次執行保存至 boot_flow_results（流程、截圖與結果），總次數與 CSV 仍使用主要開關機測試的紀錄。通用流程 CSV 的 received_frames 為 0，詳細欄位記錄流程來源，這個 0 不代表音訊檢查結果。

匯入的是獨立快照；之後修改編輯器內容時，須再次匯入才會套用到開關機循環。請保留 boot_flows、test_settings.ini，以及待測 APP、OCR 元件與相關資源。發布 ZIP 不包含本機匯入快照、設定或測試紀錄，另一台電腦請自行匯入其可使用的流程。

驗證範圍：獨立資料夾試跑已驗證通過、失敗建立 FAILED、執行中 STOP 及無電源動作；主畫面匯入保存與統計亦已檢查。未執行實際冷／暖開機循環或外部控制器驗證。
