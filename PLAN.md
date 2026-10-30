# ShizukuFlow 实施计划

> 目标：一个**像 Aegisub 一样直观**的术曲风格文字PV 综合制作软件。
> 原生工程格式 `.sf`（JSON），打包格式 `.sfp`（zip），输出视频与动图，联动岚珠工具箱等现有软件。
> 制定日期：2026-10-30

---

## 0. 已确认的技术决策

| 项 | 决策 |
|---|---|
| 技术栈 | **Tauri 2 + React 19 + Tailwind v4 + Rust**（与 LanzhuToolNX / ShiorikoTrans / RinaDown 生态一致） |
| 渲染引擎 | **自研 Canvas2D 绘制 + WebGL2 合成**，每帧是歌曲时间 `t` 的纯函数 |
| 工程格式 | **双格式**：`.sf` = JSON（日常编辑、可 diff），`.sfp` = zip（自包含分发） |

### 为什么值得自研渲染引擎

`vocaloid-style-mv-pipeline` 已经验证：**帧 = f(t) 的纯函数** 是整个架构的基石。它带来三个决定性好处：

1. **可复现** —— 同样的 `.sf` 在任何机器上渲出逐字节相同的结果
2. **可并行** —— 任意帧可独立渲染，多 worker 横向扩展
3. **可增量** —— 改了一个镜头的参数，只重渲那几秒，不用重跑全片

走 ffmpeg + libass 路线（即纯 ASS 特效）做不到第 2、3 点，因为它是一条单向的时间轴流水线，没有"随机访问任意帧"的能力。

---

## 1. 参考资产（已在 `ShizukuFlow/` 下）

| 目录 | 参考什么 |
|---|---|
| `Aegisub/` | **交互手感的唯一参考**。`audio_box.cpp` `audio_display.cpp`（波形打轴）、`audio_karaoke.cpp`（逐字 K 值）、`ass_karaoke.cpp`（卡拉OK数据结构）、`async_video_provider.cpp`（视频帧异步解码）、`ass_override.cpp`（样式覆盖标签） |
| `karaoke-templates/` | 496 个 `.ass` 特效模板（`ASS Template/`）—— 演出效果的现成语料库，用于归纳部件分类与参数体系 |
| `vocaloid-style-mv-pipeline/` | 渲染架构范本：`frame(t)` 纯函数 + Playwright 抓帧 + ffmpeg 编码；含 JIZURA 字面层集成方式 |
| `JIZURA/` | 演出部件的**分类学**：10 大类（布局/登场/保持/退场/装饰/文字加工/背景/相机/画面效果/转场），共 860 个部件 |
| `sihaiwuzhou-pv/` | 2.5D 空间感（带纹理平面 + 镜头穿行）、HDR 分组调色、分段 `frame(t)` 与交接画面 |
| `pv-tool/` | 后处理效果清单（色差/glitch/扫描线/胶片颗粒/半调/暗角）、运动检测 HUD |
| `Aegisub-Motion/` | 运动跟踪数据驱动合成 |

---

## 2. 架构总览

```
┌─────────────────────── React 19 前端（WebView2）────────────────────────┐
│  ┌──────────┐ ┌──────────────────┐ ┌─────────────┐ ┌───────────────┐  │
│  │ 部件库    │ │   演出预览画布     │ │ 波形时间轴    │ │  检查器        │  │
│  │ Library  │ │  PreviewCanvas   │ │  Timeline   │ │  Inspector    │  │
│  └──────────┘ └──────────────────┘ └─────────────┘ └───────────────┘  │
│        │              ▲                   │                │           │
│        └──────────────┼───────────────────┴────────────────┘           │
│                       │        Zustand 单一工程状态树                    │
│                       ▼                                                 │
│              ┌──────────────────┐                                       │
│              │  renderFrame(t)  │  ← 纯函数，无任何副作用                 │
│              │  Canvas2D 绘制    │                                       │
│              │  WebGL2 合成/后处理│                                       │
│              └──────────────────┘                                       │
└──────────────────────────────┬──────────────────────────────────────────┘
                               │  Tauri IPC（二进制 channel，零拷贝）
┌──────────────────────────────▼──────────────────────────────────────────┐
│                        Rust 后端（src-tauri）                            │
│  ┌────────────┐ ┌────────────┐ ┌──────────┐ ┌───────────────────────┐  │
│  │ 工程读写    │ │ 渲染调度    │ │ 音频分析  │ │  工具链定位            │  │
│  │ sf / sfp   │ │ 抓帧→管道   │ │ BPM/节拍  │ │  复用岚珠查找逻辑      │  │
│  └────────────┘ └────────────┘ └──────────┘ └───────────────────────┘  │
│         │              │                                    │           │
│         ▼              ▼                                    ▼           │
│    project.sf     ffmpeg stdin（rawvideo）      %LANZHUTOOL_TOOLS%      │
│    *.sfp(zip)     → MP4 / GIF / APNG / WebP     tools/（递归索引）      │
└─────────────────────────────────────────────────────────────────────────┘
```

