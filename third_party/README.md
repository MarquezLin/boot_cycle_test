# 第三方執行元件

`ocr/` 保存完整 Tesseract runtime：引擎、相依 DLL、tessdata 語言檔與授權。
這是建置與發布的輸入資源，請保留；它不是可清除的建置產物。

Qt Creator 建置會將此資料夾複製到建置產物的 EXE 旁，名稱仍為 `ocr`。
`package_release.bat` 及 CMake 的 package 目標也從這裡收集資源，
發布 ZIP 中的位置是 `bin/ocr`。專案根目錄不再需要 `bin` 資料夾。

搬移完整開發專案時，請一併帶上本資料夾及 `distribution/licenses`。
來源、版本與元件雜湊清單位於 `distribution/licenses`。
