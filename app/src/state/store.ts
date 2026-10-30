/**
 * 工程状态层 —— 三层架构的第二层（UI 壳层 / 工程状态层 / LUME 渲染内核）。
 *
 * 这是**权威工程的所有者**。UI 与渲染内核都只是它的订阅者：
 *
 *   UI  ──dispatch(Command)──▶  ProjectStore  ──订阅通知──▶  UI
 *                                   │
 *                                   └──renderFrame(project, t)──▶ LUME 内核
 *
 * 硬约束（对应重构报告 §3.1 / §3.2）：
 *   1. UI 不得直接改写工程 —— 只能通过 dispatch 提交命令。
 *   2. 撤销/重走使用命令模式 + **增量 diff**，禁止全量快照。
 *      常见操作（改一个参数）只产生一条叶子级补丁。
 *   3. 本文件**零框架依赖**（不 import React / @tauri-apps / DOM）。
 *      所以它可以今天住在 WebView 里、明天整体搬进原生宿主进程，
 *      而不需要改一行逻辑 —— 这就是拆层的目的。
 *
 * 崩溃恢复：每次提交都会产出一条 `HistoryEntry`（含 before/after 补丁），
 * App 把它追加写进 journal 文件，配合定期快照即可「快照 + 按序重放补丁」恢复。
 * 落盘这件事本身交给宿主去做，本层不碰 IO —— 保持边界干净。
 */

import type { Project } from '../lib/api'
import { assertSerializable, coalesceKeyOf, resolve, type EditCmd } from './commands'

// ─────────────────────────── 路径与补丁 ───────────────────────────

export type Key = string | number

export type Patch =
  /** 叶子级/实体级赋值：把 path 处的值设为 after（撤销时设回 before）。 */
  | { kind: 'set'; path: Key[]; before: unknown; after: unknown }
  /** 数组整体替换（增删改序时退化为此形式，仍是单个数组而非整个工程）。 */
  | { kind: 'array'; path: Key[]; before: unknown[]; after: unknown[] }

export interface HistoryEntry {
  id: number
  label: string
  at: number
  patches: Patch[]
  /** 合并键相同的相邻命令会被合并成一条（滑块拖动不能产生几百条历史）。 */
  coalesceKey?: string
}

/** UI 只读的工程视图 —— 明确不暴露可写句柄。 */
export interface StoreSnapshot {
  project: Project | null
  canUndo: boolean
  canRedo: boolean
  undoLabel: string | null
  redoLabel: string | null
  /** 自上次保存以来是否有未落盘的改动。 */
  dirty: boolean
  version: number
}

export type StoreEvent = 'init' | 'reset' | 'command' | 'undo' | 'redo' | 'save'

export interface DispatchOptions {
  /** 相邻同键命令在窗口期内合并（用于滑块/拖拽这类连续编辑）。 */
  coalesceKey?: string
  coalesceMs?: number
}

/** 一次编辑＝一个纯函数：吃进草稿，改完出来。不做 IO，不读外部状态。 */
export interface EditCommand {
  label: string
  apply: (draft: Project) => void
  coalesceKey?: string
}

// ─────────────────────────── 纯工具 ───────────────────────────

/**
 * 深冻结：让"快照里的工程改不动"。
 *
 * 为什么必要：`snapshot()` 如果只是把内部对象原样递出去，UI 组件拿到引用就能
 * `project.shots[0].seed = 1` 直接改写 —— dispatch 形同虚设，权威状态又回到 UI 手里。
 * 所以对外一律发投影：读可以，写会抛。
 *
 * 成本可控：只在工程真正变化时冻结一次并缓存，不是每次通知都重来。
 */
export function deepFreeze<T>(o: T): T {
  if (!o || typeof o !== 'object') return o
  if (Object.isFrozen(o)) return o
  Object.freeze(o)
  for (const k of Object.keys(o as object)) {
    deepFreeze((o as Record<string, unknown>)[k])
  }
  return o
}

const clone = <T>(v: T): T =>
  typeof structuredClone === 'function'
    ? structuredClone(v)
    : (JSON.parse(JSON.stringify(v)) as T)

const isPlainObject = (v: unknown): v is Record<string, unknown> =>
  !!v && typeof v === 'object' && !Array.isArray(v)

/** 不可变写入：沿 path 克隆出一条新链，把末端设为 value。 */
function setIn<T>(root: T, path: Key[], value: unknown): T {
  if (path.length === 0) return value as T
  const [head, ...rest] = path
  if (Array.isArray(root)) {
    const next = root.slice()
    next[head as number] = setIn(root[head as number], rest, value)
    return next as unknown as T
  }
  const obj = (root ?? {}) as Record<string, unknown>
  return { ...obj, [head as string]: setIn(obj[head as string], rest, value) } as T
}