**渲染数据流（关键，决定性能上限）**

```
renderFrame(t) → WebGL2 framebuffer → gl.readPixels() → Uint8Array
      → Tauri channel 批量回传（避免逐帧 IPC 开销）
      → Rust 写入 ffmpeg stdin（-f rawvideo -pix_fmt rgba）
      → 编码器输出 MP4 / GIF / APNG
```

> ⚠️ 绝不用 `canvas.toDataURL()`（base64 编码会吃掉 30%+ 吞吐）。
> 必须走 `readPixels` + 二进制 channel。这是本方案与浏览器方案的性能分水岭。

---

## 3. 工程格式规范

### 3.1 `.sf` —— JSON 工程文件（日常编辑）

```jsonc
{
  "format": "ShizukuFlow",
  "version": 1,
  "meta": {
    "title": "残光", "artist": "NikusonP",
    "created": "2026-10-30T07:00:00+08:00",
    "modified": "2026-10-30T07:00:00+08:00",
    "generator": "ShizukuFlow 0.1.0"
  },

  "canvas": {
    "width": 1920, "height": 1080, "fps": 30,
    "background": "#000000",
    "safeArea": { "enabled": true, "margin": 0.05 }
  },

  "media": {
    "audio": { "path": "audio/song.wav", "sha1": "...", "duration": 226.0 },
    "reference": [{ "path": "ref/bg.mp4", "type": "video" }]
  },

  "timing": {
    "bpm": 140, "offset": 0.12,
    "beats": [0.12, 0.549, 0.977, "..."],
    "downbeats": [0.12, 1.83, "..."],
    "source": "auto"            // auto | manual | imported
  },

  "styles": [{                  // 可被任意镜头引用的样式定义
    "id": "main", "font": "Noto Serif SC", "weight": 900,
    "size": 96, "letterSpacing": -0.02, "lineHeight": 1.1,
    "fill": "#FFFFFF", "stroke": { "color": "#000000", "width": 4 },
    "shadow": { "offset": [0, 4], "blur": 8, "color": "#00000080" }
  }],

  "lyrics": [{
    "id": "L001",
    "start": 12.34, "end": 15.67,
    "text": "我还记得黎明的颜色",
    "syllables": [             // 逐字/逐音节，Aegisub K 值的等价物
      { "text": "我", "start": 12.34, "end": 12.62 },
      { "text": "还", "start": 12.62, "end": 12.89 }
    ],
    "style": "main"
  }],

  "shots": [{                  // 分镜 = 演出部件的组装
    "id": "S001",
    "start": 12.34, "end": 13.10,
    "lyricRefs": ["L001"],
    "layers": {
      "background": { "part": "solid", "params": { "color": "#0B0B0F" } },
      "text":      { "part": "type.hero.center", "params": { "scale": 1.0 } },
      "decor":     [{ "part": "frame.rule", "params": { "weight": 2 } }],
      "overlay":   { "part": "grain", "params": { "amount": 0.08 } }
    },
    "motion": {
      "entrance": { "part": "slide.up",   "ease": "expo.out", "duration": 0.18 },
      "hold":     { "part": "micro.drift" },
      "exit":     { "part": "cut.hard" },
      "camera":   { "part": "push.in", "strength": 0.04 }
    },
    "transition": { "part": "slice.h", "duration": 0.12 },
    "seed": 84213,             // 随机部件抽样的种子 → 可复现
    "locked": false            // 锁定的镜头不参与「一键换构图」
  }],

  "assets": [
    { "id": "A001", "type": "image", "path": "assets/bg_01.png", "sha1": "..." },
    { "id": "A002", "type": "font",  "path": "assets/fonts/MyFont.ttf", "sha1": "..." }
  ],

  "export": {
    "preset": "web-1080p",
    "video": { "codec": "h264", "crf": 18, "preset": "medium" },
    "gif":   { "fps": 12, "width": 640, "dither": "sierra2_4a" },
    "apng":  { "fps": 24, "colors": 256 }
  }
}
```

**设计原则**

- **外链资源 + sha1 校验**：`assets/` 用相对路径引用，sha1 用于打开工程时检测素材缺失/变更
- **seed 固化**：任何"随机"都必须把种子写进 `.sf`，否则可复现性立刻崩塌
- **层级固定为 5 层**：`background / text / decor / overlay / transition`，与 JIZURA 的分类学对齐，便于日后移植部件
- **lyrics 与 shots 分离**：一行歌词可以横跨多个镜头（`/` 切分镜），一个镜头也能引用多行（合唱）

### 3.2 `.sfp` —— 打包工程（zip 容器）

