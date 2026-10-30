# ShizukuFlow — 文字PV / 术曲风格 PV 工具集

> 面向 **ボカロ / V家手书 / 文字PV** 风格的制作工具与引擎收集。
> 收录标准：文字是主角、逐格卡点、可程序化渲染 —— 而非"字幕 + 背景图"式歌词视频。
> 收录时间：2026-10-30 · 全部 `git clone --depth 1`

> 📌 **本目录同时是「ShizukuFlow 软件」的研发资料库。**
> 目标是一个像 Aegisub 一样直观的术曲文字PV 制作软件，原生工程格式 `.sf`(JSON)，
> 打包格式 `.sfp`(zip)，输出视频与动图，联动岚珠工具箱。
> **完整实施计划见 [`PLAN.md`](PLAN.md)。**
>
> 下方「参考资产」一栏中的 `Aegisub/`、`karaoke-templates/`、`vocaloid-style-mv-pipeline/`
> 是软件研发的直接参考对象，其余为选型调研素材。

## 一眼选型

| 需求 | 用哪个 |
|---|---|
| 想马上出片、零安装 | **JIZURA**（浏览器直接打开 `index.html`） |
| 中文界面 + AI Agent 协同 | **JIZURA-CN** |
| 想做完整的 4 分钟术曲 MV | **vocaloid-style-mv-pipeline** ⭐ |
| 实时演出 / 直播背景 | **pv-tool**、**jizura-sync** |
| 已有 AviUtl2 工作流 | **JIZURA-AviUtl2** |
| 想自己写引擎 | **sihaiwuzhou-pv**（2.5D moderngl+skia）、**Chamina**（Rust） |
| 学术 / 扩散模型路线 | **KineTy** |

---

## 一、主力工具

### 1. `JIZURA/` — 852wa/JIZURA ★2145 · MIT
**最正统的文字PV自动构成工具。** 输入歌词 → 自动组装分镜 → 浏览器导出 MP4。

- **860 个演出部件** + 27 种风格（樱花 / 深海 / 夕烧 / 报纸 / 蒸汽波 / 恐怖废墟…）
- 部件分类：布局 184、登场 125、保持 52、退场 109、装饰 130、文字加工 62、背景 66、相机 36、画面效果 69、转场 27
- 按 `R` 一键随机：风格 + 氛围 + 动态 + 配色 + 构图整体换，**带氛围标签约束的抽样**，不是瞎随机
- 7 种基础氛围 + 恐怖系（监视画面、门缝、手电筒、涂黑字、雪花噪点，52 种效果）
- 导出：16:9 / 9:16 / 4:3 / 3:4 / 1:1 / 4:5 / 21:9、720p~4K、24/30/60fps（含 2/3 帧定格「文字PV感」）
- 格式：MP4（含音轨）/ 序列 PNG / 透明 PNG / **绿幕·黑幕**（便于后期合成）
- 音频：自动检测 BPM 与节拍；空格键手动踩点；时间轴拖拽
- 歌词记法：`/` 切分镜、`*强调*`、`!` 闪光摇曳、`|` 注释、`[01:23.45]` 时间戳、`[间奏 8]`
- **AE 面板**：`JIZURA_AE.jsx`（脚本版）/ `JIZURA_CEP.zip`（CEP 版）。导出 JSON 进 AE 后**文字仍是可编辑文本图层**，动画由表达式驱动，不是死视频
- 纯前端零依赖（Python + Node 构建脚本，无 npm 包），靠 WebCodecs 出 H.264 MP4，分块写入不怕长视频爆内存，GPU 编码失败自动回退软编
- 自动识别日/中/韩/英歌词并映射 Google Fonts；界面 7 语切换
- 输出物版权归制作者，商用自由。**歌词与曲的版权另计**

```
# 本地跑：直接浏览器打开 JIZURA/index.html 即可
python3 build.py            # 从 src/ app/ 重新构建 index.html
```

