/**
 * 可序列化编辑命令 —— Qt 骨架 / LUME 内核 / WebView 面板三方共用的词汇表。
 *
 * 为什么必须有这一层：
 *   三方跨进程（Qt 主进程 ⇄ WebView 渲染进程 ⇄ 内核进程），**函数无法跨进程序列化**。
 *   `store.dispatch({ label, apply: (draft) => {...} })` 里的闭包在今天很好用，
 *   但它过不了任何进程边界 —— 一旦 WebView 面板要向 Qt 宿主提交编辑，就断了。
 *
 * 所以命令必须是纯数据：JSON.stringify 过去、另一头 parse 回来照做。
 * 这层也是将来把状态层整体搬进 Rust/C++ 宿主时唯一需要重写的东西，
 * UI 与内核因此可以各自演进。
 *
 * 硬约束：任何命令的载荷必须能通过 JSON 往返（不允许 undefined / 函数 / Map / 类实例）。
 * 这条由 `commands.test.ts` 里的往返测试守着。
 */

import type { Project, Shot } from '../lib/api'
import type { Patch } from './store'

export const PROTOCOL_VERSION = 1

/** JSON 值的类型刻画 —— 用来在类型层面卡住「不能塞进命令里的东西」。 */
export type Json = string | number | boolean | null | Json[] | { [k: string]: Json }

export type EditCmd =
  /** 整体替换一个镜头（检查器改完、部件库落位后的结果都走这条）。 */
  | { t: 'shot.replace'; shotId: string; shot: ShotJson }
  /** 开/关画面安全框。 */
  | { t: 'canvas.safe'; enabled: boolean }
  /** 挂载音轨。 */
  | { t: 'media.audio'; path: string; duration: number }
  /** 直接下发补丁（崩溃恢复重放、跨端同步用；UI 日常不该走这条）。 */
  | { t: 'patches'; patches: Patch[] }

/** Shot 在工程里本就是纯数据结构，这里只是把类型收窄到「可 JSON 化」。 */
export type ShotJson = Shot & Json

export interface ResolvedCommand {
  label: string
  apply: (draft: Project) => void
}

// ───────────────────── 解析：命令数据 → 具体改动 ─────────────────────

type Handler<C> = (cmd: C) => ResolvedCommand

const handlers: { [K in EditCmd['t']]: Handler<Extract<EditCmd, { t: K }>> } = {
  'shot.replace': (c) => ({
    label: `镜头 ${c.shotId}`,
    apply: (d) => {
      const i = d.shots.findIndex((s) => s.id === c.shotId)
      if (i < 0) return
      d.shots = d.shots.map((s, k) => (k === i ? (c.shot as Shot) : s))
    },
  }),

  'canvas.safe': (c) => ({
    label: '画面安全框',
    apply: (d) => {
      d.canvas.safe_area = { ...d.canvas.safe_area, enabled: c.enabled }
    },
  }),

  'media.audio': (c) => ({
    label: '挂载音轨',
    apply: (d) => {
      d.media.audio = { path: c.path, sha1: '', duration: c.duration, kind: 'audio' }
    },
  }),

  patches: (c) => ({
    label: '重放补丁',
    apply: (d) => {
      for (const p of c.patches) assign(d, p.path, p.kind === 'array' ? p.after : p.after)
    },
  }),
}

function assign(root: Project, path: (string | number)[], value: unknown) {
  if (path.length === 0) return
  let cur: unknown = root
  for (let i = 0; i < path.length - 1; i++) {
    const k = path[i]
    cur = Array.isArray(cur) ? cur[k as number] : (cur as Record<string, unknown>)[k as string]
  }
  const last = path[path.length - 1]
  if (Array.isArray(cur)) cur[last as number] = value
  else (cur as Record<string, unknown>)[last as string] = value
}

/** 把一条命令翻译成「历史要记什么 + 草稿要改什么」。 */
export function resolve(cmd: EditCmd): ResolvedCommand {
  const h = handlers[cmd.t]
  if (!h) throw new Error(`未知的编辑命令类型：${(cmd as { t: string }).t}`)
  return (h as Handler<EditCmd>)(cmd)
}