```
project.sfp  (zip)
├── project.sf          # 与 .sf 完全一致的 JSON，不做任何改写
├── manifest.json       # 版本、资源清单、打包时间、打包器版本
└── assets/             # 全部素材（图片/字体/音频/参考视频）
    ├── audio/song.wav
    └── ...
```

- 打开 `.sfp` = 解压到临时目录 → 加载 `project.sf` → 落盘时回写 zip
- 保存为 `.sfp` 时自动执行资源收集（把外链素材复制进包体并改写为相对路径）

> 类比：`.sf` 是源码，`.sfp` 是发行包。日常用 `.sf` 走版本控制，交付归档用 `.sfp`。

---

## 4. 界面布局（Aegisub 手感的映射）

Aegisub 的经典布局是「网格 + 视频 + 波形 + 样式」四区。ShizukuFlow 做同构映射：

```
┌────────────────────────────────────────────────────────────────────────┐
│  工具栏  新建/打开/保存(.sf .sfp)  │  撤销重做  │  导出  │  发送到岚珠    │
├───────────┬──────────────────────────────────────────┬─────────────────┤
│           │                                          │                 │
│  部件库    │            演出预览画布                    │   检查器         │
│  Library  │         （实时渲染，100% 缩放）             │   Inspector     │
│           │                                          │                 │
│ ┌───────┐ │                                          │  ┌───────────┐  │
│ │布局   │ │      术中术：叠加构图辅助线                  │  │ 字体家族   │  │
│ │登场   │ │      （三分/黄金螺旋/安全框）                │  │ 字号/字距  │  │
│ │保持   │ │                                          │  │ 描边/阴影  │  │
│ │退场   │ │                                          │  │ 入场曲线   │  │
│ │装饰   │ │                                          │  │ 相机强度   │  │
│ │文字加工│ │                                          │  │ Seed       │  │
│ │背景   │ │                                          │  └───────────┘  │
│ │相机   │ │                                          │                 │
│ │画面效果│ ├──────────────────────────────────────────┤  ┌───────────┐  │
│ │转场   │ │         音频波形时间轴                      │  │ 🎲 换一个  │  │
│ └───────┘ │  ┌节拍轨 ─────────────────────────────┐   │  │ 只换此镜头 │  │
│           │  ├歌词轨 ─────────────────────────────┤   │  │ 🔒 锁定    │  │
│  [搜索]   │  ├分镜轨 ─────────────────────────────┤   │  └───────────┘  │
│  [标签筛选]│  └特效轨 ─────────────────────────────┘   │                 │
│           │     波形 · 频谱 · 拖拽打点 · K 值分割       │                 │
└───────────┴──────────────────────────────────────────┴─────────────────┘
```

### 打轴交互（照搬 Aegisub 肌肉记忆）

| 交互 | 行为 |
|---|---|
| 波形拖选 | 横向拖出选区 = 设定行/镜头的起止 |
| 逐字分割 | 选中歌词行后按 `Ctrl+K` 进入 K 值模式，波形上逐音节打点（等价 `audio_karaoke.cpp`） |
| 吸附 | 可切换吸附到「节拍 / 小节 / 逐字起点 / 关闭」 |
| 空格 | 播放/暂停 |
| `I` / `O` | 设定入点 / 出点 |
| `←` `→` | 上一帧 / 下一帧（按住 Shift = 逐节拍） |
| `Ctrl+←/→` | 上一镜头 / 下一镜头 |
| 滚轮 | 时间轴缩放；`Ctrl+滚轮` 缩放到当前选区 |
| `R` | 🎲 一键换构图（整体或当前镜头） |
| 拖入 | 音频/视频/图片放入时间轴 |

**必须做到**：波形缩放要跟 Aegisub 一样有多级金字塔缓存（分钟级概览 → 采样级精修），
否则 4 分钟的歌缩放时会明显卡顿。参考 `libaegisub/audio/` 的分块缓存策略。

### 演出部件库

分类沿用 JIZURA 已验证的 10 类（布局 / 登场 / 保持 / 退场 / 装饰 / 文字加工 / 背景 / 相机 / 画面效果 / 转场），
每个部件是一个 `{ id, name, tags, params schema, render(ctx, t) }` 的注册单元：

- **tags 驱动随机**：`Glitch / 抒情 / Pop / Graphic / Editorial / Horror / 和风 / 文字PV系 / 动能系`
  一键换构图时只从当前氛围标签匹配的部件池里抽样 —— 保证「随机但气场不乱」
- **params schema 驱动检查器 UI**：部件声明参数类型（数值/颜色/枚举/曲线），检查器自动生成控件，不用为每个部件手写 UI
- **进度目标**：P1 阶段先实现每个分类 8~12 个部件（共约 100 个），覆盖 80% 常用演出；后续按 `docs/PART_AUTHORING.md` 持续扩充

---

## 5. 渲染与导出

### 5.1 视频导出

