# 任务书 · hy3 第二单 —— 导出容器的前端半段

> 单轮交付。工作目录 `d:/Project/ShizukuFlow/app`。
> **只读两个文件**：`src/lib/output.ts`、`src/lib/exporter.ts`。
> **Rust 侧由主控实现，你一行都不要碰 `src-tauri/`。**

## 背景（已给齐，不要探索）

- `output.ts` 定义导出设置，目前 `container: 'mp4'` 是唯一值。
- `exporter.ts` 的 `exportVideo()` 已能导出 MP4：逐帧渲染 → `toBlob('image/jpeg')` → 落盘 → 调 `export_mux` 让 ffmpeg 合成。
- 主控正在给 `export_mux` 加 `format` 参数，支持 gif / apng / webp。
- 你这单只负责：**让前端知道要出什么格式、按格式决定帧图片类型与降档参数**。

## 交付一：`src/lib/output.ts` 追加（不改已有函数）

```ts
export type Container = 'mp4' | 'gif' | 'apng' | 'webp'

/** 从输出路径推断容器；认不出就当 mp4。大小写不敏感。 */
export function containerFromPath(outPath: string): Container

/** 容器扩展名（不带点） */
export function containerExt(c: Container): string

/** 只有 mp4 能带音轨 */
export function containerHasAudio(c: Container): boolean

/** 帧图片类型：mp4/gif → image/jpeg；apng/webp → image/png（要 alpha） */
export function containerFrameMime(c: Container): 'image/jpeg' | 'image/png'
```

并把 `OutputSettings.container` 的类型 `'mp4'` 改成 `Container`。

实现要求：

- `containerFromPath` 取最后一个 `.` 之后的部分，`toLowerCase()` 后精确匹配 `gif`/`apng`/`webp`，其余一律 `'mp4'`。
- `.apng` 是四字母扩展名，**不要**用"取最后几个字符"这类写法。

## 交付二：`src/lib/exporter.ts` 四处改动

### 1. `ExportOptions` 增加字段

```ts
  /** 输出容器；缺省按 opts.outPath 扩展名推断 */
  container?: Container
```

### 2. 函数开头：确定容器 + GIF 降档

在 `const fps = ...` 之后插入：

```ts
  const container = opts.container ?? containerFromPath(opts.outPath)
```

再插入降档块（**只有 gif 生效**）：

```ts
  // GIF 是 256 色索引格式：降帧到 12fps、宽限 640。
  // 文字 PV 本身是定格风格，这一步几乎不损观感，体积却能砍掉一大半。
  let outFps = fps
  let outW = W
  let outH = H
  if (container === 'gif') {
    outFps = Math.min(fps, 12)
    if (W > 640) {
      outW = 640
      outH = Math.max(2, Math.round(((H * 640) / W) / 2) * 2)
    }
  }
```

**这是本单最容易做错的地方**，四处必须同步改，漏一处时长就错几倍：

| 现在 | 改成 |
|---|---|
| `const total = Math.max(1, Math.ceil(dur * fps))` | `... Math.ceil(dur * outFps))` |
| 画布 `canvas.width = W` / `canvas.height = H` | `outW` / `outH` |
| `const t = start + i / fps` | `start + i / outFps` |
| 铺底色 `fillRect(0, 0, W, H)` | `fillRect(0, 0, outW, outH)` |
| `renderFrame(ctx, W, H, ...)` | `renderFrame(ctx, outW, outH, ...)` |

### 3. 帧编码：mime 与扩展名跟着容器走

把 `toBlob(res, 'image/jpeg', 0.94)` 那段改成：

```ts
    const mime = containerFrameMime(container)
    const ext = mime === 'image/png' ? 'png' : 'jpg'
    const blob = await new Promise<Blob | null>((res) =>
      canvas.toBlob(res, mime, mime === 'image/jpeg' ? 0.94 : undefined),
    )
```

写盘文件名改成 `` `f${String(i).padStart(6, '0')}.${ext}` ``。

### 4. `invoke('export_mux', ...)` 参数

增加一项 `format: container`（Rust 已实现该参数）。

音频改成：

```ts
    audioPath: containerHasAudio(container) ? audio : null,
```

## 交付三：测试

新建 `src/lib/__tests__/output.test.ts`，用例照抄：

| 输入 | 期望 |
|---|---|
| `containerFromPath('a.gif')` | `'gif'` |
| `containerFromPath('C:\\x\\y.APNG')` | `'apng'` |
| `containerFromPath('a.webp')` | `'webp'` |
| `containerFromPath('a.mp4')` | `'mp4'` |
| `containerFromPath('无扩展名')` | `'mp4'` |
| `containerFromPath('a.tar.gz')` | `'mp4'` |
| `containerHasAudio('mp4')` | `true` |
| `containerHasAudio('gif')` | `false` |
| `containerFrameMime('gif')` | `'image/jpeg'` |
| `containerFrameMime('apng')` | `'image/png'` |
| `containerExt('apng')` | `'apng'` |

## 验收（自己跑，贴真实输出）

```bash
cd d:/Project/ShizukuFlow/app
npx tsc --noEmit
npx vitest run
```

零错误、全绿。跑挂就把真实报错贴回来，别猜着改范围外的代码。

## 禁止

- **不许碰 `src-tauri/` 下任何文件**
- 不许改 `renderFrame.ts`、`parts.ts`、`api.ts`、`store.ts`、任何 `.tsx`
- 不许改 `output.ts` 已有的 `QUALITY_PRESETS` / `estimateBytes` / `humanBytes` / `defaultOutPath`
- 不许加依赖、不许改 `package.json`
- 不许做超出本单的"顺手优化"

## 回传

1. 每处改动的文件与行号
2. 两条验收命令的真实输出
3. 一句话确认：GIF 的 `total` 用的是 `outFps` 而非 `fps`
