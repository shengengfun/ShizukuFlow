# 任务书 · hy3 —— 歌词编辑命令层

> 单轮交付任务。工作目录 `d:/Project/ShizukuFlow/app`。
> 你有用量上限，所以本文已把需要的事实全给齐了：**不要再做全库探索**，只需读文末列出的 3 个文件。

## 目标

给 ShizukuFlow 补上「歌词增删改 + 逐字 K 值」的**纯逻辑层**。不写任何 UI（UI 由主控接）。

## 已给定事实（不用再查）

- `app/src/lib/api.ts` 已定义：`Lyric = { id, start, end, text, syllables: Syllable[], style }`，`Syllable = { text, start, end }`（秒）。
- `app/src/state/commands.ts` 是**可序列化编辑命令**层，当前只有 4 条命令：`shot.replace` / `canvas.safe` / `media.audio` / `patches`。
- 该层有**硬约束**：命令载荷必须无损 JSON 往返（`app/src/state/__tests__/commands.test.ts` 守着）。
- 状态层是手写 class（不是 zustand），`project.lyrics` 是数组。
- 目前**没有任何**歌词编辑相关命令，这是最大空洞。

## 交付物（只允许动这两个文件 + 两个测试文件）

### 1. 新建 `app/src/lib/lyrics.ts`

纯函数。禁止 import store / React / DOM / 任何新依赖。

```ts
export function splitSyllables(text: string): string[]
export function distributeEvenly(text: string, start: number, end: number): Syllable[]
export function retimeBoundary(syls: Syllable[], boundary: number, t: number): Syllable[]
export function shiftLyric(l: Lyric, dt: number): Lyric
export function clampSyllables(syls: Syllable[], start: number, end: number): Syllable[]
```

行为规格（这是验收判据，逐条实现）：

1. `splitSyllables`：CJK（汉字/假名/谚文）逐字切分；ASCII 连续字母数字整体成词；空白不产生音节；标点并入前一个音节（开头标点并入后一个）。
2. `distributeEvenly`：按切分个数把 `[start, end]` 等分；**末个音节的 end 精确等于 end**（不靠累积加法，用 `start + (i+1)*dur/n` 算边界，避免浮点漂移）。
3. `retimeBoundary`：把「第 boundary 个音节之后的分界点」移到 `t`。只改相邻两个音节的边界，其余不变；`t` 越界时吸附到 `[syls[boundary].start, syls[boundary+1].end]`；保证单调不减。`boundary` 或 `boundary+1` 越界时原样返回。
4. `shiftLyric`：整行与所有音节同时平移 `dt`，`start` 不小于 0（若平移后会为负，整体夹到 0 并保持内部相对间距）。
5. `clampSyllables`：把音节裁剪进 `[start, end]`，丢掉完全落在区间外的音节，被截断的音节收到边界上。

**所有函数不得修改入参**，返回新数组/新对象。

### 2. 修改 `app/src/state/commands.ts`

在 `EditCmd` 联合类型里新增 4 条，并按文件现有模式补齐 `handlers`、`coalesceKeyOf`、`isEditCmd`、`COMMANDS`：

```ts
| { t: 'lyric.add'; lyric: LyricJson }
| { t: 'lyric.remove'; lyricId: string }
| { t: 'lyric.replace'; lyric: LyricJson }
| { t: 'lyric.syllables'; lyricId: string; syllables: Syllable[] }
```

语义：

- `lyric.add`：插入后 `project.lyrics` 按 `start` 升序（**稳定排序**，同 start 保持原相对顺序）。
- `lyric.remove` / `lyric.replace` / `lyric.syllables`：**找不到 id 时静默返回，不改动 draft**，绝不抛错。
- `lyric.syllables`：写入音节的同时，把该行 `start`/`end` 对齐到首音节 `start` 与末音节 `end`。
- `lyric.replace`：整体替换同 id 的那一行（保持数组位置不变）。
- `label` 用中文短语：`歌词 ${id}` / `逐字 K 值`。
- `coalesceKeyOf`：`lyric.syllables` 返回 `lyric:${lyricId}:syl`（连续拖动合并成一条历史）；其余返回 `undefined`。

### 3. 测试

- 新建 `app/src/lib/__tests__/lyrics.test.ts`：覆盖中英混排切分、均分末位精确对齐、`retimeBoundary` 越界吸附与单调性、入参不可变（调用后 deep-equal 原数组）。
- 在 `app/src/state/__tests__/commands.test.ts` 里补：4 条新命令的 JSON 往返、找不到 id 的静默语义、`lyric.add` 的稳定排序。

## 照抄级示例（实现时逐条对照，错一条就是没通过）

**`splitSyllables`**

| 输入 | 期望 |
|---|---|
| `"我还记得"` | `["我","还","记","得"]` |
| `"Hello world"` | `["Hello","world"]` |
| `"dawn 的颜色"` | `["dawn","的","颜","色"]` |
| `"！真的吗？"` | `["！真","的","吗？"]` |
| `"   "` | `[]` |

（规则：开头标点并入后一个音节，结尾标点并入前一个音节。）

**`distributeEvenly("我还", 0, 1)`**

期望 `[{text:'我',start:0,end:0.5},{text:'还',start:0.5,end:1}]`。
边界必须用 `start + dur * (i + 1) / n` 直接算，**不许累积加法** —— 否则末位会出现 `0.9999999`。

**`retimeBoundary`**（入参 `[{a,0,1},{b,1,2}]`）

| boundary | t | 期望 |
|---|---|---|
| 0 | 1.5 | `[{a,0,1.5},{b,1.5,2}]` |
| 0 | 9 | `[{a,0,2},{b,2,2}]`（吸附到 `syls[1].end`） |
| 0 | -9 | `[{a,0,0},{b,0,2}]`（吸附到 `syls[0].start`） |
| 5 | 1.5 | 原样返回（但必须是**新数组**） |

**`shiftLyric`**（入参 `{start:1,end:2,syllables:[{x,1,1.5},{y,1.5,2}]}`，`dt=-3`）

期望 `{start:0,end:1,syllables:[{x,0,0.5},{y,0.5,1}]}` —— 负向平移时整体夹到 0，内部间距不变。

**`clampSyllables`**（入参 `[{a,0,1},{b,1,2},{c,2,3}]`，区间 `[0.5, 2.5]`）

期望 `[{a,0.5,1},{b,1,2},{c,2,2.5}]`。

## 验收（必须自己跑，把真实输出贴回）

```bash
cd d:/Project/ShizukuFlow/app
npx tsc --noEmit
npx vitest run
```

两行都必须零错误、全绿。

## 禁止

- 不许改 `api.ts`、`store.ts`、`renderFrame.ts`、任何 `.tsx` 文件。
- 不许引入新依赖、不许改 `package.json`。
- 不许「顺手优化」无关代码；diff 里出现无关改动即视为不通过。

## 回传格式

1. 改动文件清单（路径 + 每个文件新增行数）
2. 上面两条命令的**真实终端输出**
3. 你选择的切分规则里，对「英文单词 + 中文混排」和「纯标点行」这两个边界的处理结论各一行
