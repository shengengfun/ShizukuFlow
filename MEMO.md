# ShizukuFlow 备忘录（时间轴 / 编辑，2026-10）

> 这份文件是给「下一次打开工程的人」看的：怎么用、为什么这么写、哪些坑已经踩过、改完怎么验。

## 1. 时间轴一分钟上手

时间轴自上而下：刻度尺 → 镜头条（胶片缩略图）→ 歌词行 → 波形 → **四层部件轨** → 动效行。

| 操作 | 结果 |
| --- | --- |
| 点刻度尺 / 波形空白 | 播放头跳过去（按住可拖） |
| 拖镜头块中间 / 左右边缘 | 平移镜头 / 裁剪时长 |
| 点部件块 | 选中该镜头的「整层部件」（同类多件合并成一块） |
| 上下拖部件块 | 换层（背景/装饰/文字/覆盖），落下后一条撤销 |
| 空白处拖框 | 框选多块（Shift 追加） |
| Shift 点块 | 加选 / 减选 |
| Ctrl 点部件块 | 选中所有镜头里的同一层 |
| Del / Backspace | 有部件选择先删部件，没有才删镜头 |
| Esc | 取消拖动 / 框选 / 清空选择 |
| 滚轮 | 横向平移 |
| Ctrl+滚轮 | 以光标为锚缩放 |
| Shift+F | 适配全片（换工程时也会自动适配一次） |
| 右键 | 部件：删除 / 同层全选；镜头：删除；空白：适配全片 |

缩放滑块在时间轴标题栏右侧，和 Ctrl+滚轮双向同步，标签是相对默认缩放（46px/s）的倍率。

## 2. 编辑与撤销

- 所有改动都走 `sf::ProjectEditor`：`apply`（单条）或 `applyBatch`（一批）。
- 批量是**原子**的：任一条失败就按逆序回滚，工程原样不动，不会入栈。
- 撤销栈存的是「编辑前工程快照」（`MainWindow::afterProjectEdit`），所以一条批量 = 一条撤销。
- 部件删除/换层的命令序列都是**倒序删 + 顺序插**，保证下标始终有效。
- 换工程/新建工程会清空撤销栈和选择，并重新适配全片。

## 3. 架构速览

```
TimelinePane (外壳: 标题栏/滑块/状态条)
  └ TimeCanvas (自绘: 绘制 + 命中 + 手势)
       ├ onEdit(EditCmd)            → MainWindow::applyEditCmd   → afterProjectEdit
       ├ onBatch(EditCmd 列表, 文案) → MainWindow::applyEditBatch → afterProjectEdit
       └ onSelect / onStatus / onSeek / onPlayToggle / onZoom
```

- 绘制顺序：刻度 → 镜头条 → 歌词 → 波形 → 四层部件 → 动效 → 游标/框选。
- 命中顺序与绘制**相反**（`hitTest` 倒序遍历 shots），多轨层叠时点到的就是最上面那块。
- 部件选择状态：`m_partSel`（镜头下标 + 层号），与镜头选择 `m_sel` 互斥（点部件轨以外会清掉）。
- 选中状态、拖动目标层（`m_dropLayer`）都只是视图状态，不碰工程；松手才发命令。

## 4. 踩过的坑（重要）

1. **窗口级 QAction 会抢走控件的按键**：`QKeySequence::Delete` 的 QAction 是 WindowShortcut，时间轴的 `keyPressEvent` 根本收不到 Del。所以删除逻辑必须放到 `MainWindow::deleteSelectedShot` 里先问时间轴（`TimelinePane::deleteSelection`）。Space 同理（两边都做同一件事，没出问题）。
2. **`setProject` 比指针判断不出换工程**：`m_project` 是同一个成员对象，载入新工程地址不变。要看 `project->path`；连续「新建工程」路径都是空，得由 `MainWindow::newProject` 调 `reloadProjectView()` 显式重置。
3. **`fitAll` 不能在 show 之前算**：启动时控件宽度还是布局中间值，算出来的 pps 偏小（全片缩在左半屏）。改为 `requestFitAll()` 记账，`showEvent` 里 `QTimer::singleShot(0)` 等布局稳定再 fit。
4. **`setTime` 每次调用都自动跟随会打断浏览**：编辑后外层会重设同一个 `t`。改成「时间真的变了才跟随」，并且全片可见时不跟。
5. **缩放滑块必须双向同步**：`onZoom` 回写滑块时用 `QSignalBlocker`，否则 `valueChanged → setPps → onZoom` 打转；滑块用对数映射，1.0× 附近手感才细。
6. **部件块只能纵向拖**：横向抖一下不该进入拖动（阈值只看 dy），光标用 OpenHand/ClosedHand 而不是 SizeAll，语义才诚实。
7. **窄块文字要省略**：直接 drawText 会被裁成半截字，全片缩放时糊成一片。改成 `elidedText`，只剩「…」就不画。
8. **多轨重叠的接缝检查要分 track**：`sf_lint` 早期把跨轨层叠误报成重叠，现在按 track 分组。
9. **离屏截图里中文会变方框**：`QT_QPA_PLATFORM=offscreen` 没有中文字体，截图只能看布局/配色，别拿它判断文字。

## 5. 已知边界（现在没做，别误以为是 bug）

- 没有「分割镜头 / 入出点 / 关键帧 / 复制粘贴 / 吸附 / 框选自动滚动」。
- `PartMove`（层内换序）、`ShotTrack`（换轨）只有核心命令和测试，UI 没有入口，等部件库面板一起做。
- 部件块横向拖动没有意义（部件渲染覆盖整个镜头区间），只有纵向换层。
- 撤销是整工程快照：镜头很多时内存和入栈耗时随规模线性增长；要上大规模先把快照换成 diff。
- 导出前体检只有 CLI：`sf_dump <工程> --lint`，还没接进导出流程。
- `sf_lint.{h,cpp}` 目前是未跟踪文件（CMake 已引用），提交时记得 `git add`。

## 6. 改完怎么验

```powershell
$env:PATH="D:/Project/Qt/6.10.3/mingw_64/bin;D:/Project/Qt/Tools/mingw1310_64/bin;$env:PATH"
cd d:/Project/ShizukuFlow/qt-shell
cmake --build build -j 4
.\build\sf_edit_test.exe                 # 期望 207 checks passed
.\build\shizuku_shell.exe --selftest     # 期望 RESULT: PASS (0 failed)
.\build\sf_dump.exe "..\projects\余韻.sf" --lint
```

`--selftest` 覆盖：工程往返、LRC 往返（≤5ms）、撤销/重做、批量原子性相关的核心路径。时间轴手势没有自动化测试，改动交互后至少手动过一遍：选块 → 上下拖 → Ctrl+Z、框选 → Del → Ctrl+Z、Esc、滚轮平移、Ctrl+滚轮缩放、Shift+F。

## 7. 文件地图

| 文件 | 职责 |
| --- | --- |
| `qt-shell/src/TimelinePane.cpp/h` | 时间轴全部：绘制、命中、选择、拖动、缩放、右键菜单 |
| `qt-shell/src/MainWindow.cpp` | 编辑入口、快照撤销、Delete 路由、「帮助 → 快捷键」 |
| `qt-shell/src/core/sf_edit.cpp/h` | `EditCmd` / `ProjectEditor` / 批量与逆操作 |
| `qt-shell/src/core/sf_lint.cpp/h` | 导出前体检（缺文件、黑场、空隙、重叠） |
| `qt-shell/tools/sf_edit_test.cpp` | 核心编辑单测（207 项） |
| `qt-shell/tools/sf_dump.cpp` | 工程导出/统计/`--lint` |
