# CAD 快照

`shore-spotter-jetson-case/` 是使用者指定的 `~/freecad/shore-spotter-jetson-case/` 檔案快照。
來源不是獨立 Git repository，因此將檔案本體納入 Shore-Spotter，同一份 commit 即可取回，沒有使用指向外部資料夾的 symlink。

本輪來源只有兩檔：Pocket Up-to-Face 最小失敗模型與既有結果 JSON；**不包含完整機殼主模型**。
來源未修改、未開啟或重算 FreeCAD 文件；內容只做 byte／SHA-256 核對，不把歷史診斷當成此次幾何驗收。
檔案清單、來源相對路徑與 hash 見 [snapshot.json](shore-spotter-jetson-case/snapshot.json)。

CAD 後續變更需從使用者指定的實際主檔取得，更新快照與 manifest，避免把這份副本誤認成所有主設計的同步資料夾。