```
for each frame i:
    t = i / fps
    renderFrame(t)              # 前端纯函数
    gl.readPixels() → Uint8Array
    → 攒够 N 帧批量过 channel
Rust: 写入 ffmpeg -f rawvideo -pix_fmt rgba -s WxH -r FPS -i - ... out.mp4
```

- 并行：多 worker 渲染不同帧区间，各自写临时分片，最后无损 concat
- **增量渲染**：比对 `.sf` 的镜头哈希，只重渲变化的区间
- 编码器：复用岚珠工具箱的 ffmpeg；硬件加速（NVENC/QSV/AMF）走岚珠已有的探测逻辑

### 5.2 动图导出（本项目的一个天然优势）

**关键洞察**：术曲PV 本身就是 2~3 帧定格风格，所以输出 GIF 时**降帧到 8~12fps 几乎不损失原味**，
体积却能砍掉 60%+。这是别的项目做不到的——普通视频降帧会有明显卡顿感，而文字PV不会。

| 格式 | 处理方式 |
|---|---|
| **GIF** | ffmpeg 两遍：先 `palettegen`（从全片采样）生成 256 色全局调色板，再 `paletteuse=dither=sierra2_4a`。`stats_mode=diff` 处理渐变 |
| **APNG** | `ffmpeg -plays 0`，保留 24/32 位色，无 GIF 的色带问题，体积约为同画质 GIF 的 1/3 |
| **WebP 动图** | 质量/体积比最优，社交平台兼容性好 |
| **序列 PNG** | 带 alpha，供 AE / 岚珠进一步处理 |
| **透明/绿幕** | 供后期合成的素材通道 |

---

## 6. 与岚珠工具箱等现有软件的联动

### 6.1 工具链：**联动，但不绑定**（⛔ 架构硬约束）

> **这是两个独立的软件。** 联动只意味着「发现到就复用」，
> **绝不意味着「依赖它才能跑」**。岚珠工具箱被卸载、迁移、改名，
> 或它的 `tools/` 被清空，本软件都只是少一个兜底选项，功能必须完全不受影响。

据此定了三条硬规矩，已实现在 `app/src-tauri/src/tools.rs`：

**① 移植，不调用。** 把岚珠的查找逻辑**抄过来**（约 200 行 Rust），
而不是依赖它的 crate 或进程。两边各改各的，互不影响。

**② 自己的目录永远排在借用目录之前。**

| 优先级 | 目录 | 归属 |
|---|---|---|
| 1 | `SHIZUKUFLOW_TOOLS` 环境变量 | 自己的 |
| 2 | 设置里手动指定（想借用就在这儿显式指） | 自定义 |
| 3 | exe 同级 `tools/`、上两级 `tools/` | 自己的 |
| 4 | `%APPDATA%\ShizukuFlow\tools` | 自己的 |
| 5 | `%APPDATA%\LanzhuTool\tools` | **借用（可选兜底）** |
| 6 | 都没有 → exe 同级 `tools/`（占位） | — |

**③ 禁止硬编码任何指向岚珠仓库/安装位置的绝对路径。**

全部外部借用点集中在**一个常量**里，一眼看清耦合面，也方便整体断开：

```rust
const BORROWED_DIRS: [(&str, &str); 1] = [("LanzhuTool", "tools")];
//                     ↑ AppData 下的目录名，不是绝对路径
```

同理**不读 `LANZHUTOOL_TOOLS` 环境变量** —— 那是岚珠的内部配置，可以指向任何地方。
真要借用，用户在设置里显式指定即可。

找到工具后**按文件名递归索引**（所以 `tools/video/ffmpeg/ffmpeg.exe` 这种嵌套结构能直接用），
扫一次整体缓存，不污染工作目录。

**约束已固化成单元测试**（防回归，改坏了当场拦下）：

| 测试 | 拦什么 |
|---|---|
| `no_hardcoded_absolute_paths` | 把 `D:\Project\...` 之类写回来 |
| `no_reference_to_other_project_env_vars` | 去读别人的环境变量 |
| `borrowed_dirs_lists_only_appdata_relative_locations` | 借用点写成绝对路径 |
| `resolve_survives_when_borrowed_dir_gone` | 借用目录没了就崩 |

> 📌 **待确认**：岚珠工具箱是否暴露 CLI 或 deep link（如 `lanzhutool://`）。
> 若有，可做到「一键跳转到岚珠继续压制」；若没有，则退化为导出文件 + 打开输出目录。
> **无论哪种，功能不可用都必须优雅降级，不能卡住主流程。**

### 6.2 数据通道

| ShizukuFlow 输出 | 岚珠工具箱消费方式 |
|---|---|
| FFV1 / ProRes 无损母版 | 视频页转码成最终压制格式 |
| PNG 序列（带 alpha） | 常用页「单图转视频」或 AVS 脚本 |
| `.ass` 字幕 | 自动匹配同名字幕，封装进成品 |
| 纯音频 + 时间轴 | 音频页转码 |

