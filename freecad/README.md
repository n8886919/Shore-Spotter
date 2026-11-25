# FreeCAD 模型開發

PCB 機殼的設計原始檔是 [pcb_case.py](pcb-case/pcb_case.py)。從專案根目錄執行以下命令。此主機使用已安裝的 FreeCAD 26.3rc2，Python 需能載入 FreeCAD、Part 及 MeshPart。

```sh
export PYTHONPATH="$HOME/.local/opt/FreeCAD-26.3rc2/usr/lib"
~/.local/bin/freecad python freecad/pcb-case/pcb_case.py
```

用 FreeCAD 打開 `freecad/pcb-case/build/pcb_case.FCStd` 量測，切片使用同目錄的 `pcb_case.stl`。Git 追蹤 Python、驗證工具與文件。改動後重新執行上述命令即可更新兩個產物。

## 修改 PCB 機殼

尺寸單位為 mm。原點是核心骨架左前方安裝柱的軸心與骨架底面的交點。四柱沿 +X、+Y 排列，骨架沿 +Z 拉伸，USB 板凸出部伸向 −Z。

| 修改位置 | 對應設計 |
|---|---|
| `frame()`、`POST_PITCH` | 四柱中心距、圓弧連接的骨架、GPS 座與階梯孔 |
| `lilygo_and_servo()` | LilyGO 側窗與孔、舵機控制板支架、底部輕量化與螺絲避讓 |
| `usb_mount()`、`USB_HOLE_PITCH` | USB 板安裝壁、四角補強、螺絲孔、下方兩個螺母槽、電源開口 |

改孔距時先確認哪一排孔要跟著移動。例如 USB 的兩列安裝中心由中心線向兩側各取半個孔距，孔、補強與螺母槽共用該中心。改完重新產生模型，量孔中心與邊距，檢查補強肉厚、裝配空間及 STL 切片。腳本會檢查有效單一實體及封閉網格。

## 其他零件也可沿用

1. 將現用原模型釘選到可取回的 Git commit 或 tag。以原 FCStd 最終實體、草圖和裝配關係確認尺寸與方向。
2. 在 `freecad/<零件>/` 手寫一份可獨立重建的 Python。按板件、固定座、開口等部位組織幾何，共用裝配基準。生成物放該零件的 `build/`。
3. 第一次重建時先比對相同座標下的原件與新件，再提交等形版本。以下工具讀取各文件中單一頂層實體，分別列出增加和移除的體積。任一方向大於 0.00001 mm³ 即以非零狀態結束，並列出差異的位置。
4. 後續變更獨立提交，檢查差異只出現在預期區域，另量關鍵尺寸。需在零件上刻版本時，先提交幾何，再使用該 commit 的短雜湊刻字，另行提交。固定刻字值可讓同一版在之後的 checkout 重建時保持一致。

## PCB 機殼的遷移基準

原始 CAD 倉庫的提交 `f3c69b92dbabbcf2fabaf60c2b81fcd3ed723c81` 已匯入主倉庫，標籤為 `archive/pcb-case-fcstd`。備份主倉庫或移到其他主機時一併傳送此標籤。下列指令可取回原檔，並與**未刻字**重建版比對。

```sh
git show archive/pcb-case-fcstd:Shore-Spotter-Case.FCStd > /tmp/pcb-case-original.FCStd
~/.local/bin/freecad python freecad/compare_solids.py /tmp/pcb-case-original.FCStd freecad/pcb-case/build/pcb_case.FCStd
```

首次驗收時兩個 FCStd 均有一個有效實體，雙向相減的體積皆為 0.000000000 mm³。列印公差與實機裝配需以實物量測。
