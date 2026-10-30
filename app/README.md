# ShizukuFlow — 应用程序

> 像 Aegisub 一样直观的术曲风格文字 PV 制作软件。
> 原生工程格式 `.sf`（JSON），打包格式 `.sfp`（zip），输出视频与动图。
>
> 完整实施计划见上级目录的 [`../PLAN.md`](../PLAN.md)，参考资料见 [`../README.md`](../README.md)。

## 技术栈

Tauri 2 + React 19 + Tailwind v4 + Rust —— 与岚珠工具箱 / ShiorikoTrans / RinaDown 同一套，
配置和镜像（`.npmrc`、`src-tauri/.cargo/config.toml`）都从岚珠那边取的同一份。

## ⛔ 与岚珠工具箱的关系：联动，但不绑定

**这是两个独立的项目。** 岚珠被卸载、迁移、改名，本软件功能必须完全不受影响。

三条硬规矩（实现在 `src-tauri/src/tools.rs`，有单元测试守着）：

1. **移植，不调用** —— 把查找逻辑抄过来，不依赖它的 crate 或进程。
2. **自己的目录永远排在借用目录之前**（`SHIZUKUFLOW_TOOLS` → 手动指定 → 自带 → 借用）。
3. **禁止硬编码任何指向岚珠的绝对路径**，也不读它的环境变量。

外部借用点全部集中在一个常量里，方便一眼看清耦合面、也方便整体断开：

```rust
const BORROWED_DIRS: [(&str, &str); 1] = [("LanzhuTool", "tools")];
```

防回归测试（改坏了当场拦下）：

| 测试 | 拦什么 |
|---|---|
| `no_hardcoded_absolute_paths` | 把 `D:\Project\...` 之类写回生产代码 |
| `no_reference_to_other_project_env_vars` | 去读别人的环境变量 |
| `borrowed_dirs_lists_only_appdata_relative_locations` | 借用点写成绝对路径 |
| `resolve_survives_when_borrowed_dir_gone` | 借用目录没了就崩 |

## 目录结构

```
app/
├── index.html              默认深色：给 <html> 预置 .dark，避免首帧闪白
├── src/
│   ├── globals.css         设计令牌层（Tailwind v4 @theme + :root / .dark）
│   ├── App.tsx             主界面
│   ├── lib/
│   │   ├── appearance.ts   主题 + 强调色（虹咲 13 人配色，默认樱坂雫 #01B7ED）
│   │   └── api.ts          后端命令封装（类型与 Rust 侧逐字段对应）
│   ├── components/         （P1）
│   ├── pages/              （P1）
│   └── state/              （P1）
├── src-tauri/
│   ├── src/
│   │   ├── main.rs         入口
│   │   ├── cmd.rs          Tauri 命令层
│   │   ├── sf/mod.rs       .sf / .sfp 数据模型与读写
│   │   └── tools.rs        工具链定位（含解耦约束与测试）
│   └── icons/              由 tools/make_icon.py 生成
└── tools/
    └── make_icon.py        图标生成器（手写 PNG/ICO，无第三方依赖）
```

## 开发

```bash
cd app
npm install
npm run tauri dev      # 开发
npm run tauri build    # 出 NSIS 安装包
```

`npm run dev` 可以纯浏览器预览界面样式（端口 1420）。
⚠️ 与岚珠工具箱共用 1420，两台不要同时 dev。

## 工程格式要点

`.sf` 是 JSON，`.sfp` 是 zip（`project.sf` + `assets/` + `manifest.json`）。
三条硬约束：

1. **所有可选字段 `#[serde(default)]`** —— 后加字段不能让老工程打不开。
2. **随机必须固化种子**（`Shot::seed`）—— 否则 `renderFrame(t)` 的可复现性崩塌。
3. **部件参数用 `serde_json::Value`** —— 部件是开放集合，检查器按 schema 渲染控件。

## 当前进度（P0 骨架）

- [x] Tauri 2 + React 19 + Tailwind v4 骨架，构建通过
- [x] `.sf` / `.sfp` 数据模型与读写（含 zip 打包、sha1 校验、版本迁移框架）
- [x] 工具链定位（含解耦约束与 4 条防回归测试）
- [x] 主题系统：明暗双主题，默认深色，强调色樱坂雫 `#01B7ED`
- [x] 图标：水蓝圆角底 + 白色水滴
- [ ] 音频解码与波形金字塔缓存
- [ ] 时间轴四轨与打轴交互
- [ ] 渲染引擎 `renderFrame(t)`
- [ ] 导出管线

## 测试

```bash
cd src-tauri
cargo test                                  # 20 个单元测试
cargo test probe_real_env -- --nocapture --ignored
#                                            ↑ 真实环境体检：打印工具目录与来源
```