### 6.3 生态内其他软件

- **RinaDown**（Tauri 下载器）：素材/音源获取
- **ShiorikoTrans**（Tauri 翻译）：歌词翻译后导入 `.sf` 的 `lyrics`
- 联动方式统一为：**文件交换 + 可选的 CLI/deep link 唤醒**，避免做成进程间硬耦合

---

## 7. 开发路线图

### P0 — 地基（约 2 周）
- [ ] Tauri 2 + React 19 + Tailwind v4 工程骨架（直接抄 LanzhuToolNX 的 `tauri.conf.json` / vite 配置 / 国内镜像 `.npmrc`）
- [ ] `.sf` / `.sfp` 的 Rust 数据模型（serde）+ 读写 + 版本迁移框架
- [ ] 音频解码与波形金字塔缓存（Rust 侧解码 → 峰值数据 → 前端绘制）
- [ ] 时间轴四轨（节拍/歌词/分镜/特效）与拖选打轴交互
- [ ] 逐字 K 值打点模式
- [ ] 工具链定位模块（移植岚珠的查找逻辑）

**P0 出口标准**：能载入一首歌，在波形上打出带逐字时间戳的歌词并存成 `.sf`。

### P1 — 渲染与演出（约 3 周）
- [ ] `renderFrame(t)` 纯函数框架 + Canvas2D 绘制层
- [ ] WebGL2 合成与后处理层（暗角/色差/glitch/扫描线/颗粒/半调）
- [ ] 部件注册系统与参数 schema 驱动的检查器
- [ ] 首批 ~100 个部件（10 类 × 8~12）
- [ ] BPM 自动检测 + 手动踩点 + 吸附
- [ ] 🎲 一键换构图（seed + 标签约束抽样）+ 锁定/只换此项
- [ ] 实时预览（含 2/3 帧定格模拟）

**P1 出口标准**：能用部件拼出一段 30 秒的术曲风格演出并实时预览。

### P2 — 导出（约 2 周）
- [ ] 抓帧 → `readPixels` → 二进制 channel → ffmpeg 管道
- [ ] MP4 导出（含音轨）+ 硬件加速
- [ ] 多 worker 并行渲染 + 分片拼接
- [ ] GIF（palettegen/paletteuse 两遍）/ APNG / WebP 动图
- [ ] PNG 序列 / 透明通道 / 绿幕素材
- [ ] 增量渲染（镜头哈希比对）
- [ ] 导出任务队列与进度（可抄岚珠的任务栏进度条做法）

**P2 出口标准**：导出一段 1080p30 的成品 MP4 与一个体积合理的 GIF。

### P3 — 联动与打磨（约 2 周）
- [ ] 「发送到岚珠工具箱」通道
- [ ] `.ass` 双向导入导出（兼容已有压制流程）
- [ ] 工程模板与预设
- [ ] 自动保存 / 崩溃恢复
- [ ] 撤销重做栈（含音频回退）

### P4 — 进阶（持续）
- [ ] 2.5D 空间感（参考 `sihaiwuzhou-pv` 的带纹理平面 + 镜头穿行）
- [ ] 竖屏原生重排版（16:9 与 9:16 双向自动重排，非裁切）
- [ ] Whisper 自动歌词对齐（参考 `sihaiwuzhou-pv` 的 `align_lyrics.py`）
- [ ] 部件市场 / 用户自定义部件脚本
- [ ] 批处理 CLI（无头渲染，便于自动化）

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **抓帧性能瓶颈** | 4K60 导出过慢甚至不可用 | `readPixels` + 二进制 channel（绝不用 toDataURL）；多 worker 并行；提供 1/2 分辨率预览档 |
| **中文字体渲染** | 字形缺字、竖排、CJK 断行错乱 | 字体子集化加载；CJK 断行规则单独实现（`sihaiwuzhou-pv` 有踩坑经验）；提供霞鹜文楷/朱雀仿宋等开源字体兜底 |
| **波形缩放卡顿** | 打轴手感崩掉，直接违背"像 Aegisub"的初衷 | 多级峰值金字塔缓存（P0 就要做对，后期改代价大） |
| **部件系统过度设计** | 陷入无止境的部件扩充 | 严格限定 P1 只做 100 个，按"覆盖 80% 常用演出"收敛；其余走 P4 用户自定义 |
| **与岚珠耦合过深** | 两边互相牵制，升级就断 | 坚持文件交换为主的松耦合，工具链共享仅复用"查找逻辑"，不依赖岚珠的内部 API |
| **`.sf` 格式早期定版过早** | 后期加字段导致兼容性地狱 | P0 就引入 `version` + 迁移框架；所有可选字段走 `#[serde(default)]` |

---

## 9. 命名与约定

