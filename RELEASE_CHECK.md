# 新電腦發布檢查（2026-10-15）

交付：release/GVFG_BootCycleTest-1.1.1-Windows-x64.zip（136,848,998 bytes，約 131 MiB）。
完整解壓後執行 bin/GVFG_BootCycleTest.exe。

已包含工具、Qt DLL／plugins、qt.conf、官方 VC++ x64 runtime 安裝程式、Tesseract 5.4.0.20240606 引擎與相依 DLL、英文／繁體中文語言資料及授權說明。可離線執行，不需 Qt Creator 或額外辨識服務。VC++ runtime 尚未安裝的電腦請執行 bin/vc_redist.x64.exe。

從實際 ZIP 解壓，將程序 PATH 限縮至 Windows 系統目錄並清空 Qt plugin 環境變數後，建立測試項目的 UI 預覽、流程自我檢查、文字辨識自我檢查均 exit 0。辨識的 Running、Not Running、繁體「執行中」均通過；報告的引擎路徑確認位於解壓後套件的 bin/ocr，而不是本機安裝目錄。

ZIP 共 104 個項目。檢查未包含個人設定、STOP／FAILED、匯入快照或測試紀錄。套件檢查及 SHA-256 在 build/portable_package_check.json；解壓測試位置在 build/portable_check_path.txt。

跨電腦仍須準備待測 APP、其相依元件與驅動、擷取硬體及訊號來源。匯入 JSON 時調整待測 EXE 與工作目錄，並確認控制項、視窗布局、DPI 與框選區域。舊流程的 OCR 引擎路徑若不存在，會自動尋找套件內引擎。

先試跑（不關機），成功後再安裝目前使用者的登入自動啟動並正式測試。搬移工具後請更新自動啟動。冷開機的外部 UART 電源控制尚未完成。

上述是本機解壓隔離測試；尚未在未安裝任何開發工具的 Windows／VM 上測試，也未執行實際冷／暖開機或硬體循環。第三方元件的來源與授權資訊位於 distribution/licenses，並隨 ZIP 一起發布。
