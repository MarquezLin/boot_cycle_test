待測程式放置區

請將待測 APP 的 EXE、DLL 與必要資源完整放入此資料夾。此資料夾的全部內容會隨 package_release.bat 一起打包。
範例目前指向 GVFG_Customer_Sample_Release_1.0.2_20261110/gvfg_qt_preview.exe。更換 APP 或資料夾名稱時，請同步修改腳本的 executable 與 workingDirectory。

沒有待測 APP 仍可先匯入、查看與編輯腳本；執行前才需準備好 EXE 及相依檔案。若使用自動定位，只有 dut 最上層唯一的 EXE 會被自動採用，多個或子目錄內的 EXE 請在編輯器指定。