/** 合并键：同一控件的连续拖动应当合成一条历史（命令数据与 UI 控件一一对应）。 */
export function coalesceKeyOf(cmd: EditCmd): string | undefined {
  switch (cmd.t) {
    case 'shot.replace':
      return `shot:${cmd.shotId}`
    case 'canvas.safe':
      return 'canvas.safe'
    case 'media.audio':
      return undefined
    case 'patches':
      return undefined
    default:
      return undefined
  }
}

// ───────────────────── 校验 / 往返 ─────────────────────

/**
 * 深度可序列化检查。
 *
 * 注意：不能只靠「JSON 往返后还能解析」来判断安全 ——
 * `JSON.stringify` 遇到**函数会静默丢键**、遇到 `NaN/Infinity` 会变成 `null`、
 * 遇到 `Date`/`Map`/类实例会降维或失真。这些都不会抛错，往返比较甚至还"通过"，
 * 但命令跨到对端时内容已经丢了 —— 这是最坏的失败方式（静默损坏）。
 *
 * 所以这里逐个叶子检查类型，返回第一个出问题的路径。
 */
export function serializabilityIssue(v: unknown, path = '$', depth = 0): string | null {
  if (depth > 64) return `${path}: 嵌套过深`
  if (v === null) return null
  switch (typeof v) {
    case 'string':
    case 'boolean':
      return null
    case 'number':
      // NaN / ±Infinity 经 JSON 会变成 null —— 静默失真
      return Number.isFinite(v) ? null : `${path}: ${String(v)} 无法用 JSON 表示`
    case 'undefined':
      return `${path}: undefined 会在跨端后消失`
    case 'function':
      return `${path}: 函数无法跨进程序列化`
    case 'symbol':
    case 'bigint':
      return `${path}: ${typeof v} 无法序列化`
    case 'object':
      break
    default:
      return `${path}: 未知类型 ${typeof v}`
  }
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) {
      const bad = serializabilityIssue(v[i], `${path}[${i}]`, depth + 1)
      if (bad) return bad
    }
    return null
  }
  // 只允许纯对象：Date / Map / Set / 类实例都会失真
  const proto = Object.getPrototypeOf(v) as object | null
  if (proto !== Object.prototype && proto !== null) {
    return `${path}: 非纯对象（${(v as object).constructor?.name ?? '?'}）会在跨端后失真`
  }
  for (const [k, val] of Object.entries(v as Record<string, unknown>)) {
    const bad = serializabilityIssue(val, `${path}.${k}`, depth + 1)
    if (bad) return bad
  }
  return null
}

/** 提交前把关：载荷里有任何跨不过边界的东西就当场炸，而不是到对端才发现丢了。 */
export function assertSerializable(cmd: EditCmd): void {
  const issue = serializabilityIssue(cmd)
  if (issue) throw new Error(`编辑命令不可跨端序列化：${issue}`)
}

/** 运行时守卫：任何命令都必须能无损地 JSON 往返。 */
export function isJsonRoundTrippable(value: unknown): boolean {
  try {
    const s = JSON.stringify(value)
    if (s === undefined) return false
    return JSON.stringify(JSON.parse(s)) === s
  } catch {
    return false
  }
}

export const COMMANDS = ['shot.replace', 'canvas.safe', 'media.audio', 'patches'] as const

/** 结构校验：跨端进来的命令不一定可信。 */
export function isEditCmd(v: unknown): v is EditCmd {
  if (!v || typeof v !== 'object') return false
  const c = v as Partial<EditCmd> & { t?: string }
  switch (c.t) {
    case 'shot.replace':
      return typeof (c as { shotId?: unknown }).shotId === 'string' && !!c.shot
    case 'canvas.safe':
      return typeof (c as { enabled?: unknown }).enabled === 'boolean'
    case 'media.audio':
      return (
        typeof (c as { path?: unknown }).path === 'string' &&
        typeof (c as { duration?: unknown }).duration === 'number'
      )
    case 'patches':
      return Array.isArray((c as { patches?: unknown }).patches)
    default:
      return false
  }
}