### 2. `JIZURA-CN/` — yahnhaagendaz/JIZURA
JIZURA 的中日双语 Agent-Ready 分支，针对中文创作者痛点改造：
- 中日双语热切换、专业排版与 AE 术语本地化
- 3-2-1 节拍倒计时预备、**0.5x / 0.75x 变速慢放打点**（快歌从容对点）
- ↶ 撤销重做（智能回退音频 2.5s）、独立视频轨时间轴与留白标定
- 规范化 JSON 数据契约，支持本地 HTTP / CLI 无头渲染批处理
- 工作流：Agent 歌词解析 → 镜头编排 → AE 脚本导出

### 3. `vocaloid-style-mv-pipeline/` — EGSECDA ★233 · MIT ⭐ **最强对口**
**这就是为术曲风格量身定做的。** 给 AI Agent 一首歌 + 歌词，产出成片级歌词 MV。

- **前端动画 = 每帧是歌曲时间 `t` 的纯函数**，所以可复现、可并行渲染、可分段重渲
- Canvas2D 绘制 + WebGL2 合成 → Playwright 抓帧 → ffmpeg 编码。无 AE、无时间轴编辑器
- **内置 JIZURA 字面层**做歌词演出，另有 Blender toon-3D 分镜脚本
- 一次产出：1920×1080 母版 + **原生重排版的 1080×1920 竖屏版**（不是裁切）+ 分享码率
- 结构：`skills/vocaloid-style-mv/`（SKILL.md 方法论）+ `template/`（引擎 + 场景库 + 音频分析 + 逐字对齐 + 渲染器）
- 流程：listen → concept → assets → storyboard → build → review → deliver
- 示例「残光」：3:46、68 个卡点镜头、RTX 3080 上 **≈4 分钟渲完 6796 帧**（3 workers, ≈31fps）
- 为 Claude Code 写，但纯 Markdown + 脚本，Codex 等其他 Agent 也能跑

---

## 二、演出引擎 / 实时

### 4. `pv-tool/` — DanteAlighieri13210914/pv-tool ★456 · ⚠️ **非商业许可**
PixiJS 8 + TypeScript 的实时视觉特效引擎，16 套预设 + 54 种可配置效果。
模板名很懂行：**斩击、蓝色冲击、蓝色构成、城市文字雨、青墨、全息、赛博监控、戒尺、歇斯底里之夜**…
- 图层：Background（纹理/渐变/三角网格/棋盘）、Decoration（几何形/放射线/透视网格/黄金螺旋/三分构图）、Text（hero/散落/字条/字卡/描边/发光/竖排副文本/公式雨）、Overlay（暗角/色差/glitch/扫描线/胶片颗粒/半调/HUD）、Motion（实时运动检测跟踪框）
- 音频 BPM 驱动动画与镜头；支持拖入图片/视频作背景并自动取色；HiDPI 原生渲染

> ⚠️ **许可重要**：因 vtblive 商业滥用且拒绝协商，作者自 **2026/3/24 起从 AGPL-3.0 改为 Non-Commercial License**。
> 个人使用、教学研究、非商业同人项目免费；**任何商业用途（含付费视频制作、直播、SaaS）必须向作者购买商业授权**。克隆学习没问题，商用前务必确认。

### 5. `jizura-sync/` — Saqoosha ★12
把 JIZURA 接到 Spotify 上做**实时歌词演出** —— 播什么歌就实时渲染什么，适合直播背景 / 可视化。

### 6. `JIZURA-AviUtl2/` — KinuX2 ★14
JIZURA 的非官方 AviUtl2 移植版。输入歌词选演出，直接在 AviUtl2 里做 motion graphics，适合已有 AviUtl 工作流的圈子。

---

## 三、自建引擎 / 研究