- 工程代号：**ShizukuFlow**（与主目录一致）
- 扩展名：`.sf`（JSON 工程）、`.sfp`（zip 打包）
- 主题色建议：沿用虹咲应援色体系（岚珠用 `#F69992`），ShizukuFlow 可取**樱坂雫**的应援色，与项目名呼应
- 许可：建议与生态一致走 **Apache-2.0**（LanzhuToolNX）或 MIT

---

## 10. 下一步

计划确认后，建议按这个顺序启动：

1. **先啃 Aegisub 的波形实现**（`src/audio_display.cpp` + `libaegisub/audio/`），把 P0 最容易踩坑的波形缓存一次做对
2. 定稿 `.sf` schema（写成 Rust struct + JSON Schema，前后端共用一份定义）
3. 搭骨架，先跑通「载入音频 → 打轴 → 存 `.sf`」这条最短闭环

需要我现在就把 `.sf` 的 Rust 数据模型和 JSON Schema 写出来吗？

---

## 进度日志

### 2026-10-30（续）— P0 音频引擎落地 ✅

**ffmpeg 改为自带（用户决策）**
用户指出 ffmpeg 就是个自包含二进制，ShizukuFlow 自己装一份最干净。已落实：
- 下载 BtbN 静态构建的 ffmpeg/ffprobe 到 `app/src-tauri/tools/`（约 170MB/份，已 `gitignore` 不入库）
- 编写 `tools/fetch_ffmpeg.ps1` 可复现脚本（用 `curl.exe --ssl-no-revoke` 跳过沙箱证书吊销检查）
- `tauri.conf.json` 的 `bundle.resources` 声明打包时附带 ffmpeg/ffprobe
- 解析器命中自带目录时来源标记为 `Own`；借用岚珠仅作可选兜底，且代码里硬编码对方路径的防回归测试依旧守住
- 实测：`%APPDATA%\LanzhuTool` 下只有 settings.json 没有 tools，印证"不能指望别人目录"；自备后完全自洽

**音频解码 + 波形金字塔（P0 核心，已实测）**
- `src/tauri/src/audio.rs`：用自备 ffmpeg 把音轨解码为单声道 44.1k s16 PCM（落磁盘缓存，按文件指纹失效），构建 256→262144 样本的多级 (min,max) 峰值金字塔
- 视图聚合 `peaks(t0,t1,width)` 无论缩放到什么比例，耗时只与画布宽度成正比——这是"像 Aegisub"的手感基石
- 峰值缓存存 `Arc<Pyramid>`，避免每次取波形 clone 21MB PCM
- 4 条单元测试覆盖：峰值捕获、长度匹配、极致放大回退、空 PCM 安全

**端到端验证**
- ffmpeg 解码实测：3.0s 测试音频 → 精确 132300 样本 → 3.0s 时长
- 24 个 Rust 单元测试全过（含 tools/sf/audio），cargo check 仅一条与代码无关的文件系统锁警告
- 前端 `Waveform.tsx` 接入真实引擎：打开音频 → `audio_load` 解码建塔 → `audio_peaks` 取数，支持滚轮缩放（以光标为锚）、点击/拖动刮擦、播放头、自适应刻度尺

### 下一步（P1 渲染演出）
P0 地基已全部就绪（骨架/格式/工具链/主题/音频）。下一步进入 P1：Canvas2D 绘制 + WebGL2 合成引擎，逐字卡拉OK timing，部件系统（背景/文字/装饰/转场）。

### 2026-10-30（续2）— 多声道音频 + P1 渲染预览闭环 ✅

**多声道（回应「一定要单声道吗」）**
用户指出单声道丢声场信息、不利创作。已把音频引擎从单声道升级为**保留原始声道数**：
- `audio.rs`：`Pyramid` 改为每声道独立金字塔，新增 `ChannelView`（mix/left/right/mid/side）
- 默认 `mix` 维持 Aegisub 手感；`mid`/`side` 给创作后门（人声居中、伴奏拉宽）
- 导出永远 mux 原文件，不二次编码，绝不丢立体声（已在代码注释钉死）
- 2 条双声道单测 + 26→28 总测试通过

**P1-M1 渲染预览闭环（帧 = f(t) 纯函数）**
- 新增 `src/lib/renderFrame.ts`：给定 (project, t) 画出一帧，**与导出共用同一入口**，保证预览即所得
  - 背景 + 当行歌词 + **逐字卡拉OK 填充**（ghost 轮廓 + 已唱字 sweep + 正在唱字强调色下划）
  - letterbox 保持 16:9，字号按画幅等比，样式走 `Style`（字体/字重/字号/描边/阴影/字距）
- 新增 `src/components/Preview.tsx`：画布按播放头出帧
- 新增后端 `project_demo` 命令：吐一段 4 行歌词 + 4 镜头、覆盖 0–12s 的演示工程（兼作回归 fixture）
- `App.tsx`：加「示例工程」按钮 + ▶/⏮ 播放控制 + 视觉播放循环（墙钟推进、循环于时长）
- 前端 `npm run build` 通过，Rust `cargo check` 零硬错误（仅无关文件锁警告）