const eq = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b)

/**
 * 增量 diff：产出从 a 变到 b 所需的最小补丁集。
 * 只有确实变了的子树才会出现在补丁里（常见情况是一条叶子）。
 */
export function diff(a: unknown, b: unknown, path: Key[] = [], out: Patch[] = []): Patch[] {
  if (a === b) return out
  if (Array.isArray(a) && Array.isArray(b)) {
    if (eq(a, b)) return out
    // 元素带 id 且 id 序列不变 → 逐元素下钻，产出实体级补丁
    const keyed = a.every(isPlainObject) && b.every(isPlainObject)
    if (keyed && a.length === b.length) {
      const sameIds = a.every((x: Record<string, unknown>, i) => {
        const y = b[i] as Record<string, unknown>
        return x.id !== undefined && x.id === y.id
      })
      if (sameIds) {
        for (let i = 0; i < a.length; i++) diff(a[i], b[i], [...path, i], out)
        return out
      }
    }
    out.push({ kind: 'array', path, before: a, after: b })
    return out
  }
  if (isPlainObject(a) && isPlainObject(b)) {
    for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
      diff(a[k], b[k], [...path, k], out)
    }
    return out
  }
  out.push({ kind: 'set', path, before: a, after: b })
  return out
}

function applyPatch(p: Patch, project: Project, dir: 'undo' | 'redo'): Project {
  if (p.kind === 'array') return setIn(project, p.path, dir === 'undo' ? p.before : p.after)
  return setIn(project, p.path, dir === 'undo' ? p.before : p.after)
}

// ─────────────────────────── Store ───────────────────────────

const DEFAULT_HISTORY_LIMIT = 200

export class ProjectStore {
  private project: Project | null = null
  private undoStack: HistoryEntry[] = []
  private redoStack: HistoryEntry[] = []
  private listeners = new Set<(s: StoreSnapshot, e: StoreEvent) => void>()
  private seq = 0
  private version = 0
  private dirty = false
  /** 上一次提交的合并键与时间，用于判断能否并入同一条历史。 */
  private last: { key: string; at: number; entry: HistoryEntry } | null = null
  /** 对外发布的只读投影，随 version 失效重算。 */
  private frozen: Project | null = null
  private frozenVersion = -1

  constructor(private historyLimit = DEFAULT_HISTORY_LIMIT) {}

  // ── 读 ──

  get(): Project | null {
    return this.project
  }

  /** 对外只读投影。UI 拿到它可以渲染，但改写会在严格模式下抛错。 */
  read(): Project | null {
    if (!this.project) return null
    if (this.frozenVersion !== this.version || this.frozen === null) {
      this.frozen = deepFreeze(this.project)
      this.frozenVersion = this.version
    }
    return this.frozen
  }

  snapshot(): StoreSnapshot {
    return {
      project: this.read(),
      canUndo: this.undoStack.length > 0,
      canRedo: this.redoStack.length > 0,
      undoLabel: this.undoStack.at(-1)?.label ?? null,
      redoLabel: this.redoStack.at(-1)?.label ?? null,
      dirty: this.dirty,
      version: this.version,
    }
  }

  /** 最近的历史记录（给「编辑历史」面板用，最新在最后）。 */
  history(): readonly HistoryEntry[] {
    return this.undoStack
  }

  redos(): readonly HistoryEntry[] {
    return this.redoStack
  }

  // ── 订阅 ──

  subscribe(fn: (s: StoreSnapshot, e: StoreEvent) => void): () => void {
    this.listeners.add(fn)
    return () => this.listeners.delete(fn)
  }

  private emit(e: StoreEvent) {
    const snap = this.snapshot()
    for (const fn of this.listeners) fn(snap, e)
  }

  // ── 生命周期 ──

  /** 载入工程（打开/新建）。历史清空 —— 跨工程的撤销没有意义。 */
  reset(project: Project | null) {
    // 接管所有权：内部克隆一份。既不冻结调用方手里的对象，
    // 也防止调用方事后改写自己那份而绕过 dispatch。
    this.project = project ? clone(project) : null
    this.frozen = null
    this.frozenVersion = -1
    this.undoStack = []
    this.redoStack = []
    this.last = null
    this.dirty = false
    this.version++
    this.emit(project ? 'reset' : 'init')
  }

  /** 标记已落盘，清掉 dirty。 */
  markSaved() {
    this.dirty = false
    this.emit('save')
  }

  // ── 写：唯一入口 ──

