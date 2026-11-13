# CAD repository 索引

使用者更正的主模型目錄是 `~/freecad/Shore-Spotter-Case/`，它有自己的 Git repository。
本次在該 repository 提交 `Shore-Spotter-Case.FCStd`、既有 STL 及 README；commit **`f3c69b92dbabbcf2fabaf60c2b81fcd3ed723c81`**。
檔案大小／SHA-256、存檔時間與驗證範圍見 [reference.json](reference.json)。程式 repository 不重複拷貝模型。

FCStd ZIP／Document.xml 完整性與 STL 二進位長度／14,696 三角形通過，沒有開啟、重算或重新匯出模型。
STL 的匯出早於 FCStd 最後存檔，未確認兩者幾何一致；既有 `.snapshots` YAML 也未重新產生。

需要從 CAD 歷史取回主檔可用：

```sh
git -C ~/freecad/Shore-Spotter-Case show f3c69b92dbabbcf2fabaf60c2b81fcd3ed723c81:Shore-Spotter-Case.FCStd > /tmp/Shore-Spotter-Case.FCStd
```

原先誤指的 `~/freecad/shore-spotter-jetson-case/` 只有 Pocket 診斷資料，其工作目錄副本已移除；
先前 checkpoint 的診斷副本仍留在 Git 歷史，與這個正式外殼 repository 分開看待。原始兩個目錄都未改幾何。