**环境坑（已知）**：`npm run build` 在清理 `dist` 时会触发 node-safe-delete 的"回收站"shim 在本机失败；当前绕法是先 `rm -rf dist`。已记入，待后续在 vite 配置或 CI 处理。

### 下一步
- P1-M2：部件系统（背景装饰/转场/运镜），在 `renderFrame(t)` 上叠 `PartRef`
- P1-M3：在波形上直接打逐字 K 时间轴（Aegisub 的 karaoke 模式），喂给 `lyric.syllables`
- P2：导出管线（ffmpeg 抓帧 → 编码 MP4/GIF/APNG），接实时音视频同步

### 2026-10-30（续3）— Qt 内核出片 + 缓存缺陷修复 ✅

**先更正状态：本文档落后于实际。** 渲染内核已按「Qt 骨架 / 内核心脏 / WebView 衣服」迁到 C++：
`qt-shell/src/core/` 下 `lume.cpp`(31KB, Draw IR + 11 缓动 + 8 算子 + 字形 LRU) +
`sf_render.cpp`(56KB, TS 版 `renderFrame.ts` 的完整移植) + `sf_project.cpp`。功能面
（背景11/文字/卡拉OK逐字/装饰6/overlay5/转场4/相机）已与 TS 版对齐，连续播放 avg 6.55ms/帧。

**修掉三个缓存缺陷（都在 `sf_render.cpp`）**
1. 噪点层用「距上次烘焙多久」判定刷新，来回 seek 会反复重烘同一档位 —— 改成与背景一致的
   `bgBucket` 0.1s 分桶。
2. 噪点槽**从不写 `lastUsed`**（恒为 0），一旦插入就被 LRU 当作最久未用；且噪点走的是插入
   不淘汰的路径，40 个镜头能把 16 槽的背景池整个顶掉 —— 噪点独立成池（8 槽），两条路径共用
   `evictIfNeeded`。
3. `Draw::Bitmap` 每帧 `make_shared<QImage>` 浅拷贝槽图 —— 槽里直接存 `shared_ptr<QImage>`，
   绘制时引用计数 +1，零拷贝。

实测背景缓存条目从超限的 28 收敛到 16；全片抽样 avg 15.20→14.84/15.06ms（噪声内，无回退）。

**新增：内核直出 MP4**
`sf_dump --export <out.mp4> [--fps N] [--size WxH] [--range t0 t1] [--ffmpeg path]`
逐帧 `renderInto` 复用缓冲 → RGBA8888 → **管道直写 ffmpeg stdin**，不落盘、不过 IPC。
对比 Tauri 版「前端画 → 每帧 JPEG 写磁盘 → ffmpeg 读回来」省掉两次往返。

实测（`余韻` 20–30s，1280×720@30，crf18 veryfast）：300 帧 4.68s = **64.1 帧/秒，2.14x 实时**，
体积 0.8MB；抽帧与内核直出参考帧 **PSNR 46.16 dB**（视觉无损）。

**音轨 mux（同日补）**
`--export` 现在默认挂上工程 `media.audio.path`，可用 `--audio <path>` 覆盖、`--no-audio` 静音。
- 相对路径按 **.sf 所在目录** 解析；命令行 `--audio` 给的路径按 **当前工作目录** 解析（两者语义不同，混用会找不到文件）
- `--range` 的起始时间以输入侧 `-ss` 施加到音频，与视频帧同一时间点起算
- 音频统一 `-ar 48000`：源是 96kHz FLAC 时，高采样率 AAC 播放器兼容性差、平台侧照样重采样

实测（`余韻` 20–30s，1280×720@30）：
- 音轨挂载正确，AAC 48kHz 立体声，视频 10.000s / 音频 9.984s（差 16ms，AAC 帧对齐）
- **声画同步核对**：输出首秒 `mean_volume` = **-12.7 dB**，与源音轨 20–21s 完全一致（源首秒是 -20.2 dB）→ `-ss` 精确生效
- **全片导出**（213.37s / 6402 帧 / 1280×720）：68.43s 完成 = **93.6 帧/秒，3.12x 实时**，22.8 MB，音视频时长 213.3667 / 213.3733s

**遗留**
- 导出仍是单线程；`libx264` 已用 18 线程，但渲染与编码可再拆成生产者/消费者。
- 还差 GIF/APNG/WebP、硬件编码（nvenc/qsv/amf）分支。
- `CanvasDef::safeAreaMargin` 已解析但预览安全框写死 5%，两者未接线。

### 2026-10-31 — 导出多格式 + 并行分片 + ASS 字幕 ✅