### 7. `sihaiwuzhou-pv/` — whenpoem ★3 · **中文案例，值得精读**
《四海五洲》文字 PV 的完整渲染代码（TOPKINGCREAM 词曲 / 星尘原唱，140 BPM，3:19.77）。全片程序生成。
- **自研 2.5D 引擎**（`scripts/engine/`）：字、纸、布、墙、翻板、剪影全是三维空间里带纹理的平面，镜头在平面间平移推拉穿行；静止看着是二维，动起来才有纵深
- OpenGL 4.3（Python moderngl）+ skia 栅格化 / 矢量轮廓填充
- HDR 分组渲染：「过去」加胶片颗粒暖色光晕、「今天」干净锐利，再合成
- 全片九段（`seg_a1`~`seg_h`），各提供 `frame(t)`；交接画面集中在 `film/handoff.py`，可分段并行制作
- 工具链：`separate.py`（Demucs 人声/鼓分离）、`analyze_audio.py`（节拍响度）、`transcribe*.py`+`align_lyrics.py`（Whisper 逐字对齐）、`timing.py`（逐字时间+鼓点）、`fetch_images.py`（Wikimedia 素材+授权记录）

### 8. `Chamina/` — StellavacuaLuxumbra ★2
Rust 文字 MV 渲染库，支持 JavaScript 脚本驱动演出。
⚠️ 作者自述：**未完成、无可用 GUI、性能极差**，仅供研究参考。

### 9. `KineTy/` — SeonmiP ★59
ECCV 2024《Kinetic Typography Diffusion Model》官方代码。扩散模型路线，研究向，非生产力工具。

---

## 四、配套 / Agent 集成

| 目录 | 仓库 | 说明 |
|---|---|---|
| `jizura-helper-skill/` | Zaious ★2 | 让 AI 助手把"你描述的画面感"直接转成 JIZURA 配置文件的 Agent Skill |
| `dsh-tool-jizura/` | Flooding-Rain ★1 | 调用 JIZURA 在线应用，歌词文本 → 文字PV（MP4 / PNG 序列）的工具插件 |
| `jpv-lyrics-motion-kit/` | donbeeshyvt-jpg ★0 | 日系 PV 文字动态模板集，**兼容 Remotion** |

---

## 许可速查

| 项目 | 许可 | 商用 |
|---|---|---|
| JIZURA / JIZURA-CN | MIT | ✅ 自由（输出物版权归你） |
| vocaloid-style-mv-pipeline | MIT | ✅ |
| sihaiwuzhou-pv | 见仓库 | 需自查素材授权 |
| pv-tool | **Non-Commercial** | ❌ 需购买授权 |
| KineTy | 见仓库 | 需自查 |
| 其余 | 见各仓库 LICENSE | — |

> 通用提醒：工具本身授权 ≠ 歌曲与歌词的授权。使用他人曲目的音频与歌词始终受各自权利人约束。

## 五、参考资产（ShizukuFlow 软件研发用）

| 目录 | 仓库 | 参考什么 |
|---|---|---|
| `Aegisub/` | TypesettingTools/Aegisub ★2035 | **交互手感的唯一参考**。`audio_box.cpp` `audio_display.cpp`（波形打轴）、`audio_karaoke.cpp`（逐字 K 值）、`ass_karaoke.cpp`、`async_video_provider.cpp`、`ass_override.cpp`。`libaegisub/audio/` 的峰值金字塔缓存是波形不卡的关键 |
| `karaoke-templates/` | Seekladoom ★228 | 496 个 `.ass` 特效模板（在 `ASS Template/`，另有若干 rar 合集）。演出效果的现成语料库，用于归纳部件分类与参数体系 |
| `Aegisub-Motion/` | TypesettingTools ★210 | 运动跟踪数据驱动合成的 Lua 实现 |

> Aegisub 主仓库用 `--no-recurse-submodules` 克隆（34M），避免拉下大量子模块。

---

## 备注

本机 `git clone` 走 https 时 schannel 报 `CRYPT_E_NO_REVOCATION_CHECK`（沙箱无法做证书吊销检查），
已通过 `GIT_SSL_NO_VERIFY=1` 绕过。若后续在本目录再克隆，同样加这个环境变量即可。
