# 發布檢查（2026-11-10）

已執行 `package_release.bat /nopause`，Release 建置及 ZIP 打包成功。

- 套件：`release/GVFG_BootCycleTest-1.3.2-Windows-x64.zip`
- 大小：139,706,916 bytes；108 個 ZIP 項目。
- 入口：完整解壓後執行 `bin/GVFG_BootCycleTest.exe`。
- 範例：`examples/sample_capture_card_test.json`，待測 EXE 留空供現場選擇。
- 影片：`test_media/capture_card_test_1080p60.mp4`；已確認範例的相對路徑可解析，且套件影片與來源 SHA-256 相同。
- 已確認 `_CPack_Packages` 與解壓驗證副本均已清除；ZIP 不含個人設定與試跑資料。

從實際 ZIP 解壓，限制 PATH 為 Windows 系統目錄、清除 Qt plugin 環境變數後，14 項流程模型、離線擷取、影片比對、掃描器、分類、編輯、插入、繼電器協定、UI、流程事件、數值讀取、補充步驟及 OCR 測試全數 exit 0。驗證紀錄位於可重新產生的 `build/package_validation.json`。

以上為本機解壓隔離及合成／UI 測試；未在全新 Windows／VM 驗證，也未執行實體繼電器切換、斷電、重開機或擷取硬體循環。不同 APP 的背景繪圖、實際訊號品質與前景相容需求仍須現場試跑確認。