**导出格式按扩展名分派**（`sf_dump --export <out>`，同一套渲染循环）：
- `.mp4/.mov` → H.264 + AAC（原路径）
- `.gif` → `split` 单遍调色板（`palettegen:stats_mode=diff` + `paletteuse:bayer`），不带音轨
- `.webp` → libwebp 动画（`-q:v 82 -loop 0`）
- 目录 / 无扩展名 → PNG 序列 `frame_00000.png`（给 AviUtl/AE 继续加工）

实测（`余韻` 30–34s，640×360）：GIF 60 帧 0.45s / 3.3MB；WebP 48 帧 0.23s / 0.7MB；
PNG 序列 30 帧 4.79s / 9.5MB。GIF 的瓶颈从编码转到了**渲染**（132 帧/秒）。

**并行分片导出**：`--export out.mp4 --jobs N`
- 按整数帧号切片 → 各起一个 `sf_dump` 子进程渲染 `--no-audio` 分段 → `concat` demuxer 不重编码拼接 → 最后单独 mux 音轨
- 选进程而非线程：内核的字体/图片/视频帧缓存共享，做成线程安全要付全局锁代价；进程崩了也能只重跑该片
- 上界减半帧（`t1 = start + (b_{k+1} - 0.5)/fps`）保证 `ceil` 恰好切出 n 帧
- **帧时刻钉到 fps 网格**（`t = round((start + i/fps)*fps)/fps`）：切片起点非 0 时，`i/fps` 的浮点误差会让「恰好落在切点」的那一帧时而算进前一个镜头（画出场闪白）、时而算进后一个镜头。钉网格后串行/并行/任意切片算出同一个 double
- 回归：串行 71.51s vs 4 路并行 58.26s（全片 6402 帧）；PSNR 平均 **58.53 dB**，仅末帧 29.66 dB（`-shortest` 收尾差一帧）

**ASS 字幕导出**（`src/core/sf_ass.{h,cpp}`，`sf_dump --ass out.ass`）：
- 逐字时间戳直接转 `{\k}`，`Style` 行由 `styles[]` 映射（字号按画布高归一到 1080）
- `an5 + \pos(W/2, H*style.y)` 复现渲染里的落点，`{`/`}`/换行 做转义
- UTF-8 BOM + `QSaveFile` 原子写（Aegisub 对无 BOM 中文靠猜；写到一半崩掉不留残件）

**增量导出**：`--jobs N --cache <dir>`
- 分片指纹 = `{canvas, styles, fps, 尺寸, 帧号区间, 落在区间内的 shots[], lyrics[]}` 的 SHA-1
- 改一个镜头只让覆盖它的分片失效：40s 片段实测 全渲染 12.06s / 全命中 1.24s / 只改 S015 命中 3/4 得 6.34s
- 先写 `.part` 再改名，中断的缓存文件不会被当成成品读回
- **缓存产物与现渲产物 PSNR = inf（逐字节一致）**，这是缓存能开的前提
- 缓存目录必须转绝对路径：拼接清单落在临时目录，ffmpeg 按清单所在目录解析相对路径

**LRC 双向**（`src/core/sf_lrc.{h,cpp}`，`--lrc` / `--lrc-import`）
- 支持 `[mm:ss.xx]`、`[mm:ss:xx]`、一行多时间标签、`[ti/ar/offset]`，以及增强格式 `<mm:ss.xx>` 拆逐字
- 两位小数=厘秒、三位=毫秒（`[00:01.5]` 不是 5 毫秒）；UTF-8 严格失败回退系统 ANSI（中文 Windows = GBK）
- 余韻 37 行往返：行起点/逐字起点误差 ≤5ms，文本与逐字数量完全一致
- 行尾按「下一行起点」推（.lrc 没有结束时间），末行兜底 +4s

**音频分析**（`src/core/sf_audio.{h,cpp}`，`--audio [--peaks-png]`）
- ffmpeg 解 4kHz 单声道 s16 → 100 桶/秒 min/max 峰值（时间轴直接画，替代原来的伪波形）
- BPM 用归一化自相关 + 抛物插值 + 轻微节奏先验；**原来用「总和/剩余样本数」做分母，长 lag 占便宜，会把 130 听成 65**
- 余韻实测：BPM 129.71 / 首拍 0.170s / 置信度 2.47（工程里写的 `bpm: 79` 是早期估算，未改）

**审片工具**：`--still <t> [out.png]`、`--contact out.png [--cols/--rows/--cell/--from/--to]`、
`--thumbs <dir> --every 2`（107 张 3.2s，文件名 `thumb_<毫秒>.png`）

**遗留**
- 分片并行受内存带宽限制，10 核机器上 4 路只拿到 1.2–1.7x（每片独占时 83 帧/秒，4 片同跑降到 33 帧/秒）
- 硬件编码分支（nvenc/qsv/amf）仍未做
- ASS 只导出不导入
- `余韻.sf` 的 `bpm: 79` 与实测 129.71 不符，镜头卡点没压在真实拍上；要修得重新生成整片