  /**
   * 提交一次编辑。这是改写工程的**唯一合法途径**。
   * @param cmd 纯函数式的编辑描述
   * @returns 是否真的产生了变化（无变化时不会产生历史记录，避免污染撤销栈）
   */
  dispatch(cmd: EditCommand, opts: DispatchOptions = {}): boolean {
    if (!this.project) return false
    const before = this.project

    const draft = clone(before)
    cmd.apply(draft)
    if (eq(before, draft)) return false // 空编辑不入历史

    const patches = diff(before, draft, [])
    const now = Date.now()
    const key = opts.coalesceKey ?? cmd.coalesceKey
    const windowMs = opts.coalesceMs ?? 600

    let nextProject: Project = draft
    let entry: HistoryEntry | null = null

    // 合并：同一控件在窗口期内连续编辑 → 合成一条历史
    const canMerge =
      key !== undefined &&
      this.last !== null &&
      this.last.key === key &&
      now - this.last.at <= windowMs &&
      this.undoStack.at(-1) === this.last.entry

    if (canMerge && this.last) {
      const prev = this.last.entry
      // before 取更早的、after 取更新的，合并成一条历史
      const merged = mergePatches(prev.patches, patches)
      entry = { ...prev, at: now, patches: merged }
      this.undoStack[this.undoStack.length - 1] = entry
      this.last = { key, at: now, entry }
    } else {
      entry = { id: ++this.seq, label: cmd.label, at: now, patches, coalesceKey: key }
      this.undoStack.push(entry)
      if (this.undoStack.length > this.historyLimit) this.undoStack.shift()
      this.last = key !== undefined ? { key, at: now, entry } : null
    }

    // 任何新编辑都会让重做栈失效
    this.redoStack = []
    this.project = nextProject
    this.dirty = true
    this.version++
    this.emit('command')
    return true
  }

  /**
   * 提交一条**可序列化命令**（三方架构的唯一合规入口）。
   *
   * 与 `dispatch` 的区别：这里收的是纯数据命令（`EditCmd`），
   * 所以它既能被 WebView 面板提交，也能被 Qt 宿主 / 崩溃恢复流程提交 ——
   * 函数闭包的 `dispatch` 过不了进程边界，长期应当全部收敛到这里。
   */
  dispatchCmd(cmd: EditCmd, opts: DispatchOptions = {}): boolean {
    // 先把关：载荷里混了函数 / NaN / Date 就应该当场失败，
    // 而不是等命令走到 Qt 宿主才发现内容静默丢失。
    assertSerializable(cmd)
    const { label, apply } = resolve(cmd)
    return this.dispatch(
      { label, apply, coalesceKey: coalesceKeyOf(cmd) },
      { coalesceMs: opts.coalesceMs, coalesceKey: opts.coalesceKey },
    )
  }

  undo(): boolean {
    const entry = this.undoStack.pop()
    if (!entry || !this.project) return false
    let next = this.project
    // 逆序还原：后写的先撤，避免同一路径父子补丁互相干扰
    for (let i = entry.patches.length - 1; i >= 0; i--) {
      next = applyPatch(entry.patches[i], next, 'undo')
    }
    this.project = next
    this.redoStack.push(entry)
    this.last = null
    this.dirty = true
    this.version++
    this.emit('undo')
    return true
  }

  redo(): boolean {
    const entry = this.redoStack.pop()
    if (!entry || !this.project) return false
    let next = this.project
    for (const p of entry.patches) next = applyPatch(p, next, 'redo')
    this.project = next
    this.undoStack.push(entry)
    this.last = null
    this.dirty = true
    this.version++
    this.emit('redo')
    return true
  }

  clearHistory() {
    this.undoStack = []
    this.redoStack = []
    this.last = null
    this.emit('save')
  }
}

/**
 * 合并两条补丁集时不能直接 concat：同一路径上会留下「中间态」的重复项，
 * 撤销时按逆序逐个还原反而会写错值。做法是先按路径去重（保留最早的 before），
 * 再补上新增路径。
 */
function mergePatches(prev: Patch[], next: Patch[]): Patch[] {
  const byPath = new Map<string, Patch>()
  for (const p of [...prev, ...next]) {
    const k = p.kind + ':' + p.path.join('.')
    const exist = byPath.get(k)
    if (!exist) {
      byPath.set(k, p)
      continue
    }
    // 同一路径：before 取更早的，after 取更新的
    if (exist.kind === 'set' && p.kind === 'set') {
      byPath.set(k, { kind: 'set', path: p.path, before: exist.before, after: p.after })
    } else if (exist.kind === 'array' && p.kind === 'array') {
      byPath.set(k, { kind: 'array', path: p.path, before: exist.before, after: p.after })
    } else {
      byPath.set(k, p)
    }
  }
  return [...byPath.values()]
}
